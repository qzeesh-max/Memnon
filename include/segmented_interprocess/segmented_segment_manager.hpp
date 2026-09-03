// Copyright (C) 2026 Zeeshan Qazi
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

/// \file segmented_segment_manager.hpp
/// Multi-segment memory manager: routes allocations across sub-segments and
/// grows automatically by adding new sub-segments when exhausted.
///
/// Each sub-segment contains one Boost rbtree_best_fit segment_manager that
/// handles the actual free-list bookkeeping within that sub-segment.  The
/// segmented_segment_manager acts as a dispatch layer:
///
///   allocate(n)    → try each sub-segment in order; grow if all exhausted
///   deallocate(p)  → find owning sub-segment via trie; delegate to it
///   grow(n)        → add a new sub-segment of at least n bytes
///
/// Named objects (construct / find / destroy)
/// ───────────────────────────────────────────
/// Named objects are managed via a per-sub-segment Boost segment_manager.
/// find() does a linear scan across sub-segments (O(num_segments)).  For v1
/// this is acceptable; a global index can be added later.
///
/// Thread safety
/// ─────────────
/// Each Boost segment_manager uses an interprocess_mutex internally.
/// The sub-segment list (`segments_`) is guarded by `list_mtx_` (a
/// std::mutex — process-local, since this object is not in shared memory).
/// Trie lookups in allocate/deallocate are lock-free.

#pragma once

#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <iostream>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <unordered_map>
#include <functional>
#include <typeindex>

// Boost interprocess for the inner segment managers
#include <boost/interprocess/mem_algo/rbtree_best_fit.hpp>
#include <boost/interprocess/sync/mutex_family.hpp>
#include <boost/interprocess/indexes/iset_index.hpp>
#include <boost/interprocess/segment_manager.hpp>

#include "sub_segment.hpp"
#include "segment_registry.hpp"
#include "segmented_offset_ptr.hpp"
#include "segmented_allocator.hpp"
#include "prefetch_worker.hpp"
#include "shm_spinlock.hpp"

namespace segmented_interprocess {

// ============================================================================
// Inner Boost segment_manager type
// ============================================================================
// We parametrise Boost's rbtree_best_fit with our segmented_offset_ptr<void>
// as the void_pointer so that ALL internal free-list and rbtree node pointers
// use our encoding and can be correctly resolved after segment growth.

using si_void_ptr    = boost::interprocess::offset_ptr<void>;
using si_mutex_fam   = detail::shm_spin_mutex_family;
using si_alloc_algo  = boost::interprocess::rbtree_best_fit<si_mutex_fam, si_void_ptr>;
using si_seg_mgr     = boost::interprocess::segment_manager<
                            char, si_alloc_algo,
                            boost::interprocess::iset_index>;

// Minimum size needed to hold one segment_manager (padded with overhead)
inline constexpr std::size_t kMinSegmentSize  = 1u << 20; // 1 MiB
inline constexpr std::size_t kDefaultSegSize  = 16u << 20; // 16 MiB
inline constexpr std::size_t kGrowMultiplier  = 2u; // double size each grow

// ============================================================================
// segmented_segment_manager
// ============================================================================

class segmented_segment_manager {
public:
    // -----------------------------------------------------------------------
    // Types
    // -----------------------------------------------------------------------
    using size_type       = std::size_t;
    using void_pointer    = si_void_ptr;
    using segment_manager = si_seg_mgr;

    // -----------------------------------------------------------------------
    // Construction — anonymous (single-process)
    // -----------------------------------------------------------------------
    explicit segmented_segment_manager(size_type initial_size = kDefaultSegSize)
        : anon_(true), shm_root_name_()
    {
        add_segment_anon(
            std::max(initial_size, kMinSegmentSize),
            /*is_primary=*/true);
        prefetch_worker::instance().register_manager(this);
    }

    // -----------------------------------------------------------------------
    // Construction — named SHM (cross-process, creator)
    // -----------------------------------------------------------------------
    segmented_segment_manager(const std::string& root_name,
                              size_type initial_size = kDefaultSegSize,
                              bool creator = true)
        : anon_(false), shm_root_name_(root_name)
    {
        if (creator) {
            add_segment_shm(0, std::max(initial_size, kMinSegmentSize),
                            /*is_primary=*/true, /*is_creator=*/true);
            init_segment_table();
        } else {
            open_all_segments();
        }
        prefetch_worker::instance().register_manager(this);
    }

    ~segmented_segment_manager() {
        prefetch_worker::instance().unregister_manager(this);
        // Destroy segment_managers before munmapping
        std::unique_lock<std::shared_mutex> lk(list_mtx_);
        for (auto& info : segments_) {
            if (info.smgr)
                info.smgr->~segment_manager(); // explicit dtor, no dealloc
            segment_registry::instance().unregister_segment(info.seg.get());
        }
    }

    // Non-copyable
    segmented_segment_manager(const segmented_segment_manager&) = delete;
    segmented_segment_manager& operator=(const segmented_segment_manager&) = delete;

    // -----------------------------------------------------------------------
    // Raw allocation
    // -----------------------------------------------------------------------

    /// Allocate `bytes` contiguous bytes from any sub-segment.
    /// Grows if no sub-segment has sufficient free space.
    void* allocate(size_type bytes) {
        void* p = nullptr;
        {
            std::shared_lock<std::shared_mutex> lk(list_mtx_);
            p = try_allocate_locked(bytes);
        }
        if (!p) [[unlikely]] {
            grow(bytes + kMinSegmentSize);
            std::shared_lock<std::shared_mutex> lk(list_mtx_);
            p = try_allocate_locked(bytes);
        }
        if (!p) [[unlikely]]
            throw std::bad_alloc();
        return p;
    }

    /// Allocate without throwing; returns nullptr on failure.
    void* allocate(size_type bytes, std::nothrow_t) noexcept {
        try {
            void* p = nullptr;
            {
                std::shared_lock<std::shared_mutex> lk(list_mtx_);
                p = try_allocate_locked(bytes);
            }
            if (!p) [[unlikely]] {
                grow(bytes + kMinSegmentSize);
                std::shared_lock<std::shared_mutex> lk(list_mtx_);
                p = try_allocate_locked(bytes);
            }
            return p;
        } catch (...) { return nullptr; }
    }

    /// Deallocate a pointer obtained from allocate().
    /// Uses the trie to find the owning sub-segment.
    void deallocate(void* ptr) noexcept {
        if (!ptr) [[unlikely]] return;
        sub_segment* seg =
            segment_registry::instance().find(
                detail::ptr_to_vaddr(ptr));
        if (!seg) [[unlikely]] {
            assert(false && "deallocate: pointer not in any sub-segment");
            return;
        }
        segment_manager* smgr = smgr_for(seg);
        if (smgr) [[likely]]
            smgr->deallocate(ptr);
    }

    // -----------------------------------------------------------------------
    // Named object interface (process-local name registry)
    // -----------------------------------------------------------------------
    // Note: Named objects are managed with a process-local std::unordered_map
    // to avoid Boost named_proxy template complications.  For cross-process
    // usage, named objects must be re-discovered by the opening process using
    // find<T>(name) after reconstructing the map from the segment header.

    /// Allocate memory and construct a named object of type T.
    /// Returns nullptr if a named object with this name already exists.
    template<class T, class... Args>
    T* construct(const char* name, Args&&... args) {
        {
            std::shared_lock<std::shared_mutex> lk(list_mtx_);
            
            // Try to construct in existing segments
            for (auto& info : segments_) {
                try {
                    T* ptr = info.smgr->construct<T>(name)(std::forward<Args>(args)...);
                    if (ptr) return ptr;
                } catch (const boost::interprocess::bad_alloc&) {
                    // Not enough memory in this sub-segment, try next
                    continue;
                }
            }
        }
        
        // If we reach here, we need to grow
        grow(sizeof(T) + 1024); // Add some padding for the named object metadata
        
        std::shared_lock<std::shared_mutex> lk2(list_mtx_);
        // Try the newly added segment (which is the last one)
        try {
            return segments_.back().smgr->construct<T>(name)(std::forward<Args>(args)...);
        } catch (const boost::interprocess::bad_alloc&) {
            throw std::bad_alloc();
        }
    }

    /// Find a named object by name.  Returns {ptr, count} or {nullptr, 0}.
    template<class T>
    std::pair<T*, std::size_t> find(const char* name) const {
        std::shared_lock<std::shared_mutex> lk(list_mtx_);
        for (const auto& info : segments_) {
            auto res = info.smgr->find<T>(name);
            if (res.first) {
                return res;
            }
        }
        return {nullptr, 0};
    }

    /// Destroy a named object by name.  Calls its destructor and frees memory.
    template<class T>
    bool destroy(const char* name) {
        std::shared_lock<std::shared_mutex> lk(list_mtx_);
        for (auto& info : segments_) {
            if (info.smgr->destroy<T>(name)) {
                return true;
            }
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Capacity / introspection
    // -----------------------------------------------------------------------

    /// Total free bytes across all sub-segments.
        size_type get_free_memory_unlocked() const noexcept {
        size_type total = 0;
        for (const auto& info : segments_) total += info.smgr->get_free_memory();
        return total;
    }
    size_type get_size_unlocked() const noexcept {
        size_type total = 0;
        for (const auto& info : segments_) total += info.seg->size;
        return total;
    }

    size_type get_free_memory() const {
        std::shared_lock<std::shared_mutex> lk(list_mtx_);
        size_type total = 0;
        for (const auto& info : segments_)
            total += info.smgr->get_free_memory();
        return total;
    }

    /// Total capacity (mapped bytes) across all sub-segments.
    size_type get_size() const {
        std::shared_lock<std::shared_mutex> lk(list_mtx_);
        size_type total = 0;
        for (const auto& info : segments_)
            total += info.seg->size;
        return total;
    }

    /// Number of sub-segments currently in use.
    std::size_t segment_count() const {
        std::shared_lock<std::shared_mutex> lk(list_mtx_);
        return segments_.size();
    }

    // -----------------------------------------------------------------------
    // Explicit grow
    // -----------------------------------------------------------------------
    void grow(size_type min_bytes) {
        std::lock_guard<std::mutex> glk(grow_mtx_);
        size_type new_chunk_size = 0;
        size_type current_file_size = 0;
        {
            std::shared_lock<std::shared_mutex> lk(list_mtx_);
            if (get_free_memory_unlocked() >= min_bytes) return;
            new_chunk_size = std::max(min_bytes,
                segments_.empty() ? kDefaultSegSize
                                  : segments_.back().seg->size * kGrowMultiplier);
            new_chunk_size = detail::align_up(new_chunk_size, detail::page_size());
            current_file_size = get_size_unlocked();
        }
        if (anon_) {
            auto seg = sub_segment::create_anon(new_chunk_size);
            if (!seg) throw std::bad_alloc();
            std::unique_lock<std::shared_mutex> lk(list_mtx_);
            add_segment_anon_preallocated(std::move(seg), false);
        } else {
            std::size_t new_file_size = current_file_size + new_chunk_size;
            if (!detail::shm_grow(shm_root_name_.c_str(), new_file_size)) {
                throw std::bad_alloc();
            }
            std::unique_lock<std::shared_mutex> lk(list_mtx_);
            // is_creator = false (we just map a chunk, don't create file)
            // initialize_smgr = true (we are growing, so we initialize the new segment manager)
            add_segment_shm(current_file_size, new_chunk_size, false, false, true);
            update_segment_table();
        }
    }

    void grow_background() noexcept {
        std::unique_lock<std::mutex> glk(grow_mtx_, std::try_to_lock);
        if (!glk.owns_lock()) return;

        size_type new_chunk_size = 0;
        size_type current_file_size = 0;
        {
            std::shared_lock<std::shared_mutex> lk(list_mtx_);
            if (segments_.empty()) return;
            auto& last_seg = segments_.back();
            if (last_seg.smgr->get_free_memory() >= (last_seg.seg->size / 2)) return;
            new_chunk_size = std::max(kMinSegmentSize, last_seg.seg->size * kGrowMultiplier);
            new_chunk_size = detail::align_up(new_chunk_size, detail::page_size());
            current_file_size = get_size_unlocked();
        }

        if (anon_) {
            auto seg = sub_segment::create_anon(new_chunk_size);
            if (!seg) return;
            std::unique_lock<std::shared_mutex> lk(list_mtx_);
            add_segment_anon_preallocated(std::move(seg), false);
        } else {
            std::size_t new_file_size = current_file_size + new_chunk_size;
            if (!detail::shm_grow(shm_root_name_.c_str(), new_file_size)) return;
            {
                std::unique_lock<std::shared_mutex> lk(list_mtx_);
                if (current_file_size == get_size_unlocked()) {
                    add_segment_shm(current_file_size, new_chunk_size, false, false, true);
                    update_segment_table();
                }
            }
        }
    }

    friend class prefetch_worker;

private:
    // -----------------------------------------------------------------------
    // Internal segment info
    // -----------------------------------------------------------------------
    struct seg_info {
        std::unique_ptr<sub_segment> seg;
        segment_manager*             smgr = nullptr; // lives at seg->base_vaddr
    };

    // -----------------------------------------------------------------------
    // Helpers (must be called with list_mtx_ held)
    // -----------------------------------------------------------------------

    void* try_allocate_locked(size_type bytes) noexcept {
        void* p = nullptr;
        for (auto& info : segments_) {
            p = info.smgr->allocate(bytes, std::nothrow_t{});
            if (p) [[likely]] break;
        }
        if (p) {
            auto& last_seg = segments_.back();
            if (last_seg.smgr->get_free_memory() < (last_seg.seg->size / 2)) {
                prefetch_worker::instance().hint_growth(this);
            }
        }
        return p;
    }



    void add_segment_anon(size_type size, bool is_primary) {
        auto seg = sub_segment::create_anon(size);
        if (!seg) throw std::bad_alloc();
        add_segment_anon_preallocated(std::move(seg), is_primary);
    }
    void add_segment_anon_preallocated(std::unique_ptr<sub_segment> seg, bool is_primary) {
        size_type size = seg->size;
        seg->is_primary = is_primary;

        // Register BEFORE constructing segment_manager so that the
        // segment_manager's internal offset_ptr writes can resolve correctly.
        segment_registry::instance().register_segment(seg.get());

        // Determine the byte offset at which the segment_manager starts.
        // For sub-segment 0, reserve space for the segment_table header.
        std::size_t smgr_offset =
            is_primary ? detail::align_up(sizeof(segment_table_t),
                                           alignof(si_seg_mgr))
                       : 0;

        void* smgr_base = static_cast<char*>(seg->base_vaddr) + smgr_offset;
        std::size_t smgr_size = size - smgr_offset;

        // Placement-new the Boost segment_manager.
        auto* smgr = ::new(smgr_base) si_seg_mgr(smgr_size);

        seg->manager = this;
        seg->discover_growth_fn = [](void*, std::size_t) { return false; }; // anon doesn't grow lazily
        seg->find_by_file_offset_fn = [](void* mgr, std::size_t offset) {
            return static_cast<segmented_segment_manager*>(mgr)->find_segment_by_file_offset(offset);
        };
        segments_.push_back({std::move(seg), smgr});
    }

    void add_segment_shm(size_type file_offset, size_type size,
                          bool is_primary, bool is_creator, bool initialize_smgr = false) {
        std::unique_ptr<sub_segment> seg;
        if (is_creator) {
            seg = sub_segment::create_shm(shm_root_name_.c_str(), size);
            initialize_smgr = true; // Creator always initializes
        } else {
            seg = sub_segment::open_shm_chunk(shm_root_name_.c_str(), size, file_offset);
        }
        if (!seg) throw std::runtime_error(
            "segmented_segment_manager: failed to map SHM chunk");
        seg->is_primary = is_primary;

        segment_registry::instance().register_segment(seg.get());

        std::size_t smgr_offset =
            is_primary ? detail::align_up(sizeof(segment_table_t),
                                           alignof(si_seg_mgr))
                       : 0;

        void* smgr_base = static_cast<char*>(seg->base_vaddr) + smgr_offset;
        std::size_t smgr_size = size - smgr_offset;

        seg->manager = this;
        seg->discover_growth_fn = [](void* mgr, std::size_t offset) {
            return static_cast<segmented_segment_manager*>(mgr)->lazy_discover_growth(offset);
        };
        seg->find_by_file_offset_fn = [](void* mgr, std::size_t offset) {
            return static_cast<segmented_segment_manager*>(mgr)->find_segment_by_file_offset(offset);
        };

        segments_.push_back({std::move(seg), nullptr});

        segment_manager* smgr = nullptr;
        if (initialize_smgr) {
            smgr = ::new(smgr_base) si_seg_mgr(smgr_size);
        } else {
            // Second process: segment_manager is already in place
            smgr = reinterpret_cast<si_seg_mgr*>(smgr_base);
        }
        
        segments_.back().smgr = smgr;
    }

    segment_manager* smgr_for(sub_segment* seg) const noexcept {
        for (const auto& info : segments_)
            if (info.seg.get() == seg) return info.smgr;
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Segment table (cross-process, sub-segment 0)
    // -----------------------------------------------------------------------

    void init_segment_table() {
        if (segments_.empty()) return;
        auto* tbl = segments_.front().seg->segment_table();
        tbl->magic   = kSegmentTableMagic;
        tbl->version = kSegmentTableVersion;
        tbl->capacity = static_cast<uint32_t>(kMaxSubSegments);
        tbl->count.store(1, std::memory_order_relaxed);

        const sub_segment* s0 = segments_.front().seg.get();
        auto& e = tbl->entries[0];
        e.file_offset = 0;
        e.size = static_cast<uint64_t>(s0->size);
    }

    void update_segment_table() {
        if (segments_.empty() || !segments_.front().seg->is_primary) return;
        auto* tbl = segments_.front().seg->segment_table();

        boost::interprocess::scoped_lock<
            boost::interprocess::interprocess_mutex> lk(tbl->mtx);

        std::size_t new_idx = segments_.size() - 1;
        if (new_idx >= kMaxSubSegments)
            throw std::runtime_error("Too many sub-segments");

        const sub_segment* ns = segments_[new_idx].seg.get();
        auto& e = tbl->entries[new_idx];
        e.file_offset = static_cast<uint64_t>(ns->file_offset);
        e.size = static_cast<uint64_t>(ns->size);
        tbl->count.store(static_cast<uint32_t>(new_idx + 1),
                         std::memory_order_release);
    }

    void open_all_segments() {
        // Open sub-segment 0 first
        // We need the size of sub-segment 0 — probe via a small initial map
        // to read the header.  Use a fixed minimum to bootstrap.
        auto seg0 = sub_segment::open_shm_chunk(shm_root_name_.c_str(), kMinSegmentSize, 0);
        if (!seg0) throw std::runtime_error(
            "Cannot open primary segment");

        segment_registry::instance().register_segment(seg0.get());
        seg0->is_primary = true;
        seg0->manager = this;
        seg0->discover_growth_fn = [](void* mgr, std::size_t offset) {
            return static_cast<segmented_segment_manager*>(mgr)->lazy_discover_growth(offset);
        };
        seg0->find_by_file_offset_fn = [](void* mgr, std::size_t offset) {
            return static_cast<segmented_segment_manager*>(mgr)->find_segment_by_file_offset(offset);
        };

        auto* tbl = seg0->segment_table();
        if (tbl->magic != kSegmentTableMagic)
            throw std::runtime_error("Invalid segment table magic");

        uint32_t count = tbl->count.load(std::memory_order_acquire);

        // Remap sub-segment 0 at correct size if necessary
        uint64_t s0_size = tbl->entries[0].size;
        if (s0_size > kMinSegmentSize) {
            segment_registry::instance().unregister_segment(seg0.get());
            seg0 = sub_segment::open_shm_chunk(shm_root_name_.c_str(),
                                          static_cast<size_type>(s0_size), 0);
            if (!seg0) throw std::bad_alloc();
            seg0->is_primary = true;
            seg0->manager = this;
            seg0->discover_growth_fn = [](void* mgr, std::size_t offset) {
                return static_cast<segmented_segment_manager*>(mgr)->lazy_discover_growth(offset);
            };
            seg0->find_by_file_offset_fn = [](void* mgr, std::size_t offset) {
                return static_cast<segmented_segment_manager*>(mgr)->find_segment_by_file_offset(offset);
            };
            segment_registry::instance().register_segment(seg0.get());
        }

        std::size_t smgr_offset = detail::align_up(sizeof(segment_table_t),
                                                   alignof(si_seg_mgr));
        void* smgr_base = static_cast<char*>(seg0->base_vaddr) + smgr_offset;
        auto* smgr0 = reinterpret_cast<si_seg_mgr*>(smgr_base);

        segments_.push_back({std::move(seg0), smgr0});

        // Map the remaining segments
        tbl = segments_.front().seg->segment_table(); // re-read after remap
        count = tbl->count.load(std::memory_order_acquire);
        for (uint32_t i = 1; i < count; ++i) {
            const auto& e = tbl->entries[i];
            add_segment_shm(static_cast<size_type>(e.file_offset),
                            static_cast<size_type>(e.size),
                            /*is_primary=*/false, /*is_creator=*/false);
        }
    }

    // -----------------------------------------------------------------------
    // Lazy Discovery
    // -----------------------------------------------------------------------
public:
    bool lazy_discover_growth(std::size_t required_file_offset) {
        if (anon_) return false; // anonymous memory cannot grow lazily

        std::unique_lock<std::shared_mutex> lk(list_mtx_);
        
        // Ensure we haven't already mapped it while waiting for the lock
        std::size_t current_mapped_size = 0;
        for (const auto& info : segments_) {
            current_mapped_size += info.seg->size;
        }
        if (required_file_offset < current_mapped_size) return true; // already discovered

        auto* tbl = segments_.front().seg->segment_table();
        uint32_t count = tbl->count.load(std::memory_order_acquire);
        
        bool discovered = false;
        std::size_t current_idx = segments_.size();
        for (uint32_t i = current_idx; i < count; ++i) {
            const auto& e = tbl->entries[i];
            add_segment_shm(static_cast<size_type>(e.file_offset),
                            static_cast<size_type>(e.size),
                            /*is_primary=*/false, /*is_creator=*/false);
            discovered = true;
            current_mapped_size += e.size;
        }
        
        return discovered && required_file_offset < current_mapped_size;
    }

    sub_segment* find_segment_by_file_offset(std::size_t file_offset) const {
        std::shared_lock<std::shared_mutex> lk(list_mtx_);
        for (const auto& info : segments_) {
            if (file_offset >= info.seg->file_offset && 
                file_offset < info.seg->file_offset + info.seg->size) {
                return info.seg.get();
            }
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------
    bool                   anon_;
    std::string            shm_root_name_;
    std::mutex grow_mtx_;
    mutable std::shared_mutex list_mtx_;
    std::vector<seg_info>  segments_;

    // We no longer use a process-local map for named objects,
    // we use the inner boost::interprocess::segment_manager's named allocation capabilities.
};

} // namespace segmented_interprocess


namespace segmented_interprocess {
inline void prefetch_worker::register_manager(segmented_segment_manager* mgr) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (active_managers_++ == 0) {
        stop_.store(false, std::memory_order_release);
        thread_ = std::thread(&prefetch_worker::worker_loop, this);
    }
}
inline void prefetch_worker::unregister_manager(segmented_segment_manager* mgr) {
    {
        std::lock_guard<detail::spinlock> lk(queue_lock_);
        std::queue<segmented_segment_manager*> new_q;
        while (!queue_.empty()) {
            auto* m = queue_.front();
            queue_.pop();
            if (m != mgr) new_q.push(m);
        }
        queue_ = std::move(new_q);
    }
    while (working_on_.load(std::memory_order_acquire) == mgr) {
        std::this_thread::yield();
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (--active_managers_ == 0) {
        stop_.store(true, std::memory_order_release);
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
}
inline void prefetch_worker::hint_growth(segmented_segment_manager* mgr) {
    {
        std::lock_guard<detail::spinlock> lk(queue_lock_);
        if (!queue_.empty() && queue_.back() == mgr) return;
        queue_.push(mgr);
    }
    { std::lock_guard<std::mutex> slk(sleep_mtx_); }
    cv_.notify_one();
}
inline void prefetch_worker::worker_loop() {
    while (true) {
        segmented_segment_manager* mgr_to_grow = nullptr;
        {
            std::lock_guard<detail::spinlock> lk(queue_lock_);
            if (!queue_.empty()) {
                mgr_to_grow = queue_.front();
                queue_.pop();
                working_on_.store(mgr_to_grow, std::memory_order_release);
            }
        }
        if (mgr_to_grow) {
            mgr_to_grow->grow_background();
            working_on_.store(nullptr, std::memory_order_release);
            continue;
        }
        std::unique_lock<std::mutex> slk(sleep_mtx_);
        cv_.wait(slk, [this]() {
            bool has_work = false;
            {
                std::lock_guard<detail::spinlock> lk(queue_lock_);
                has_work = !queue_.empty();
            }
            return stop_.load(std::memory_order_acquire) || has_work;
        });
        if (stop_.load(std::memory_order_acquire)) {
            bool has_work = false;
            {
                std::lock_guard<detail::spinlock> lk(queue_lock_);
                has_work = !queue_.empty();
            }
            if (!has_work) break;
        }
    }
}
} // namespace segmented_interprocess

// Signal that segmented_segment_manager is now fully defined so that
// segmented_allocator.hpp can compile the method bodies.
#define SEGMENTED_SEGMENT_MANAGER_DEFINED
#include "segmented_allocator.hpp"
#include "prefetch_worker.hpp"  // re-include to pick up the impl block
