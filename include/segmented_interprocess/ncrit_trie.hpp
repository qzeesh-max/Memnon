/// \file ncrit_trie.hpp
/// Lock-free n-Crit Radix Trie for virtual-address → sub_segment* mapping.
///
/// Design overview
/// ───────────────
/// The trie is a fixed-depth, fixed-stride multi-level radix trie.  Keys are
/// page numbers (addr >> page_shift).  Values are pointers to sub_segment
/// descriptors.
///
/// Default parameters (macOS arm64 with 16 KiB pages, 48-bit user VA):
///   Stride  = 9  → fanout 512 per node, 4 KB per node (single page)
///   Levels  = 4  → covers 36-bit page numbers → 50-bit VA
///
/// Node layout
/// ───────────
/// Every node is an aligned array of `kFanOut` atomic `uintptr_t` slots.
/// Interpretation depends on the traversal depth:
///
///   depth in [0, Levels-2]:   slot holds 0 (empty) or a pointer to the
///                              child node (bit 0 clear, guaranteed by alignment).
///
///   depth == Levels-1 (leaf): slot holds 0 (no mapping) or a *tagged* value
///                              pointer: (reinterpret_cast<uintptr_t>(value) | 1).
///
/// Because Value (sub_segment*) is at least 8-byte aligned, bit 0 is always
/// 0 for a real pointer, allowing the tag to be used as a sentinel.
///
/// Memory-order rationale
/// ──────────────────────
///   insert_range / clear_leaf:
///     - CAS on internal node slots uses memory_order_acq_rel (success) and
///       memory_order_acquire (failure).  This ensures:
///         * The newly allocated child node (written entirely before CAS) is
///           visible to any thread that reads the CAS result with acquire.
///         * If two inserters race, the loser reads the winner's node with
///           acquire, establishing a happens-before edge.
///     - Leaf stores use memory_order_release, ensuring the leaf value is
///       visible to any subsequent acquire-load on the same slot.
///
///   lookup_page (and lookup):
///     - Every load is memory_order_acquire, forming a chain of happens-before
///       edges with inserting threads' release stores.  This guarantees that
///       a lookup observes a fully initialised node whose child link was
///       published with acq_rel.
///
/// Thread-local cache
/// ──────────────────
/// A thread-local cache stores the most-recently looked-up (base, end, seg*)
/// triple.  For allocator-internal operations (free-list traversal, rbtree
/// rotations) that stay within one sub-segment, the cache eliminates most
/// trie traversals, reducing overhead from 4 cache-miss loads to a single
/// in-cache range check.
///
/// Memory management
/// ─────────────────
/// Trie nodes are allocated from the system heap (::operator new) and are
/// never deallocated individually.  Destruction of the entire trie
/// (`~ncrit_trie`) recursively frees all nodes.  Because nodes are never
/// individually freed, there is no ABA problem and no epoch/hazard-pointer
/// machinery is needed for correctness.  This is safe because:
///   • Inserters only ever ADD new nodes (never remove).
///   • Leaf slots are updated atomically; a stale reader at most sees an
///     old value, not a freed pointer.
///   • Unregistration (remove_range) tombstones leaf slots to 0; the node
///     itself remains allocated.
///
/// Usage
/// ─────
/// \code
///   ncrit_trie<sub_segment*> trie;
///   trie.insert_range(base_addr, size, &seg);
///   sub_segment* s = trie.lookup(some_addr);   // nullptr if not registered
///   trie.remove_range(base_addr, size);
/// \endcode

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <new>

#include "detail/platform.hpp"

namespace segmented_interprocess {

// ============================================================================
// ncrit_trie
// ============================================================================

/// \brief Lock-free, page-granular address → Value radix trie.
///
/// \tparam Value     Must be a pointer type whose natural alignment ≥ 2,
///                   so that bit 0 is always 0 in a valid value pointer.
///                   `nullptr` is not a valid stored value; it represents
///                   "no mapping" (same as an absent entry).
/// \tparam Stride    Bits consumed per trie level (default 9 → fanout 512).
/// \tparam Levels    Number of trie levels (default 4 → 36-bit page index).
template<
    class Value,
    unsigned Stride = 9,
    unsigned Levels = 4
>
class ncrit_trie {
    static_assert(Levels >= 1, "Need at least 1 level");
    static_assert(Stride >= 1 && Stride <= 16,
                  "Stride must be in [1, 16]");

public:
    static constexpr unsigned kStride   = Stride;
    static constexpr unsigned kLevels   = Levels;
    static constexpr unsigned kFanOut   = 1u << Stride;
    static constexpr uintptr_t kIdxMask = kFanOut - 1u;

    /// Sentinel bit used to tag leaf values (distinguishes them from
    /// internal-node pointers in parent slots).
    static constexpr uintptr_t kLeafTag = uintptr_t(1);

    // -----------------------------------------------------------------------
    // Node
    // -----------------------------------------------------------------------
    /// A trie node: an array of kFanOut atomic pointer-sized slots.
    /// Aligned to 64 bytes so that the first cache line is shared by the
    /// first 8 slots, and subsequent cache lines cover contiguous groups.
    struct alignas(64) node_t {
        std::atomic<uintptr_t> ch[kFanOut];

        node_t() noexcept {
            for (auto& a : ch)
                a.store(0, std::memory_order_relaxed);
        }

        // Non-copyable — nodes are owned by the trie
        node_t(const node_t&) = delete;
        node_t& operator=(const node_t&) = delete;
    };

    // -----------------------------------------------------------------------
    // Thread-local cache
    // -----------------------------------------------------------------------
    /// Per-thread cache of the most recently used sub-segment range.
    /// The cache is valid when seg != nullptr.
    struct tls_entry {
        uintptr_t               base = 0;
        uintptr_t               end  = 0;
        Value                   seg  = nullptr;
        std::atomic<uintptr_t>* leaf = nullptr;
    };

    struct tls_cache_t {
        static constexpr unsigned kSize = 16;
        tls_entry entries[kSize];

        // Retrieve an entry and move it to the front if found
        tls_entry* hit(uintptr_t addr, uintptr_t expected_tag) {
            for (unsigned i = 0; i < kSize; ++i) {
                if (addr >= entries[i].base && addr < entries[i].end) {
                    // Lockless passive eviction check:
                    // Verify the leaf slot still contains the segment pointer.
                    // If remove_range was called, it zeroed this slot.
                    if (entries[i].leaf && entries[i].leaf->load(std::memory_order_acquire) == expected_tag) {
                        if (i > 0) {
                            // Move to front (LRU)
                            tls_entry temp = entries[i];
                            for (unsigned j = i; j > 0; --j) {
                                entries[j] = entries[j - 1];
                            }
                            entries[0] = temp;
                        }
                        return &entries[0];
                    } else {
                        // Stale entry: segment was unmapped. Evict it.
                        for (unsigned j = i; j < kSize - 1; ++j) {
                            entries[j] = entries[j + 1];
                        }
                        entries[kSize - 1] = {};
                        return nullptr;
                    }
                }
            }
            return nullptr;
        }

        void insert(uintptr_t base, uintptr_t end, Value seg, std::atomic<uintptr_t>* leaf) {
            // Shift down
            for (unsigned i = kSize - 1; i > 0; --i) {
                entries[i] = entries[i - 1];
            }
            entries[0] = {base, end, seg, leaf};
        }

        void invalidate() {
            for (unsigned i = 0; i < kSize; ++i) {
                entries[i] = {};
            }
        }
    };

    // -----------------------------------------------------------------------
    // Construction / Destruction
    // -----------------------------------------------------------------------
    ncrit_trie() : root_(new node_t{}) {}

    ~ncrit_trie() {
        destroy_subtree(root_, 0);
    }

    // Non-copyable / non-moveable (atomics inside)
    ncrit_trie(const ncrit_trie&) = delete;
    ncrit_trie& operator=(const ncrit_trie&) = delete;

    // -----------------------------------------------------------------------
    // Public interface
    // -----------------------------------------------------------------------

    /// Register all pages covering [base, base+size) with `value`.
    /// Concurrent calls to insert_range and lookup are safe.
    /// Concurrent calls to insert_range and remove_range for overlapping
    /// ranges are safe but produce unspecified (last-writer-wins) results.
    ///
    /// \pre  value != nullptr
    void insert_range(uintptr_t base, std::size_t size, Value value) noexcept {
        assert(value != nullptr);
        const unsigned ps = detail::page_shift();
        const uintptr_t page_begin = base >> ps;
        const uintptr_t page_end =
            (base + size + ((uintptr_t(1) << ps) - 1u)) >> ps;
        for (uintptr_t pg = page_begin; pg < page_end; ++pg)
            insert_leaf(pg, value);
        invalidate_cache();
    }

    /// Remove all page registrations in [base, base+size).
    /// After this call, lookup() for any address in [base, base+size) returns
    /// nullptr (unless a concurrent insert_range races and wins).
    void remove_range(uintptr_t base, std::size_t size) noexcept {
        const unsigned ps = detail::page_shift();
        const uintptr_t page_begin = base >> ps;
        const uintptr_t page_end =
            (base + size + ((uintptr_t(1) << ps) - 1u)) >> ps;
        for (uintptr_t pg = page_begin; pg < page_end; ++pg)
            clear_leaf(pg);
        invalidate_cache();
    }

    /// Return the Value registered for the page containing `addr`, or nullptr.
    ///
    /// Uses the thread-local cache: if `addr` falls within the cached range,
    /// returns immediately without touching the trie.  Otherwise performs a
    /// full trie traversal and updates the cache.
    ///
    /// Complexity: O(Levels) atomic acquire-loads, plus O(1) cache check.
    Value lookup(uintptr_t addr) const noexcept {
        return lookup_impl(addr);
    }

private:
    Value lookup_impl(uintptr_t addr) const noexcept {
        tls_cache_t& cache = tls_cache_();
        for (unsigned i = 0; i < tls_cache_t::kSize; ++i) {
            tls_entry& entry = cache.entries[i];
            if (addr >= entry.base && addr < entry.end) {
                uintptr_t expected_tag = reinterpret_cast<uintptr_t>(entry.seg) | kLeafTag;
                if (entry.leaf && entry.leaf->load(std::memory_order_acquire) == expected_tag) {
                    if (i > 0) {
                        tls_entry temp = entry;
                        for (unsigned j = i; j > 0; --j) cache.entries[j] = cache.entries[j - 1];
                        cache.entries[0] = temp;
                    }
                    return cache.entries[0].seg;
                } else {
                    for (unsigned j = i; j < tls_cache_t::kSize - 1; ++j) cache.entries[j] = cache.entries[j + 1];
                    cache.entries[tls_cache_t::kSize - 1] = {};
                    return nullptr; // Was stale, evicted. Must fall back to full traversal.
                }
            }
        }

        // --- Full trie traversal ---
        const unsigned ps = detail::page_shift();
        std::atomic<uintptr_t>* leaf_slot = nullptr;
        Value v = lookup_page_with_slot(addr >> ps, &leaf_slot);

        // Update cache
        if (v != nullptr) {
            uintptr_t base = (addr >> ps) << ps;
            uintptr_t end  = base + (uintptr_t(1) << ps);
            cache.insert(base, end, v, leaf_slot);
        }
        return v;
    }

    static tls_cache_t& tls_cache_() noexcept {
        static thread_local tls_cache_t cache{};
        return cache;
    }

public:
    /// Expands the bounds of the most recently accessed (index 0) cache entry
    /// if its segment pointer matches the provided `seg`.
    static void expand_cache_bounds_top(uintptr_t base, std::size_t size, Value seg) noexcept {
        tls_cache_t& cache = tls_cache_();
        if (cache.entries[0].seg == seg) {
            cache.entries[0].base = base;
            cache.entries[0].end  = base + size;
        }
    }

    /// Invalidate the TLS cache for the current thread.
    /// Call when a segment is unregistered.
    static void invalidate_cache() noexcept {
        tls_cache_().invalidate();
    }



    // -----------------------------------------------------------------------
    // Low-level: raw page-number lookup (no cache)
    // -----------------------------------------------------------------------
    Value lookup_page(uintptr_t page_num) const noexcept {
        std::atomic<uintptr_t>* leaf_slot = nullptr;
        return lookup_page_with_slot(page_num, &leaf_slot);
    }

    Value lookup_page_with_slot(uintptr_t page_num, std::atomic<uintptr_t>** out_leaf_slot) const noexcept {
        const node_t* cur = root_;
        for (unsigned d = 0; d < Levels - 1u; ++d) {
            const unsigned idx  = level_idx(page_num, d);
            const uintptr_t raw = cur->ch[idx].load(std::memory_order_acquire);
            if (raw == 0) return nullptr;
            cur = reinterpret_cast<const node_t*>(raw);
        }
        const unsigned idx = level_idx(page_num, Levels - 1u);
        const std::atomic<uintptr_t>& leaf_ref = cur->ch[idx];
        const uintptr_t raw = leaf_ref.load(std::memory_order_acquire);
        if (raw == 0) return nullptr;
        if (out_leaf_slot) *out_leaf_slot = const_cast<std::atomic<uintptr_t>*>(&leaf_ref);
        return reinterpret_cast<Value>(raw & ~kLeafTag);
    }

    // -----------------------------------------------------------------------
    // Statistics (diagnostic)
    // -----------------------------------------------------------------------
    /// Count the number of allocated nodes (recursive, for diagnostics only).
    std::size_t node_count() const noexcept {
        return count_nodes(root_, 0);
    }

private:
    // -----------------------------------------------------------------------
    // Index helpers
    // -----------------------------------------------------------------------

    /// Extract the `Stride`-bit index for `depth` from a page number.
    /// Depth 0 uses the most-significant bits; depth Levels-1 the least.
    static unsigned level_idx(uintptr_t page_num, unsigned depth) noexcept {
        const unsigned shift = (Levels - 1u - depth) * Stride;
        return static_cast<unsigned>((page_num >> shift) & kIdxMask);
    }

    // -----------------------------------------------------------------------
    // Mutation helpers
    // -----------------------------------------------------------------------

    /// Insert a single page-number → value mapping.
    void insert_leaf(uintptr_t page_num, Value value) noexcept {
        node_t* cur = root_;

        // Walk to the parent of the leaf level, creating nodes as needed.
        for (unsigned d = 0; d < Levels - 1u; ++d) {
            const unsigned idx  = level_idx(page_num, d);
            uintptr_t child = cur->ch[idx].load(std::memory_order_acquire);

            if (child == 0) {
                // Allocate a new child node
                node_t* fresh = new (std::nothrow) node_t{};
                if (!fresh) return; // OOM — best-effort

                const uintptr_t desired = reinterpret_cast<uintptr_t>(fresh);
                uintptr_t expected = 0;

                if (!cur->ch[idx].compare_exchange_strong(
                        expected, desired,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    // Lost the race: another thread installed a node first.
                    delete fresh;
                    child = expected; // Winner's node (acquire guarantees visibility)
                } else {
                    child = desired;
                }
            }

            cur = reinterpret_cast<node_t*>(child);
        }

        // Leaf slot: store tagged value with release so any subsequent
        // acquire-load sees the value.
        const unsigned  idx = level_idx(page_num, Levels - 1u);
        const uintptr_t tag = reinterpret_cast<uintptr_t>(value) | kLeafTag;
        cur->ch[idx].store(tag, std::memory_order_release);
    }

    /// Tombstone a single page-number mapping (set to 0 = absent).
    void clear_leaf(uintptr_t page_num) noexcept {
        node_t* cur = root_;
        for (unsigned d = 0; d < Levels - 1u; ++d) {
            const unsigned  idx   = level_idx(page_num, d);
            const uintptr_t child = cur->ch[idx].load(std::memory_order_acquire);
            if (child == 0) return; // Node path does not exist — nothing to clear
            cur = reinterpret_cast<node_t*>(child);
        }
        const unsigned idx = level_idx(page_num, Levels - 1u);
        cur->ch[idx].store(0, std::memory_order_release);
    }

    // -----------------------------------------------------------------------
    // Memory management
    // -----------------------------------------------------------------------

    /// Recursively free all nodes in the subtree rooted at `node`.
    static void destroy_subtree(node_t* node, unsigned depth) noexcept {
        if (!node) return;
        if (depth < Levels - 1u) {
            for (unsigned i = 0; i < kFanOut; ++i) {
                const uintptr_t raw =
                    node->ch[i].load(std::memory_order_relaxed);
                if (raw != 0)
                    destroy_subtree(reinterpret_cast<node_t*>(raw), depth + 1u);
            }
        }
        delete node;
    }

    std::size_t count_nodes(const node_t* node, unsigned depth) const noexcept {
        if (!node) return 0;
        std::size_t total = 1;
        if (depth < Levels - 1u) {
            for (unsigned i = 0; i < kFanOut; ++i) {
                const uintptr_t raw =
                    node->ch[i].load(std::memory_order_relaxed);
                if (raw != 0)
                    total += count_nodes(reinterpret_cast<const node_t*>(raw),
                                         depth + 1u);
            }
        }
        return total;
    }



    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    node_t* const root_;
};

} // namespace segmented_interprocess
