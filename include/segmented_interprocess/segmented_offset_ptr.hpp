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

/// \file segmented_offset_ptr.hpp
/// Drop-in replacement for boost::interprocess::offset_ptr that supports
/// multiple non-contiguous mapped sub-segments.
///
/// m_offset encoding (the core semantic)
/// ───────────────────────────────────────
/// Let `self` = canonical VA of the segmented_offset_ptr instance itself
/// (i.e. `reinterpret_cast<uintptr_t>(this)` after PAC stripping).
///
///   Case A — `this` is inside a registered sub-segment S:
///     m_offset = pointee_vaddr  −  S.base_vaddr
///     get()    = S.base_vaddr   +  m_offset        → pointee_vaddr   ✓
///
///   Case B — `this` is NOT inside any registered sub-segment (stack, heap):
///     m_offset = pointee_vaddr  −  self_vaddr       (traditional offset_ptr)
///     get()    = self_vaddr     +  m_offset          → pointee_vaddr   ✓
///
/// Null sentinel: m_offset == kNull (value 1), matching Boost convention.
/// Value 1 is never produced by either encoding because:
///   Case A: S.base + 1 is inside segment_manager metadata; no user T* can
///           reside there.
///   Case B: self + 1 (1 byte ahead of this) cannot be a valid T* if
///           alignof(T) > 1, which is true for all non-char types.
///
/// Cross-segment pointers
/// ───────────────────────
/// If `this` lives in segment A and `pointee` lives in segment B:
///   m_offset = pointee_vaddr − A.base_vaddr
///   get() → A.base_vaddr + m_offset = pointee_vaddr              ✓
///
/// Pointer arithmetic (in-place)
/// ──────────────────────────────
/// For `++`, `--`, `+=n`, `-=n` on an existing pointer, we simply adjust
/// m_offset by `n * sizeof(T)` without any trie lookup.
///
/// Pointer construction (copy / assignment from raw pointer)
/// ──────────────────────────────────────────────────────────
/// Always resolves to a raw pointer via `get()` on the source and then calls
/// `set(raw_ptr)` on the destination.  Two trie lookups.  The TLS cache
/// makes both lookups fast when source and destination are in the same segment.

#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <iterator>

#include "segment_registry.hpp"
#include "detail/platform.hpp"

// Boost interprocess interop headers
#include <boost/interprocess/interprocess_fwd.hpp>
#include <boost/interprocess/detail/utilities.hpp>

namespace segmented_interprocess {

// ============================================================================
// Helper: reference_type avoids void&
// ============================================================================
namespace detail {
    template<class T> struct ref_helper          { using type = T&; };
    template<>        struct ref_helper<void>     { struct nat {}; using type = nat; };
    template<>        struct ref_helper<const void>  { struct nat {}; using type = nat; };
    template<>        struct ref_helper<volatile void> { struct nat {}; using type = nat; };
    template<>        struct ref_helper<const volatile void> { struct nat {}; using type = nat; };
}

// ============================================================================
// segmented_offset_ptr
// ============================================================================

/// \brief Smart pointer that works across multiple non-contiguous sub-segments.
///
/// \tparam T               Pointed-to type (may be void)
/// \tparam DifferenceType  Signed difference type (default ptrdiff_t)
/// \tparam OffsetType      Unsigned integer used to store the offset
///                         (default uintptr_t)
/// \tparam OffsetAlignment Alignment hint for the OffsetType storage
///                         (default 1 = use natural alignment of OffsetType)
template<
    class T,
    class DifferenceType  = std::ptrdiff_t,
    class OffsetType      = uintptr_t,
    std::size_t OffsetAlignment = 1u
>
class segmented_offset_ptr {

    static_assert(sizeof(OffsetType) >= sizeof(uintptr_t),
                  "OffsetType must be at least as wide as uintptr_t");
    static_assert(std::is_integral<OffsetType>::value &&
                  std::is_unsigned<OffsetType>::value,
                  "OffsetType must be an unsigned integer");

    // Choose the larger of OffsetAlignment and the natural alignment of OffsetType
    static constexpr std::size_t kStorageAlign =
        (OffsetAlignment > alignof(OffsetType)) ? OffsetAlignment
                                                : alignof(OffsetType);

public:
    // -----------------------------------------------------------------------
    // Type aliases (matching Boost offset_ptr)
    // -----------------------------------------------------------------------
    using element_type      = T;
    using pointer           = T*;
    using const_pointer     = const T*;
    using reference         = typename detail::ref_helper<T>::type;
    using difference_type   = DifferenceType;
    using offset_type       = OffsetType;
    using value_type        = T;
    using iterator_category = std::random_access_iterator_tag;

    template<class U>
    using rebind = segmented_offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment>;

    // -----------------------------------------------------------------------
    // Null sentinel
    // -----------------------------------------------------------------------
    /// Value of m_offset that represents a null pointer.
    /// Chosen to match Boost's convention (offset == 1 → null).
    static constexpr OffsetType kNull = OffsetType(1);

    // -----------------------------------------------------------------------
    // Constructors
    // -----------------------------------------------------------------------

    /// Default construct: null pointer.
    segmented_offset_ptr() noexcept : m_offset(kNull) {}

    /// Construct from nullptr.
    segmented_offset_ptr(std::nullptr_t) noexcept : m_offset(kNull) {}

    /// Construct from a raw pointer.
    explicit segmented_offset_ptr(pointer ptr) : m_offset(kNull) {
        set(ptr);
    }

    /// Copy constructor (always goes through get()/set() to handle
    /// cross-context encoding changes).
    segmented_offset_ptr(const segmented_offset_ptr& other)
        : m_offset(kNull) {
        set(other.get());
    }

    /// Conversion copy constructor from compatible pointer type U*.
    template<class U,
             class = std::enable_if_t<
                 std::is_convertible<U*, T*>::value &&
                 !std::is_same<U, T>::value>>
    segmented_offset_ptr(
        const segmented_offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment>& other
    ) : m_offset(kNull) {
        set(static_cast<T*>(other.get()));
    }

    // Tagged constructors (mirroring Boost cast helpers)
    struct static_cast_tag {};
    struct dynamic_cast_tag {};
    struct const_cast_tag {};
    struct reinterpret_cast_tag {};

    template<class U>
    segmented_offset_ptr(
        const segmented_offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment>& other,
        static_cast_tag) : m_offset(kNull) {
        set(static_cast<T*>(other.get()));
    }
    template<class U>
    segmented_offset_ptr(
        const segmented_offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment>& other,
        dynamic_cast_tag) : m_offset(kNull) {
        set(dynamic_cast<T*>(other.get()));
    }
    template<class U>
    segmented_offset_ptr(
        const segmented_offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment>& other,
        const_cast_tag) : m_offset(kNull) {
        set(const_cast<T*>(other.get()));
    }
    template<class U>
    segmented_offset_ptr(
        const segmented_offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment>& other,
        reinterpret_cast_tag) : m_offset(kNull) {
        set(reinterpret_cast<T*>(other.get()));
    }

    // -----------------------------------------------------------------------
    // Assignment
    // -----------------------------------------------------------------------

    segmented_offset_ptr& operator=(const segmented_offset_ptr& other) {
        set(other.get());
        return *this;
    }

    template<class U,
             class = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    segmented_offset_ptr& operator=(
        const segmented_offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment>& other
    ) noexcept {
        set(static_cast<T*>(other.get()));
        return *this;
    }

    segmented_offset_ptr& operator=(pointer ptr) {
        set(ptr);
        return *this;
    }

    segmented_offset_ptr& operator=(std::nullptr_t) noexcept {
        m_offset = kNull;
        return *this;
    }

    // -----------------------------------------------------------------------
    // Dereference / member access (disabled for void)
    // -----------------------------------------------------------------------

    template<class U = T,
             class = std::enable_if_t<!std::is_void<U>::value>>
    U& operator*() const noexcept {
        return *get();
    }

    template<class U = T,
             class = std::enable_if_t<!std::is_void<U>::value>>
    pointer operator->() const noexcept {
        return get();
    }

    // -----------------------------------------------------------------------
    // get() — the key resolution function
    // -----------------------------------------------------------------------

    /// Resolve the stored offset to a raw pointer.
    pointer get() const noexcept {
        if (m_offset == kNull) [[unlikely]] return nullptr;

        const uintptr_t self  = detail::ptr_to_vaddr(this);
        sub_segment*    self_seg   = segment_registry::instance().find(self);

        if (!self_seg) [[unlikely]] {
            // Case A: `this` is on stack/unmanaged heap
            return reinterpret_cast<pointer>(
                self + static_cast<DifferenceType>(m_offset));
        } else if (self_seg->is_anon) [[unlikely]] {
            // Case B: anonymous memory
            return reinterpret_cast<pointer>(
                reinterpret_cast<uintptr_t>(self_seg->base_vaddr) + m_offset);
        }

        std::size_t self_file_offset = self_seg->file_offset + 
            (self - reinterpret_cast<uintptr_t>(self_seg->base_vaddr));
        std::size_t target_file_offset = self_file_offset + m_offset;

        sub_segment* target_seg = self_seg->find_by_file_offset_fn(self_seg->manager, target_file_offset);
        if (!target_seg) [[unlikely]] {
            // Lazy discovery
            if (self_seg->discover_growth_fn && self_seg->discover_growth_fn(self_seg->manager, target_file_offset)) [[likely]] {
                target_seg = self_seg->find_by_file_offset_fn(self_seg->manager, target_file_offset);
            }
        }

        if (!target_seg) [[unlikely]] {
            // If it's STILL not found, the pointer is corrupted or we couldn't map the growth.
            // We can't throw std::runtime_error from a noexcept function.
            // However, get() is marked noexcept! 
            // Wait, if it fails, returning nullptr or trapping is the only option.
            // We'll return nullptr for safety.
            return nullptr;
        }

        pointer ret = reinterpret_cast<pointer>(
            reinterpret_cast<uintptr_t>(target_seg->base_vaddr) + 
            (target_file_offset - target_seg->file_offset));
        return ret;
    }

    /// Return raw offset value (Boost compatibility, internal use).
    OffsetType get_offset() const noexcept { return m_offset; }

    // -----------------------------------------------------------------------
    // Boolean conversion / null check
    // -----------------------------------------------------------------------
    explicit operator bool() const noexcept { return m_offset != kNull; }
    bool operator!()         const noexcept { return m_offset == kNull; }

    // -----------------------------------------------------------------------
    // Subscript operator (disabled for void)
    // -----------------------------------------------------------------------
    template<class U = T,
             class = std::enable_if_t<!std::is_void<U>::value>>
    U& operator[](difference_type n) const noexcept {
        return get()[n];
    }

    // -----------------------------------------------------------------------
    // In-place arithmetic (no trie lookup needed — just adjust m_offset)
    // -----------------------------------------------------------------------
    segmented_offset_ptr& operator++() noexcept {
        inc_offset(static_cast<DifferenceType>(sizeof(T)));
        return *this;
    }
    segmented_offset_ptr  operator++(int) noexcept {
        auto tmp = *this;
        inc_offset(static_cast<DifferenceType>(sizeof(T)));
        return tmp;
    }
    segmented_offset_ptr& operator--() noexcept {
        inc_offset(-static_cast<DifferenceType>(sizeof(T)));
        return *this;
    }
    segmented_offset_ptr  operator--(int) noexcept {
        auto tmp = *this;
        inc_offset(-static_cast<DifferenceType>(sizeof(T)));
        return tmp;
    }
    segmented_offset_ptr& operator+=(difference_type n) noexcept {
        inc_offset(n * static_cast<DifferenceType>(sizeof(T)));
        return *this;
    }
    segmented_offset_ptr& operator-=(difference_type n) noexcept {
        inc_offset(-n * static_cast<DifferenceType>(sizeof(T)));
        return *this;
    }

    // -----------------------------------------------------------------------
    // Binary arithmetic
    // -----------------------------------------------------------------------
    friend segmented_offset_ptr operator+(
        segmented_offset_ptr p, difference_type n) noexcept
    {
        p += n; return p;
    }
    friend segmented_offset_ptr operator+(
        difference_type n, segmented_offset_ptr p) noexcept
    {
        p += n; return p;
    }
    friend segmented_offset_ptr operator-(
        segmented_offset_ptr p, difference_type n) noexcept
    {
        p -= n; return p;
    }
    friend difference_type operator-(
        const segmented_offset_ptr& a,
        const segmented_offset_ptr& b) noexcept
    {
        return static_cast<difference_type>(a.get() - b.get());
    }

    // -----------------------------------------------------------------------
    // Comparison operators
    // -----------------------------------------------------------------------
    friend bool operator==(const segmented_offset_ptr& a,
                           const segmented_offset_ptr& b) noexcept
    { return a.get() == b.get(); }

    friend bool operator==(const segmented_offset_ptr& a, std::nullptr_t) noexcept
    { return !a; }
    friend bool operator==(std::nullptr_t, const segmented_offset_ptr& a) noexcept
    { return !a; }

    friend bool operator!=(const segmented_offset_ptr& a,
                           const segmented_offset_ptr& b) noexcept
    { return a.get() != b.get(); }
    friend bool operator!=(const segmented_offset_ptr& a, std::nullptr_t) noexcept
    { return !!a; }
    friend bool operator!=(std::nullptr_t, const segmented_offset_ptr& a) noexcept
    { return !!a; }

    friend bool operator< (const segmented_offset_ptr& a,
                           const segmented_offset_ptr& b) noexcept
    { return a.get() < b.get(); }
    friend bool operator<=(const segmented_offset_ptr& a,
                           const segmented_offset_ptr& b) noexcept
    { return a.get() <= b.get(); }
    friend bool operator> (const segmented_offset_ptr& a,
                           const segmented_offset_ptr& b) noexcept
    { return a.get() > b.get(); }
    friend bool operator>=(const segmented_offset_ptr& a,
                           const segmented_offset_ptr& b) noexcept
    { return a.get() >= b.get(); }

    // -----------------------------------------------------------------------
    // pointer_to (Boost / std::pointer_traits)
    // -----------------------------------------------------------------------
    template<class U = T, class = std::enable_if_t<!std::is_void<U>::value>>
    static segmented_offset_ptr pointer_to(U& r) noexcept {
        return segmented_offset_ptr(std::addressof(r));
    }

private:
    // -----------------------------------------------------------------------
    // set() — encode a raw pointer into m_offset
    // -----------------------------------------------------------------------
    void set(pointer ptr) {
        if (!ptr) [[unlikely]] { m_offset = kNull; return; }
        // TRACE
        // std::cout << "set() called with ptr=" << ptr << "\n" << std::flush;

        const uintptr_t self     = detail::ptr_to_vaddr(this);
        const uintptr_t pointee  = reinterpret_cast<uintptr_t>(
            const_cast<std::remove_cv_t<T>*>(ptr));
        
        sub_segment* self_seg   = segment_registry::instance().find(self);
        // std::cout << "self_seg=" << self_seg << "\n" << std::flush;
        sub_segment* target_seg = segment_registry::instance().find(pointee);
        // std::cout << "target_seg=" << target_seg << "\n" << std::flush;

        if (!self_seg) [[unlikely]] {
            // Case A: `this` is on stack/unmanaged heap
            m_offset = static_cast<OffsetType>(
                static_cast<DifferenceType>(pointee) -
                static_cast<DifferenceType>(self));
        } else if (self_seg->is_anon) [[unlikely]] {
            // Case B: anonymous memory
            m_offset = static_cast<OffsetType>(
                pointee - reinterpret_cast<uintptr_t>(self_seg->base_vaddr));
        } else [[likely]] {
            // Case C: Shared memory
            if (!target_seg) [[unlikely]] {
                throw std::runtime_error("segmented_offset_ptr in managed SHM cannot point to unmanaged memory");
            }
            if (target_seg->manager != self_seg->manager) [[unlikely]] {
                throw std::runtime_error("segmented_offset_ptr cannot point across different segmented_segment_managers");
            }
            
            std::size_t self_file_offset = self_seg->file_offset + 
                (self - reinterpret_cast<uintptr_t>(self_seg->base_vaddr));
            std::size_t target_file_offset = target_seg->file_offset + 
                (pointee - reinterpret_cast<uintptr_t>(target_seg->base_vaddr));
                
            m_offset = static_cast<OffsetType>(
                static_cast<DifferenceType>(target_file_offset) - 
                static_cast<DifferenceType>(self_file_offset));
        }

        // Validate we didn't accidentally produce the null sentinel.
        assert(m_offset != kNull &&
               "Encoded offset collides with null sentinel — likely alignment issue");
    }

    /// Adjust m_offset by `bytes` without a trie lookup.
    void inc_offset(DifferenceType bytes) noexcept {
        m_offset = static_cast<OffsetType>(
            static_cast<DifferenceType>(m_offset) + bytes);
    }

    // -----------------------------------------------------------------------
    // Storage
    // -----------------------------------------------------------------------
    alignas(kStorageAlign) OffsetType m_offset;
};

// ============================================================================
// Cast helpers (matching Boost)
// ============================================================================

template<class T, class U, class D, class O, std::size_t A>
segmented_offset_ptr<T, D, O, A>
static_pointer_cast(const segmented_offset_ptr<U, D, O, A>& p) noexcept {
    return segmented_offset_ptr<T, D, O, A>(
        p, typename segmented_offset_ptr<T, D, O, A>::static_cast_tag{});
}

template<class T, class U, class D, class O, std::size_t A>
segmented_offset_ptr<T, D, O, A>
dynamic_pointer_cast(const segmented_offset_ptr<U, D, O, A>& p) noexcept {
    return segmented_offset_ptr<T, D, O, A>(
        p, typename segmented_offset_ptr<T, D, O, A>::dynamic_cast_tag{});
}

template<class T, class U, class D, class O, std::size_t A>
segmented_offset_ptr<T, D, O, A>
const_pointer_cast(const segmented_offset_ptr<U, D, O, A>& p) noexcept {
    return segmented_offset_ptr<T, D, O, A>(
        p, typename segmented_offset_ptr<T, D, O, A>::const_cast_tag{});
}

template<class T, class U, class D, class O, std::size_t A>
segmented_offset_ptr<T, D, O, A>
reinterpret_pointer_cast(const segmented_offset_ptr<U, D, O, A>& p) noexcept {
    return segmented_offset_ptr<T, D, O, A>(
        p, typename segmented_offset_ptr<T, D, O, A>::reinterpret_cast_tag{});
}

/// to_raw_pointer — enables boost::mem_fn / to_address
template<class T, class D, class O, std::size_t A>
T* to_raw_pointer(const segmented_offset_ptr<T, D, O, A>& p) noexcept {
    return p.get();
}

} // namespace segmented_interprocess

// ============================================================================
// Boost pointer_to_other specialisation
// pointer_to_other is declared in namespace boost (see offset_ptr.hpp:757)
// ============================================================================
namespace boost {

template<class T, class D, class O, std::size_t A, class Other>
struct pointer_to_other<
    ::segmented_interprocess::segmented_offset_ptr<T, D, O, A>, Other>
{
    using type = ::segmented_interprocess::segmented_offset_ptr<Other, D, O, A>;
};

} // namespace boost

// ============================================================================
// std::pointer_traits specialisation
// ============================================================================
namespace std {

template<class T, class D, class O, std::size_t A>
struct pointer_traits<::segmented_interprocess::segmented_offset_ptr<T, D, O, A>> {
    using pointer         = ::segmented_interprocess::segmented_offset_ptr<T, D, O, A>;
    using element_type    = T;
    using difference_type = D;

    template<class U>
    using rebind = ::segmented_interprocess::segmented_offset_ptr<U, D, O, A>;

    // pointer_to: only for non-void (matches Boost's pattern)
    template<class U = T>
    static auto pointer_to(U& r) noexcept
        -> std::enable_if_t<!std::is_void<U>::value, pointer>
    {
        return pointer(std::addressof(r));
    }

    static T* to_address(const pointer& p) noexcept {
        return p.get();
    }
};

} // namespace std

// ============================================================================
// Boost has_trivial_destructor (needed by some Boost containers)
// ============================================================================
namespace boost {

template<class T, class D, class O, std::size_t A>
struct has_trivial_destructor<
    ::segmented_interprocess::segmented_offset_ptr<T, D, O, A>>
    : std::false_type {};

} // namespace boost

// ============================================================================
// Boost.Intrusive pointer_rebind specialisation
// ============================================================================
// Boost.Intrusive's pointer_rebind machinery looks for a nested ::rebind<U>
// template.  Our segmented_offset_ptr already provides it (see `rebind` alias
// above).  If Boost.Intrusive still cannot find it, the fallback here covers
// the partial-specialisation route.
namespace boost {
namespace intrusive {

// Forward-declare (defined in boost/intrusive/pointer_rebind.hpp)
template<class Ptr, class T> struct pointer_rebind;

template<
    class T, class D, class O, std::size_t A, class U
>
struct pointer_rebind<
    ::segmented_interprocess::segmented_offset_ptr<T, D, O, A>, U>
{
    using type = ::segmented_interprocess::segmented_offset_ptr<U, D, O, A>;
};

} // namespace intrusive
} // namespace boost
