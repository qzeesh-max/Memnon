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

/// \file sub_segment.hpp
/// Descriptor for a single contiguous virtual-address sub-segment.
///
/// A sub_segment wraps one `mmap`'d (or POSIX SHM-backed) region.  It stores
/// the base virtual address, size, and (for cross-process segments) the SHM
/// name.  The Boost `segment_manager` instance lives at the very start of the
/// mapped region; the segment table header (for sub-segment 0) lives before
/// the segment_manager at a known offset.
///
/// Lifecycle
/// ─────────
///   create_anon()  — allocate an anonymous private region (single-process)
///   create_shm()   — allocate a named POSIX SHM region (cross-process)
///   open_shm()     — attach to an existing named SHM region (second process)
///   ~sub_segment() — munmap (and shm_unlink if we are the creator)
///
/// The constructor and destructor are intentionally NOT responsible for
/// registering/unregistering with segment_registry — that is the responsibility
/// of segmented_segment_manager to keep ordering guarantees.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <memory>
#include <atomic>
#include <mutex>

#include "detail/platform.hpp"

// Boost interprocess for the segment_manager + allocator types
#include <boost/interprocess/mem_algo/rbtree_best_fit.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/indexes/iset_index.hpp>

namespace segmented_interprocess {

// Forward declare our offset_ptr replacement (used as void_pointer below)
template<class T, class D, class O, std::size_t A>
class segmented_offset_ptr;

// ============================================================================
// Shared segment table (lives at start of sub-segment 0)
// ============================================================================

/// Maximum number of sub-segments supported per segmented_managed_memory.
inline constexpr std::size_t kMaxSubSegments = 64;

/// SHM name maximum length (including null terminator).
inline constexpr std::size_t kShmNameMax = 128;

/// Magic number to verify the segment table is initialised.
inline constexpr uint64_t kSegmentTableMagic = 0xB001'5EF5'DEAD'BEEFull;
inline constexpr uint32_t kSegmentTableVersion = 1u;

/// \brief Descriptor for a single sub-segment stored in shared memory.
struct segment_entry {
    uint64_t file_offset;
    uint64_t size;
    uint64_t reserved[18];
};
static_assert(sizeof(segment_entry) == 160, "segment_entry must be 160 bytes");

/// \brief Segment table header stored at offset 0 of sub-segment 0.
///
/// The table is protected by `mtx` (a Boost interprocess_mutex stored in
/// shared memory) so that concurrent grow() calls from multiple processes are
/// serialised.  Reads of `count` are done with acquire to observe any
/// concurrent additions.
struct alignas(64) segment_table_t {
    uint64_t   magic;     ///< Must equal kSegmentTableMagic
    uint32_t   version;   ///< Must equal kSegmentTableVersion
    uint32_t   capacity;  ///< Always kMaxSubSegments (informational)
    std::atomic<uint32_t> count; ///< Number of valid entries (including seg 0)
    char       _pad[44];  ///< Pad to 64-byte boundary before mutex

    boost::interprocess::interprocess_mutex mtx; ///< Guards table modifications

    segment_entry entries[kMaxSubSegments]; ///< Per-segment descriptors
};

/// Offset of the segment_manager inside sub-segment 0 (after the table).
inline std::size_t segment_manager_offset() noexcept {
    // Align to the page boundary following the segment_table_t.
    return detail::align_up(sizeof(segment_table_t), detail::page_size());
}

// ============================================================================
// sub_segment
// ============================================================================

/// \brief A single contiguous mapped region backing part of a
///        segmented_managed_memory.
///
/// sub_segment objects are NOT stored in shared memory — they are per-process
/// C++ heap objects.  The segment_registry and segmented_segment_manager each
/// hold pointers to them.
struct sub_segment {
    void*  base_vaddr   = nullptr; ///< Virtual base of the mmap'd region
    std::size_t size    = 0;       ///< Byte size
    bool   is_primary   = false;   ///< True for sub-segment index 0
    bool   is_creator   = false;   ///< True if this process created the SHM
    bool   is_anon      = false;   ///< True for anonymous (single-process) segments
    char   shm_name[kShmNameMax] = {}; ///< SHM name (empty if anon)
    std::size_t file_offset = 0;       ///< Logical offset in the SHM file (0 for anon)
    void*  manager      = nullptr; ///< Pointer to owning segmented_segment_manager
    bool (*discover_growth_fn)(void*, std::size_t) = nullptr; ///< Callback for lazy discovery
    sub_segment* (*find_by_file_offset_fn)(void*, std::size_t) = nullptr; ///< Callback for finding segment

    // -------------------------------------------------------------------------
    // Factory functions
    // -------------------------------------------------------------------------

    /// Allocate an anonymous private region.  Single-process use only.
    static std::unique_ptr<sub_segment>
    create_anon(std::size_t size) noexcept {
        void* base = detail::mmap_alloc(size);
        if (!base) return nullptr;

        auto seg = std::make_unique<sub_segment>();
        seg->base_vaddr = base;
        seg->size       = size;
        seg->is_anon    = true;
        seg->is_creator = true;
        return seg;
    }

    /// Create a named SHM-backed region (cross-process, creator side).
    static std::unique_ptr<sub_segment>
    create_shm(const char* name, std::size_t size) noexcept {
        void* base = detail::shm_create(name, size);
        if (!base) return nullptr;

        auto seg = std::make_unique<sub_segment>();
        seg->base_vaddr = base;
        seg->size       = size;
        seg->is_anon    = false;
        seg->is_creator = true;
        std::strncpy(seg->shm_name, name, kShmNameMax - 1);
        return seg;
    }

    /// Attach to an existing named SHM region (cross-process, opener side, mapping entire region or sub-chunk).
    static std::unique_ptr<sub_segment>
    open_shm(const char* name, std::size_t size) noexcept {
        return open_shm_chunk(name, size, 0);
    }

    /// Attach to a specific chunk of an existing named SHM region.
    static std::unique_ptr<sub_segment>
    open_shm_chunk(const char* name, std::size_t size, std::size_t offset) noexcept {
        void* base = detail::shm_map_chunk(name, size, offset);
        if (!base) return nullptr;

        auto seg = std::make_unique<sub_segment>();
        seg->base_vaddr = base;
        seg->size       = size;
        seg->is_anon    = false;
        seg->is_creator = false;
        seg->file_offset = offset;
        std::strncpy(seg->shm_name, name, kShmNameMax - 1);
        return seg;
    }

    // -------------------------------------------------------------------------
    // Destructor
    // -------------------------------------------------------------------------
    ~sub_segment() {
        if (!base_vaddr) return;
        if (is_anon) {
            detail::mmap_free(base_vaddr, size);
        } else if (is_creator) {
            detail::shm_destroy(base_vaddr, size, shm_name);
        } else {
            detail::shm_close(base_vaddr, size);
        }
    }

    // Non-copyable (owns the mapping)
    sub_segment(const sub_segment&) = delete;
    sub_segment& operator=(const sub_segment&) = delete;

    sub_segment() = default;

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------
    uintptr_t base_addr() const noexcept {
        return detail::ptr_to_vaddr(base_vaddr);
    }
    uintptr_t end_addr() const noexcept {
        return base_addr() + size;
    }
    bool contains(uintptr_t addr) const noexcept {
        return addr >= base_addr() && addr < end_addr();
    }

    /// Pointer to the segment_table (only valid for is_primary == true).
    segment_table_t* segment_table() noexcept {
        assert(is_primary);
        return reinterpret_cast<segment_table_t*>(base_vaddr);
    }
    const segment_table_t* segment_table() const noexcept {
        assert(is_primary);
        return reinterpret_cast<const segment_table_t*>(base_vaddr);
    }
};

} // namespace segmented_interprocess
