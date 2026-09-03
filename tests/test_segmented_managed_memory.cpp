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

/// \file test_segmented_managed_memory.cpp
/// Integration tests for segmented_managed_memory.
///
/// Tests:
///   1. Basic construction and named object lifecycle
///   2. Allocate until first sub-segment exhausted → auto-growth
///   3. find() on a non-existent name returns nullptr
///   4. Destroy removes the named object
///   5. get_free_memory() and get_size() accounting
///   6. explicit grow() adds capacity
///   7. Large object that forces growth
///   8. Multiple named objects across sub-segments
///   9. Allocator interface (segmented_allocator)

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "segmented_interprocess/segmented_managed_memory.hpp"

using namespace segmented_interprocess;


#include <thread>
#include <chrono>


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
#define CHECK_NULL(a)    check((a)==nullptr, #a " == nullptr")
#define CHECK_NONNULL(a) check((a)!=nullptr, #a " != nullptr")

// ============================================================================
// Test struct
// ============================================================================
struct Point {
    double x, y, z;
    int    id;
    explicit Point(int i) : x(i*1.0), y(i*2.0), z(i*3.0), id(i) {}
};

// ============================================================================
// Test 1: basic construct / find / destroy
// ============================================================================
static void test_basic_lifecycle() {
    segmented_managed_memory mem(kMinSegmentSize * 2);

    // Construct a named Point
    Point* p = mem.construct<Point>("my_point", 42);
    CHECK_NONNULL(p);
    CHECK_EQ(p->id, 42);
    CHECK_EQ(p->x, 42.0);

    // Find it
    auto [found, cnt] = mem.find<Point>("my_point");
    CHECK_EQ(found, p);
    CHECK_EQ(cnt, std::size_t(1));

    // Find non-existent
    auto [missing, _] = mem.find<Point>("does_not_exist");
    CHECK_NULL(missing);

    // Destroy
    bool ok = mem.destroy<Point>("my_point");
    CHECK(ok);

    // Now find should return nullptr
    auto [gone, g] = mem.find<Point>("my_point");
    CHECK_NULL(gone);

    // Destroy again → false
    bool ok2 = mem.destroy<Point>("my_point");
    CHECK(!ok2);

    std::printf("PASS: test_basic_lifecycle\n");
}

// ============================================================================
// Test 2: auto-growth on exhaustion
// ============================================================================
static void test_auto_growth() {
    // Start with a tiny segment (1 MiB)
    segmented_managed_memory mem(kMinSegmentSize);
    std::size_t initial_segs = mem.segment_count();
    CHECK_EQ(initial_segs, std::size_t(1));

    // Allocate until we exhaust it (each allocation: 512 KiB)
    constexpr std::size_t alloc_size = 512u * 1024u;
    std::vector<void*> ptrs;
    bool grew = false;
    for (int i = 0; i < 10; ++i) {
        void* p = mem.allocate(alloc_size);
        CHECK_NONNULL(p);
        ptrs.push_back(p);
        if (mem.segment_count() > initial_segs) {
            grew = true;
            break;
        }
    }
    CHECK(grew);
    std::size_t segs_after = mem.segment_count();
    CHECK(segs_after > initial_segs);

    for (void* p : ptrs) mem.deallocate(p);

    std::printf("PASS: test_auto_growth (grew to %zu segments)\n", segs_after);
}

// ============================================================================
// Test 3: get_free_memory and get_size
// ============================================================================
static void test_capacity_accounting() {
    segmented_managed_memory mem(kDefaultSegSize);

    std::size_t free0 = mem.get_free_memory();
    std::size_t size0 = mem.get_size();
    CHECK(free0 > 0);
    CHECK(size0 > 0);
    CHECK(free0 < size0);

    void* p = mem.allocate(1024 * 1024); // 1 MiB
    CHECK_NONNULL(p);

    std::size_t free1 = mem.get_free_memory();
    CHECK(free1 < free0);

    mem.deallocate(p);
    std::size_t free2 = mem.get_free_memory();
    CHECK(free2 >= free1);

    std::printf("PASS: test_capacity_accounting\n");
}

// ============================================================================
// Test 4: explicit grow()
// ============================================================================
static void test_explicit_grow() {
    segmented_managed_memory mem(kMinSegmentSize);
    std::size_t segs0 = mem.segment_count();
    std::size_t free0 = mem.get_free_memory();

    mem.grow(kMinSegmentSize * 2);

    std::size_t segs1 = mem.segment_count();
    std::size_t free1 = mem.get_free_memory();
    CHECK(segs1 > segs0);
    CHECK(free1 > free0);

    std::printf("PASS: test_explicit_grow (free: %zu → %zu bytes)\n",
                free0, free1);
}

// ============================================================================
// Test 5: large object that forces growth
// ============================================================================
static void test_large_object_growth() {
    segmented_managed_memory mem(kMinSegmentSize);

    // Request an allocation that doesn't fit in the initial 1 MiB segment
    constexpr std::size_t big = kMinSegmentSize * 3;
    void* p = mem.allocate(big);
    CHECK_NONNULL(p);
    CHECK(mem.segment_count() >= std::size_t(2));

    // Write and read back to verify the mapping is writable
    std::memset(p, 0xAB, big);
    auto* bytes = static_cast<unsigned char*>(p);
    for (std::size_t i = 0; i < big; i += 4096)
        CHECK_EQ(bytes[i], 0xABu);

    mem.deallocate(p);
    std::printf("PASS: test_large_object_growth\n");
}

// ============================================================================
// Test 6: multiple named objects, find_or_construct
// ============================================================================
static void test_multiple_named_objects() {
    segmented_managed_memory mem(kDefaultSegSize);

    // Construct 100 named Points
    for (int i = 0; i < 100; ++i) {
        std::string name = "pt_" + std::to_string(i);
        Point* p = mem.construct<Point>(name.c_str(), i);
        CHECK_NONNULL(p);
        CHECK_EQ(p->id, i);
    }

    // Find all of them
    for (int i = 0; i < 100; ++i) {
        std::string name = "pt_" + std::to_string(i);
        auto [p, n] = mem.find<Point>(name.c_str());
        CHECK_NONNULL(p);
        CHECK_EQ(p->id, i);
    }

    // find_or_construct returns existing
    Point* existing = mem.find_or_construct<Point>("pt_50", 999);
    auto [found, _] = mem.find<Point>("pt_50");
    CHECK_EQ(existing, found);
    CHECK_EQ(existing->id, 50); // still 50, not 999

    // Destroy all
    for (int i = 0; i < 100; ++i) {
        std::string name = "pt_" + std::to_string(i);
        CHECK(mem.destroy<Point>(name.c_str()));
    }

    std::printf("PASS: test_multiple_named_objects\n");
}

// ============================================================================
// Test 7: segmented_allocator with std::vector
// ============================================================================
static void test_allocator_interface() {
    segmented_managed_memory mem(kDefaultSegSize);

    auto alloc = mem.get_allocator<int>();

    // Allocate a raw buffer
    segmented_interprocess::segmented_offset_ptr<int> p = alloc.allocate(100);
    CHECK_NONNULL(p.get());
    for (int i = 0; i < 100; ++i)
        p[i] = i;
    for (int i = 0; i < 100; ++i)
        CHECK_EQ(p[i], i);

    alloc.deallocate(p, 100);
    std::printf("PASS: test_allocator_interface\n");
}

// ============================================================================
// Test 8: raw allocate/deallocate stress (many small objects)
// ============================================================================
static void test_raw_alloc_stress() {
    segmented_managed_memory mem(kDefaultSegSize);

    static constexpr int kN = 10000;
    std::vector<void*> ptrs(kN, nullptr);

    for (int i = 0; i < kN; ++i) {
        ptrs[i] = mem.allocate(64);
        CHECK_NONNULL(ptrs[i]);
        // Write a pattern
        std::memset(ptrs[i], static_cast<unsigned char>(i & 0xFF), 64);
    }

    // Verify patterns and free
    for (int i = 0; i < kN; ++i) {
        auto* b = static_cast<unsigned char*>(ptrs[i]);
        for (int j = 0; j < 64; ++j)
            CHECK_EQ(b[j], static_cast<unsigned char>(i & 0xFF));
        mem.deallocate(ptrs[i]);
    }

    std::printf("PASS: test_raw_alloc_stress (%d allocs)\n", kN);
}


#include <thread>
#include <chrono>

// ============================================================================
// Test 9: background prefetch worker
// ============================================================================
static void test_background_prefetch() {
    segmented_interprocess::detail::shm_remove("test_background_prefetch");
    segmented_managed_memory mem("test_background_prefetch", create_only, kDefaultSegSize);

    std::size_t initial_segs = mem.segment_count();
    CHECK_EQ(initial_segs, std::size_t(1));

    // Allocate slightly more than 50% to trigger the background prefetch
    std::size_t alloc_size = (kDefaultSegSize / 2) + 1024 * 1024;
    void* p = mem.allocate(alloc_size);
    CHECK_NONNULL(p);

    // Wait for the background worker thread to execute (up to 5 seconds)
    std::size_t segs_after = initial_segs;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        segs_after = mem.segment_count();
        if (segs_after > initial_segs) {
            break;
        }
    }
    CHECK(segs_after > initial_segs);

    mem.deallocate(p);

    std::printf("PASS: test_background_prefetch (asynchronously pre-allocated segment)\n");
    segmented_interprocess::detail::shm_remove("test_background_prefetch");
}

// ============================================================================
// main
// ============================================================================
int main() {
    std::printf("=== segmented_managed_memory tests ===\n");

    test_basic_lifecycle();
    test_auto_growth();
    test_capacity_accounting();
    test_explicit_grow();
    test_large_object_growth();
    test_multiple_named_objects();
    test_allocator_interface();
    test_raw_alloc_stress();
    test_background_prefetch();

    std::printf("=== ALL segmented_managed_memory tests PASSED ===\n");
    return 0;
}
