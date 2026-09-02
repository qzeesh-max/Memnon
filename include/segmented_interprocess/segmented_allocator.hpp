/// \file segmented_allocator.hpp
/// Boost-compatible allocator that uses segmented_offset_ptr as its pointer
/// type, and delegates raw allocation to segmented_segment_manager.

#ifndef SEGMENTED_ALLOCATOR_HPP
#define SEGMENTED_ALLOCATOR_HPP

#include <cstddef>
#include <type_traits>
#include <limits>


#include "segmented_offset_ptr.hpp"

namespace segmented_interprocess {

// Forward declaration (defined in segmented_segment_manager.hpp)
class segmented_segment_manager;

// ============================================================================
// segmented_allocator
// ============================================================================

/// \brief Stateful Boost-style allocator backed by a segmented_segment_manager.
///
/// Satisfies the Allocator concept (C++17) and is compatible with
/// boost::container containers when stored in a segmented_managed_memory.
///
/// \tparam T  Element type to allocate.
template<class T>
class segmented_allocator {
public:
    using value_type      = T;
    using pointer         = segmented_offset_ptr<T>;
    using const_pointer   = segmented_offset_ptr<const T>;
    using void_pointer    = segmented_offset_ptr<void>;
    using const_void_pointer = segmented_offset_ptr<const void>;
    using reference       = T&;
    using const_reference = const T&;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::false_type;

    template<class U>
    struct rebind { using other = segmented_allocator<U>; };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------
    explicit segmented_allocator(segmented_segment_manager& mgr) noexcept
        : mgr_(&mgr) {}

    template<class U>
    segmented_allocator(const segmented_allocator<U>& other) noexcept
        : mgr_(other.manager()) {}

    // -----------------------------------------------------------------------
    // Allocation (bodies defined after segmented_segment_manager)
    // -----------------------------------------------------------------------
    pointer allocate(size_type n);
    void    deallocate(pointer p, size_type n) noexcept;

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------
    template<class U, class... Args>
    void construct(U* p, Args&&... args) {
        ::new(static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
    template<class U>
    void destroy(U* p) noexcept { p->~U(); }

    // -----------------------------------------------------------------------
    // Capacity
    // -----------------------------------------------------------------------
    size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(T);
    }

    // -----------------------------------------------------------------------
    // Equality (same manager instance)
    // -----------------------------------------------------------------------
    segmented_segment_manager* manager() const noexcept { return mgr_; }

    bool operator==(const segmented_allocator& o) const noexcept {
        return mgr_ == o.mgr_;
    }
    bool operator!=(const segmented_allocator& o) const noexcept {
        return mgr_ != o.mgr_;
    }

private:
    segmented_segment_manager* mgr_;
};

} // namespace segmented_interprocess

#endif // SEGMENTED_ALLOCATOR_HPP

// ============================================================================
// Method bodies — included after segmented_segment_manager is fully defined
// ============================================================================
// This guard prevents double-inclusion of the body block when both
// segmented_allocator.hpp and segmented_segment_manager.hpp are included.
#ifndef SEGMENTED_ALLOCATOR_IMPL_INCLUDED
#ifdef SEGMENTED_SEGMENT_MANAGER_DEFINED  // set at end of segmented_segment_manager.hpp
#define SEGMENTED_ALLOCATOR_IMPL_INCLUDED

namespace segmented_interprocess {

template<class T>
typename segmented_allocator<T>::pointer
segmented_allocator<T>::allocate(size_type n) {
    void* raw = mgr_->allocate(n * sizeof(T));
    return pointer(static_cast<T*>(raw));
}

template<class T>
void segmented_allocator<T>::deallocate(pointer p, size_type) noexcept {
    mgr_->deallocate(p.get());
}

} // namespace segmented_interprocess

#endif // SEGMENTED_SEGMENT_MANAGER_DEFINED
#endif // SEGMENTED_ALLOCATOR_IMPL_INCLUDED
