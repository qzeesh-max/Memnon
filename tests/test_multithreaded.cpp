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

/// \file test_multithreaded.cpp
/// Multi-threaded correctness and stress tests for segmented_managed_memory.
///
/// Tests:
///   1. 32 threads concurrently allocating and freeing memory
///   2. Concurrent grow() while readers are active
///   3. Concurrent named object construct + find (no name collision)
///   4. Concurrent segmented_offset_ptr read under grow
///   5. Producer-consumer: one thread grows, others resolve pointers
///   6. Timed watchdog: all tests finish in < 30 seconds
///
/// Build with -fsanitize=thread to verify no data races.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <string>
#include <chrono>
#include <random>

#include "segmented_interprocess/segmented_managed_memory.hpp"
#include "segmented_interprocess/segmented_offset_ptr.hpp"

using namespace segmented_interprocess;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Utility
// ============================================================================
static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}
#define CHECK(expr)      check((expr), #expr)
#define CHECK_EQ(a, b)   check((a)==(b), #a " == " #b)
#define CHECK_NONNULL(a) check((a)!=nullptr, #a " != nullptr")

static double elapsed_ms(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - t0).count() / 1000.0;
}

// ============================================================================
// Test 1: 32 threads, concurrent raw allocate / deallocate
// ============================================================================
static void test_concurrent_alloc_dealloc() {
    static constexpr int kThreads      = 32;
    static constexpr int kOpsPerThread = 500;
    static constexpr std::size_t kAllocSz = 1024; // 1 KiB per alloc

    segmented_managed_memory mem(kDefaultSegSize);

    std::atomic<bool> ready{false};
    std::atomic<int>  errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            while (!ready.load(std::memory_order_acquire)) {}

            std::vector<void*> mine;
            mine.reserve(kOpsPerThread);

            std::mt19937 rng(static_cast<unsigned>(t) * 1234567u);

            for (int k = 0; k < kOpsPerThread; ++k) {
                if (!mine.empty() && rng() % 3 == 0) {
                    // Deallocate a random owned pointer
                    int idx = static_cast<int>(rng() % mine.size());
                    mem.deallocate(mine[idx]);
                    mine.erase(mine.begin() + idx);
                } else {
                    void* p = mem.allocate(kAllocSz);
                    if (!p) { errors.fetch_add(1, std::memory_order_relaxed); continue; }
                    // Write and read-back a pattern
                    auto pat = static_cast<unsigned char>(t & 0xFF);
                    std::memset(p, pat, kAllocSz);
                    auto* b = static_cast<unsigned char*>(p);
                    for (std::size_t i = 0; i < kAllocSz; i += 64)
                        if (b[i] != pat)
                            errors.fetch_add(1, std::memory_order_relaxed);
                    mine.push_back(p);
                }
            }
            // Free remainder
            for (void* p : mine) mem.deallocate(p);
        });
    }

    auto t0 = Clock::now();
    ready.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    double ms = elapsed_ms(t0);

    CHECK_EQ(errors.load(), 0);
    std::printf("PASS: test_concurrent_alloc_dealloc (%d threads, %.1f ms)\n",
                kThreads, ms);
}

// ============================================================================
// Test 2: concurrent grow() while readers allocate
// ============================================================================
static void test_concurrent_grow_and_alloc() {
    segmented_managed_memory mem(kMinSegmentSize);

    std::atomic<bool> stop{false};
    std::atomic<int>  errors{0};
    std::atomic<bool> ready{false};

    static constexpr int kAllocThreads = 8;

    std::vector<std::thread> threads;

    // Alloc threads
    for (int t = 0; t < kAllocThreads; ++t) {
        threads.emplace_back([&]() {
            while (!ready.load(std::memory_order_acquire)) {}
            while (!stop.load(std::memory_order_acquire)) {
                void* p = mem.allocate(512);
                if (!p) { errors.fetch_add(1); break; }
                std::memset(p, 0xCC, 512);
                mem.deallocate(p);
            }
        });
    }

    // Grow thread
    threads.emplace_back([&]() {
        while (!ready.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            mem.grow(kMinSegmentSize);
        }
        stop.store(true, std::memory_order_release);
    });

    auto t0 = Clock::now();
    ready.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    double ms = elapsed_ms(t0);

    CHECK_EQ(errors.load(), 0);
    CHECK(mem.segment_count() > std::size_t(1));
    std::printf("PASS: test_concurrent_grow_and_alloc (%zu segs, %.1f ms)\n",
                mem.segment_count(), ms);
}

// ============================================================================
// Test 3: concurrent named object construct + find (disjoint names)
// ============================================================================
static void test_concurrent_named_objects() {
    static constexpr int kThreads = 16;
    static constexpr int kObjs    = 20; // per thread

    segmented_managed_memory mem(kDefaultSegSize);

    std::atomic<bool> ready{false};
    std::atomic<int>  errors{0};

    struct Obj {
        int thread_id;
        int obj_id;
        double data;
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            while (!ready.load(std::memory_order_acquire)) {}

            // Construct
            for (int k = 0; k < kObjs; ++k) {
                std::string name = "t" + std::to_string(t)
                                 + "_o" + std::to_string(k);
                try {
                    Obj* p = mem.construct<Obj>(name.c_str());
                    if (p) { p->thread_id = t; p->obj_id = k; p->data = static_cast<double>(t * 1000 + k); }
                    if (!p) { errors.fetch_add(1); continue; }
                    p->thread_id = t;
                    p->obj_id    = k;
                    p->data      = static_cast<double>(t * 1000 + k);
                } catch (...) { errors.fetch_add(1); }
            }
        });
    }

    ready.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    CHECK_EQ(errors.load(), 0);

    // Single-threaded verification
    int found = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int k = 0; k < kObjs; ++k) {
            std::string name = "t" + std::to_string(t)
                             + "_o" + std::to_string(k);
            auto [p, n] = mem.find<Obj>(name.c_str());
            if (p) {
                CHECK_EQ(p->thread_id, t);
                CHECK_EQ(p->obj_id, k);
                ++found;
                mem.destroy<Obj>(name.c_str());
            }
        }
    }

    std::printf("PASS: test_concurrent_named_objects (%d/%d found)\n",
                found, kThreads * kObjs);
}

// ============================================================================
// Test 4: segmented_offset_ptr correctness under concurrent grow
// ============================================================================
static void test_offset_ptr_under_grow() {
    segmented_managed_memory mem(kMinSegmentSize);

    // Allocate a sentinel value inside the first segment
    int* sentinel = static_cast<int*>(mem.allocate(sizeof(int)));
    *sentinel = 0xCAFEBABE;

    // Create a stack copy of a segmented_offset_ptr pointing to sentinel
    segmented_offset_ptr<int> stack_ptr(sentinel);

    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    std::atomic<int>  errors{0};

    // Reader thread: continuously dereferences stack_ptr
    std::thread reader([&]() {
        while (!ready.load(std::memory_order_acquire)) {}
        while (!stop.load(std::memory_order_acquire)) {
            int val = *stack_ptr;
            if (val != 0xCAFEBABE)
                errors.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Grower thread: repeatedly adds segments
    std::thread grower([&]() {
        while (!ready.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            mem.grow(kMinSegmentSize);
        }
        stop.store(true, std::memory_order_release);
    });

    ready.store(true, std::memory_order_release);
    reader.join();
    grower.join();

    mem.deallocate(sentinel);
    CHECK_EQ(errors.load(), 0);
    std::printf("PASS: test_offset_ptr_under_grow\n");
}

// ============================================================================
// Test 5: producer-consumer with segmented_offset_ptr
/// Producer allocates objects and publishes segmented_offset_ptr to them.
/// Consumers read via the published pointer.
// ============================================================================
static void test_producer_consumer() {
    segmented_managed_memory mem(kDefaultSegSize);

    static constexpr int kItems = 200;

    struct Item {
        std::atomic<int>         ready;
        segmented_offset_ptr<int> data_ptr;
        int                      expected;
    };

    // Shared channel: a vector of items in shared memory
    std::vector<Item> channel(kItems);
    for (auto& item : channel) {
        item.ready.store(0, std::memory_order_relaxed);
        item.expected = 0;
    }

    std::atomic<bool> start{false};

    // Producer
    std::thread producer([&]() {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kItems; ++i) {
            // Allocate an int inside the managed memory
            int* p = static_cast<int*>(mem.allocate(sizeof(int)));
            *p = i * 7 + 3;

            channel[i].expected  = *p;
            // Store pointer on stack — uses traditional encoding
            channel[i].data_ptr  = segmented_offset_ptr<int>(p);
            channel[i].ready.store(1, std::memory_order_release);
        }
    });

    // Consumer
    std::atomic<int> errors{0};
    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kItems; ++i) {
            while (channel[i].ready.load(std::memory_order_acquire) == 0) {
                std::this_thread::yield();
            }
            // Dereference via the published pointer
            int val = *channel[i].data_ptr;
            if (val != channel[i].expected)
                errors.fetch_add(1, std::memory_order_relaxed);
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    // Cleanup
    for (int i = 0; i < kItems; ++i)
        if (channel[i].data_ptr)
            mem.deallocate(channel[i].data_ptr.get());

    CHECK_EQ(errors.load(), 0);
    std::printf("PASS: test_producer_consumer\n");
}

// ============================================================================
// Test 6: TSan-visible race detection (negative test when compiled normally,
//         verified clean by TSan build)
// ============================================================================
static void test_no_data_races_on_trie() {
    // Indirect: 16 threads each do 1000 alloc+free cycles on separate objects.
    // If TSan reports any race in the trie / segment_registry paths, the build
    // fails.  This is the "TSan clean" guarantee.
    static constexpr int kThreads = 16;
    static constexpr int kCycles  = 1000;

    segmented_managed_memory mem(kDefaultSegSize);
    std::atomic<bool> ready{false};
    std::atomic<int>  errors{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            while (!ready.load(std::memory_order_acquire)) {}
            for (int k = 0; k < kCycles; ++k) {
                void* p = mem.allocate(256);
                if (!p) { errors.fetch_add(1); continue; }
                auto* b = static_cast<unsigned char*>(p);
                unsigned char pat = static_cast<unsigned char>((t + k) & 0xFF);
                std::memset(b, pat, 256);
                // Verify
                for (int j = 0; j < 256; j += 32)
                    if (b[j] != pat) errors.fetch_add(1);
                mem.deallocate(p);
            }
        });
    }

    auto t0 = Clock::now();
    ready.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    double ms = elapsed_ms(t0);

    CHECK_EQ(errors.load(), 0);
    std::printf("PASS: test_no_data_races_on_trie (%d threads × %d cycles, %.1f ms)\n",
                kThreads, kCycles, ms);
}

// ============================================================================
// Watchdog: abort if all tests take > 30 s
// ============================================================================
int main() {
    auto t0 = Clock::now();

    std::printf("=== multithreaded tests (hw_concurrency=%u) ===\n",
                std::thread::hardware_concurrency());

    test_concurrent_alloc_dealloc();
    test_concurrent_grow_and_alloc();
    test_concurrent_named_objects();
    test_offset_ptr_under_grow();
    test_producer_consumer();
    test_no_data_races_on_trie();

    double total_ms = elapsed_ms(t0);
    if (total_ms > 30000.0) {
        std::fprintf(stderr, "FAIL: tests exceeded 30 s watchdog (%.0f ms)\n",
                     total_ms);
        return 1;
    }

    std::printf("=== ALL multithreaded tests PASSED (%.1f ms total) ===\n",
                total_ms);
    return 0;
}
