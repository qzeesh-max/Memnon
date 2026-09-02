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

/// \file segment_registry.hpp
/// Process-local singleton that maps virtual addresses to sub_segment*.
///
/// The registry wraps an ncrit_trie and provides the full-segment-bounds TLS
/// cache update that ncrit_trie::lookup() leaves to the caller (to avoid a
/// circular dependency on sub_segment).
///
/// Thread safety
/// ─────────────
///   • lookup()    — fully lock-free, uses trie + TLS cache
///   • register_segment() / unregister_segment() — lock-free trie mutations
///     (concurrent readers always see a consistent state)
///   • The Meyers singleton (instance()) is safe under C++11 static-local init

#pragma once

#include <cstdint>
#include <cstddef>

#include "ncrit_trie.hpp"
#include "sub_segment.hpp"

namespace segmented_interprocess {

// ============================================================================
// segment_registry
// ============================================================================

class segment_registry {
public:
    /// Return the process-local singleton instance.
    static segment_registry& instance() noexcept {
        // C++11 guarantees thread-safe initialisation of function-local statics.
        static segment_registry kInstance;
        return kInstance;
    }

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    /// Register all pages of `seg` in the trie.
    /// Must be called BEFORE the segment_manager inside `seg` is constructed.
    void register_segment(sub_segment* seg) noexcept {
        trie_.insert_range(seg->base_addr(), seg->size, seg);
        // Pre-warm TLS cache for the registering thread so the very first
        // internal allocations in the segment_manager are fast.
        trie_.lookup(seg->base_addr());
        ncrit_trie<sub_segment*>::expand_cache_bounds_top(
            seg->base_addr(), seg->size, seg);
    }

    /// Remove all page registrations for `seg`.
    /// Must be called AFTER the segment_manager inside `seg` is destroyed.
    void unregister_segment(sub_segment* seg) noexcept {
        trie_.remove_range(seg->base_addr(), seg->size);
        // Invalidate TLS cache so stale cached pointer is not returned.
        ncrit_trie<sub_segment*>::invalidate_cache();
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    /// Return the sub_segment owning `addr`, or nullptr if not in any segment.
    ///
    /// Fast path: O(1) TLS range check.
    /// Slow path: O(Levels) trie acquire-loads → update TLS cache with full
    ///            segment bounds for subsequent accesses.
    sub_segment* find(uintptr_t addr) const noexcept {
        // Fast path (TLS LRU Cache) and slow path are both handled inside trie_.lookup
        sub_segment* seg = trie_.lookup(addr);

        // Update TLS cache with full segment bounds so subsequent accesses
        // within the same segment hit the cache without going to page boundaries.
        if (seg) {
            ncrit_trie<sub_segment*>::expand_cache_bounds_top(
                seg->base_addr(), seg->size, seg);
        }
        return seg;
    }

    // Non-copyable singleton
    segment_registry(const segment_registry&) = delete;
    segment_registry& operator=(const segment_registry&) = delete;

private:
    segment_registry() = default;

    mutable ncrit_trie<sub_segment*> trie_;
};

} // namespace segmented_interprocess
