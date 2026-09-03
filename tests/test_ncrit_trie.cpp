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

/// \file test_ncrit_trie.cpp
/// Unit + performance tests for ncrit_trie.
///
/// Tests:
///   1. Single-threaded insert / lookup correctness
///   2. Range insert covering multiple page boundaries
///   3. Lookup outside registered range returns nullptr
///   4. remove_range clears entries
///   5. Overlapping insert (last writer wins)
///   6. Concurrent 16-thread insert + lookup stress
///   7. Performance benchmark: single-threaded lookups

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include <functional>

#include "segmented_interprocess/ncrit_trie.hpp"
#include "segmented_interprocess/detail/platform.hpp"

using namespace segmented_interprocess;

// Helper: a dummy "sub_segment-like" struct aligned to > 1 byte
struct dummy_seg {
    uintptr_t id;
    uintptr_t base;
    std::size_t size;
};

using trie_t = ncrit_trie<dummy_seg*>;

// ============================================================================
// Utility
// ============================================================================
static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

#define CHECK(expr) check((expr), #expr)
#define CHECK_EQ(a, b) check((a)==(b), #a " == " #b)
#define CHECK_NE(a, b) check((a)!=(b), #a " != " #b)
#define CHECK_NULL(a)  check((a)==nullptr, #a " == nullptr")
#define CHECK_NONNULL(a) check((a)!=nullptr, #a " != nullptr")


// ============================================================================
// Test 1: basic single-page insert and lookup
// ============================================================================
static void test_single_page_insert_lookup() {
    trie_t trie;
    dummy_seg seg{42, 0, 0};

    // Use a well-aligned fake address that is not 0 or 1
    const uintptr_t base = detail::page_size() * 10;

    trie.insert_range(base, detail::page_size(), &seg);
    CHECK_EQ(trie.lookup(base), &seg);
    CHECK_EQ(trie.lookup(base + detail::page_size() / 2), &seg);
    // One byte past the end → different page → nullptr
    CHECK_NULL(trie.lookup(base + detail::page_size()));
    // Before the range
    CHECK_NULL(trie.lookup(base - 1));

    std::printf("PASS: test_single_page_insert_lookup\n");
}

// ============================================================================
// Test 2: multi-page range insert
// ============================================================================
static void test_multi_page_range() {
    trie_t trie;
    dummy_seg seg{1, 0, 0};

    const std::size_t ps = detail::page_size();
    const uintptr_t base = ps * 100;
    const std::size_t sz = ps * 32; // 32 pages

    trie.insert_range(base, sz, &seg);

    // Every byte in the range maps to &seg
    for (std::size_t i = 0; i < 32; ++i) {
        uintptr_t addr = base + i * ps;
        CHECK_EQ(trie.lookup(addr), &seg);
        CHECK_EQ(trie.lookup(addr + ps / 2), &seg);
        CHECK_EQ(trie.lookup(addr + ps - 1), &seg);
    }

    // Just outside
    CHECK_NULL(trie.lookup(base - 1));
    CHECK_NULL(trie.lookup(base + sz));

    std::printf("PASS: test_multi_page_range\n");
}

// ============================================================================
// Test 3: lookup outside range → nullptr
// ============================================================================
static void test_lookup_outside_range() {
    trie_t trie;
    const std::size_t ps = detail::page_size();
    CHECK_NULL(trie.lookup(0));
    CHECK_NULL(trie.lookup(ps * 999999));
    CHECK_NULL(trie.lookup(static_cast<uintptr_t>(-1)));

    std::printf("PASS: test_lookup_outside_range\n");
}

// ============================================================================
// Test 4: remove_range
// ============================================================================
static void test_remove_range() {
    trie_t trie;
    dummy_seg seg{7, 0, 0};

    const std::size_t ps = detail::page_size();
    const uintptr_t base = ps * 200;
    const std::size_t sz = ps * 8;

    trie.insert_range(base, sz, &seg);
    CHECK_EQ(trie.lookup(base), &seg);

    trie.remove_range(base, sz);
    CHECK_NULL(trie.lookup(base));
    CHECK_NULL(trie.lookup(base + sz / 2));

    std::printf("PASS: test_remove_range\n");
}

// ============================================================================
// Test 5: overlapping insert (last writer wins)
// ============================================================================
static void test_overlapping_insert() {
    trie_t trie;
    dummy_seg segA{10, 0, 0};
    dummy_seg segB{20, 0, 0};

    const std::size_t ps = detail::page_size();
    const uintptr_t base = ps * 300;
    const std::size_t sz = ps * 4;

    trie.insert_range(base, sz, &segA);
    CHECK_EQ(trie.lookup(base), &segA);

    // Overwrite with segB
    trie.insert_range(base, sz, &segB);
    CHECK_EQ(trie.lookup(base), &segB);

    std::printf("PASS: test_overlapping_insert\n");
}

// ============================================================================
// Test 6: concurrent 16-thread insert + lookup
// ============================================================================
static void test_concurrent_insert_lookup() {
    static constexpr int kThreads    = 16;
    static constexpr int kOpsPerThread = 10'000;
    static constexpr int kSegments   = 8;
    static constexpr std::size_t kSegPages = 16;

    const std::size_t ps = detail::page_size();

    // Allocate segment descriptors and register regions
    std::vector<dummy_seg> segs(kSegments);
    std::vector<uintptr_t> bases(kSegments);
    for (int i = 0; i < kSegments; ++i) {
        segs[i].id   = static_cast<uintptr_t>(i + 1);
        bases[i] = ps * static_cast<uintptr_t>(1000 + i * (kSegPages + 2));
    }

    trie_t trie;

    // Phase 1: inserters (8 threads) + lookers (8 threads) concurrently
    std::atomic<int> errors{0};
    std::atomic<bool> ready{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    // 8 inserter threads
    for (int t = 0; t < kSegments; ++t) {
        threads.emplace_back([&, t]() {
            while (!ready.load(std::memory_order_acquire)) {}
            for (int k = 0; k < kOpsPerThread; ++k) {
                trie.insert_range(bases[t], kSegPages * ps, &segs[t]);
            }
        });
    }

    // 8 lookup threads
    for (int t = 0; t < kSegments; ++t) {
        threads.emplace_back([&, t]() {
            while (!ready.load(std::memory_order_acquire)) {}
            for (int k = 0; k < kOpsPerThread; ++k) {
                uintptr_t addr = bases[t] + (static_cast<uintptr_t>(k) % kSegPages) * ps;
                dummy_seg* result = trie.lookup(addr);
                // May be null (not yet inserted) or equal to &segs[t]
                if (result != nullptr && result != &segs[t]) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    ready.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    CHECK_EQ(errors.load(), 0);

    // Verify final state
    for (int i = 0; i < kSegments; ++i) {
        for (std::size_t p = 0; p < kSegPages; ++p) {
            CHECK_EQ(trie.lookup(bases[i] + p * ps), &segs[i]);
        }
    }

    std::printf("PASS: test_concurrent_insert_lookup\n");
}

// ============================================================================
// Test 7: TLS cache correctness across multiple lookups
// ============================================================================
static void test_tls_cache() {
    trie_t trie;
    dummy_seg seg{99, 0, 0};

    const std::size_t ps = detail::page_size();
    const uintptr_t base = ps * 500;
    const std::size_t sz = ps * 4;

    trie.insert_range(base, sz, &seg);

    // Successive lookups within the same page should all return &seg
    for (int i = 0; i < 1000; ++i) {
        uintptr_t addr = base + (static_cast<uintptr_t>(i) % sz);
        CHECK_EQ(trie.lookup(addr), &seg);
    }

    // After remove, cache should be invalidated (cache miss → trie traversal)
    trie.remove_range(base, sz);
    CHECK_NULL(trie.lookup(base));

    std::printf("PASS: test_tls_cache\n");
}

// ============================================================================
// Test 8: LRU cache lockless passive eviction
// ============================================================================
static void test_lru_cache_passive_eviction() {
    trie_t trie;
    dummy_seg seg1{1, 0, 0};
    dummy_seg seg2{2, 0, 0};

    const std::size_t ps = detail::page_size();
    const uintptr_t base1 = ps * 600;
    const std::size_t sz1 = ps * 4;

    const uintptr_t base2 = ps * 700;
    const std::size_t sz2 = ps * 4;

    trie.insert_range(base1, sz1, &seg1);
    trie.insert_range(base2, sz2, &seg2);

    // Warm cache with both segments
    CHECK_EQ(trie.lookup(base1), &seg1);
    CHECK_EQ(trie.lookup(base2), &seg2);

    // Remove base1; this zero out the trie leaf slot locklessly
    trie.remove_range(base1, sz1);

    // Lookup base1 again. LRU cache must check the leaf slot,
    // see it's zeroed, evict it locklessly, and fall back to trie which returns null.
    CHECK_NULL(trie.lookup(base1));

    // Lookup base2 again. Should hit cache correctly.
    CHECK_EQ(trie.lookup(base2), &seg2);

    std::printf("PASS: test_lru_cache_passive_eviction\n");
}

// ============================================================================
// Test 9: performance benchmark (single-threaded)
// ============================================================================
static void bench_lookup() {
    trie_t trie;
    dummy_seg seg{1, 0, 0};

    const std::size_t ps = detail::page_size();
    const uintptr_t base = ps * 1000;
    const std::size_t sz = ps * 64; // 64 pages

    trie.insert_range(base, sz, &seg);

    // Warm up
    for (int i = 0; i < 10000; ++i)
        (void)trie.lookup(base + static_cast<uintptr_t>(i % sz));

    const int kIter = 10'000'000;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kIter; ++i) {
        // Vary address to stress-test cache + trie paths
        uintptr_t addr = base + ((static_cast<uintptr_t>(i) * 16384) % sz);
        volatile dummy_seg* r = trie.lookup(addr);
        (void)r;
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double ns_per_op = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t1 - t0).count() / static_cast<double>(kIter);

    std::printf("BENCH: lookup = %.1f ns/op (target < 80 ns/op)\n", ns_per_op);
    // Soft target: < 80 ns/op on arm64 MacBook (4-level trie + TLS cache)
    // Don't fail the test on timing (CI machines vary)
    if (ns_per_op > 200.0)
        std::printf("WARNING: lookup latency exceeds 200 ns/op\n");
}

// ============================================================================
// Test 9: large address (near top of 47-bit user VA space on macOS arm64)
// ============================================================================
static void test_large_address() {
    trie_t trie;
    dummy_seg seg{123, 0, 0};

    const std::size_t ps = detail::page_size();
    // Use a large address within the 36-bit page-number coverage of the trie
    // (Stride=9, Levels=4 covers up to 2^36 pages = 2^(36+14) = 2^50 bytes)
    // Use 2^45 bytes as base (well within range)
    const uintptr_t base = (uintptr_t(1) << 45) & ~(ps - 1u);

    trie.insert_range(base, ps * 2, &seg);
    CHECK_EQ(trie.lookup(base), &seg);
    CHECK_EQ(trie.lookup(base + ps), &seg);
    CHECK_NULL(trie.lookup(base + ps * 2));

    std::printf("PASS: test_large_address\n");
}

// ============================================================================
// main
// ============================================================================
int main() {
    std::printf("=== ncrit_trie tests (page_size=%zu, page_shift=%u) ===\n",
                detail::page_size(), detail::page_shift());

    test_single_page_insert_lookup();
    test_multi_page_range();
    test_lookup_outside_range();
    test_remove_range();
    test_overlapping_insert();
    test_tls_cache();
    test_concurrent_insert_lookup();
    test_lru_cache_passive_eviction();
    test_large_address();
    bench_lookup();

    std::printf("=== ALL ncrit_trie tests PASSED ===\n");
    return 0;
}
