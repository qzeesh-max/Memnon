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

/// \file segmented_managed_memory.hpp
/// Top-level user-facing class: Boost.Interprocess-compatible managed memory
/// that grows online across multiple mapped sub-segments.
///
/// Usage (single-process):
/// \code
///   segmented_managed_memory mem(64 * 1024 * 1024); // 64 MiB initial
///   auto* v = mem.construct<MyVector>("my_vec")();
///   mem.grow(32 * 1024 * 1024);                     // add 32 MiB online
///   auto [p, n] = mem.find<MyVector>("my_vec");
///   mem.destroy<MyVector>("my_vec");
/// \endcode
///
/// Usage (cross-process, creator):
/// \code
///   segmented_managed_memory creator("my_shm", open_or_create, 64*1024*1024);
/// \endcode
///
/// Usage (cross-process, opener):
/// \code
///   segmented_managed_memory opener("my_shm", open_only);
/// \endcode

#pragma once

#include <string>
#include <cstddef>
#include <utility>

#include "segmented_segment_manager.hpp"
#include "segmented_allocator.hpp"

namespace segmented_interprocess {

// ============================================================================
// Tag types (matching boost::interprocess open/create tags)
// ============================================================================
struct create_only_t {};
struct open_only_t {};
struct open_or_create_t {};

inline constexpr create_only_t   create_only{};
inline constexpr open_only_t     open_only{};
inline constexpr open_or_create_t open_or_create{};

// ============================================================================
// segmented_managed_memory
// ============================================================================

/// \brief Online-growable managed memory backed by multiple sub-segments.
///
/// Provides a Boost.Interprocess-compatible named object API (construct, find,
/// destroy) plus a grow() call to add capacity at runtime without offline
/// resizing.
class segmented_managed_memory {
public:
    using size_type    = std::size_t;
    using seg_manager  = segmented_segment_manager;

    template<class T>
    using allocator = segmented_allocator<T>;

    // -----------------------------------------------------------------------
    // Single-process constructors
    // -----------------------------------------------------------------------

    /// Create an anonymous (single-process) segmented managed memory.
    explicit segmented_managed_memory(size_type initial_size = kDefaultSegSize)
        : mgr_(std::make_unique<seg_manager>(initial_size)) {}

    // -----------------------------------------------------------------------
    // Cross-process constructors
    // -----------------------------------------------------------------------

    /// Create a named SHM-backed segmented managed memory (creator).
    segmented_managed_memory(const std::string& name,
                              create_only_t,
                              size_type initial_size = kDefaultSegSize)
        : mgr_(std::make_unique<seg_manager>(name, initial_size, /*creator=*/true))
    {}

    /// Open an existing named SHM segmented managed memory (non-creator).
    segmented_managed_memory(const std::string& name, open_only_t)
        : mgr_(std::make_unique<seg_manager>(name, 0, /*creator=*/false))
    {}

    /// Open or create a named SHM segmented managed memory.
    segmented_managed_memory(const std::string& name,
                              open_or_create_t,
                              size_type initial_size = kDefaultSegSize)
    {
        // Try open first; if it fails, create.
        try {
            mgr_ = std::make_unique<seg_manager>(name, 0, /*creator=*/false);
        } catch (...) {
            mgr_ = std::make_unique<seg_manager>(name, initial_size, /*creator=*/true);
        }
    }

    ~segmented_managed_memory() = default;

    // Non-copyable
    segmented_managed_memory(const segmented_managed_memory&) = delete;
    segmented_managed_memory& operator=(const segmented_managed_memory&) = delete;

    // -----------------------------------------------------------------------
    // Named object API
    // -----------------------------------------------------------------------

    /// Construct and return a named object.
    /// Usage: `mem.construct<Foo>("name", arg1, arg2)`
    template<class T, class... Args>
    T* construct(const char* name, Args&&... args) {
        return mgr_->construct<T>(name, std::forward<Args>(args)...);
    }

    /// Find a named object.  Returns {ptr, count} or {nullptr, 0}.
    template<class T>
    std::pair<T*, std::size_t> find(const char* name) {
        return mgr_->find<T>(name);
    }

    /// Find-or-construct: returns existing if found, otherwise constructs.
    template<class T, class... Args>
    T* find_or_construct(const char* name, Args&&... args) {
        auto [p, n] = mgr_->find<T>(name);
        if (p) return p;
        return mgr_->construct<T>(name, std::forward<Args>(args)...);
    }

    /// Destroy a named object by name.
    template<class T>
    bool destroy(const char* name) {
        return mgr_->destroy<T>(name);
    }

    // -----------------------------------------------------------------------
    // Raw allocation (anonymous)
    // -----------------------------------------------------------------------
    void* allocate(size_type bytes) { return mgr_->allocate(bytes); }
    void  deallocate(void* ptr) noexcept { mgr_->deallocate(ptr); }

    // -----------------------------------------------------------------------
    // Growth
    // -----------------------------------------------------------------------

    /// Add at least `additional_bytes` of new capacity online.
    /// All concurrent readers and writers continue to function.
    void grow(size_type additional_bytes) {
        mgr_->grow(additional_bytes);
    }

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------
    size_type    get_free_memory()  const { return mgr_->get_free_memory(); }
    size_type    get_size()         const { return mgr_->get_size(); }
    std::size_t  segment_count()    const { return mgr_->segment_count(); }
    seg_manager& get_segment_manager() noexcept { return *mgr_; }

    // -----------------------------------------------------------------------
    // Allocator factory
    // -----------------------------------------------------------------------
    template<class T>
    segmented_allocator<T> get_allocator() {
        return segmented_allocator<T>(*mgr_);
    }

private:
    std::unique_ptr<seg_manager> mgr_;
};

} // namespace segmented_interprocess
