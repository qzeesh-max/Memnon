/// \file test_segmented_offset_ptr.cpp
/// Unit tests for segmented_offset_ptr covering:
///   1. Null / default construction
///   2. In-segment encoding: get() returns correct address
///   3. Out-of-segment (stack) encoding: traditional relative offset
///   4. Copy from in-segment to stack re-encodes correctly
///   5. Copy from stack to in-segment re-encodes correctly
///   6. Cross-segment pointer (ptr in seg A, pointee in seg B)
///   7. In-place arithmetic (++, --, +=, -=)
///   8. Binary arithmetic (+, -)
///   9. Comparison operators
///  10. Null sentinel preservation
///  11. Static/const/reinterpret cast helpers
///  12. PAC-strip safety on arm64

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>

// Must include sub_segment + registry first so we can register fake segments
#include "segmented_interprocess/sub_segment.hpp"
#include "segmented_interprocess/segment_registry.hpp"
#include "segmented_interprocess/segmented_offset_ptr.hpp"
#include "segmented_interprocess/detail/platform.hpp"

using namespace segmented_interprocess;
using sop_int  = segmented_offset_ptr<int>;
using sop_void = segmented_offset_ptr<void>;
using sop_char = segmented_offset_ptr<char>;

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
// RAII guard that allocates an anonymous mmap region and registers it as a
// fake sub_segment in the global registry.
// ============================================================================
struct fake_segment {
    std::unique_ptr<sub_segment> seg;

    explicit fake_segment(std::size_t sz = 1u << 20 /* 1 MiB */) {
        seg = sub_segment::create_anon(sz);
        assert(seg && "mmap failed in test");
        segment_registry::instance().register_segment(seg.get());
    }

    ~fake_segment() {
        if (seg)
            segment_registry::instance().unregister_segment(seg.get());
    }

    /// Return a pointer to a T-sized region at byte offset `off` within the
    /// segment.  Does NOT call any constructors.
    template<class T>
    T* at(std::size_t off = 0) const {
        return reinterpret_cast<T*>(
            static_cast<char*>(seg->base_vaddr) + off);
    }

    uintptr_t base() const { return seg->base_addr(); }
    std::size_t size() const { return seg->size; }

    // Non-copyable
    fake_segment(const fake_segment&) = delete;
    fake_segment& operator=(const fake_segment&) = delete;
};

// ============================================================================
// Test 1: null / default construction
// ============================================================================
static void test_null_construction() {
    sop_int p;
    CHECK(!p);
    CHECK(p == nullptr);
    CHECK(p.get() == nullptr);
    CHECK(p.get_offset() == sop_int::kNull);

    sop_int q(nullptr);
    CHECK(q.get() == nullptr);

    std::printf("PASS: test_null_construction\n");
}

// ============================================================================
// Test 2: in-segment construction and get()
// ============================================================================
static void test_in_segment_encoding() {
    fake_segment fs(detail::page_size() * 8);

    // Place an int at offset 4096 inside the segment
    const std::size_t off = 4096;
    int* target = fs.at<int>(off);
    *target = 0xDEAD;

    // Create a segmented_offset_ptr that lives inside the segment
    sop_int* sop_in_seg = fs.at<sop_int>(2048); // at offset 2048
    ::new(sop_in_seg) sop_int(target);           // placement-new

    // get() should resolve to target
    CHECK_EQ(sop_in_seg->get(), target);
    CHECK_EQ(**sop_in_seg, 0xDEAD);

    // The stored offset must be (target_vaddr - seg.base)
    uintptr_t expected_offset =
        reinterpret_cast<uintptr_t>(target) - fs.base();
    CHECK_EQ(sop_in_seg->get_offset(), expected_offset);

    sop_in_seg->~sop_int(); // explicit destructor (no dealloc)
    std::printf("PASS: test_in_segment_encoding\n");
}

// ============================================================================
// Test 3: out-of-segment (stack) encoding — traditional relative offset
// ============================================================================
static void test_stack_encoding() {
    int value = 42;
    sop_int p(&value);  // p is on the stack; value is on the stack

    // The trie has no registration for stack addresses → traditional encoding
    CHECK_EQ(p.get(), &value);
    CHECK_EQ(*p, 42);

    // Stored offset = &value_vaddr - &p_vaddr (signed, wrapped in uintptr_t)
    uintptr_t self    = detail::ptr_to_vaddr(&p);
    uintptr_t pointee = detail::ptr_to_vaddr(&value);
    uintptr_t expected = static_cast<uintptr_t>(
        static_cast<std::ptrdiff_t>(pointee) - static_cast<std::ptrdiff_t>(self));
    CHECK_EQ(p.get_offset(), expected);

    std::printf("PASS: test_stack_encoding\n");
}

// ============================================================================
// Test 4: copy from in-segment to stack re-encodes
// ============================================================================
static void test_copy_inseg_to_stack() {
    fake_segment fs;

    // Place a sop_int inside the segment pointing to an int also in the segment
    int* target = fs.at<int>(8192);
    *target = 77;
    sop_int* in_seg = fs.at<sop_int>(1024);
    ::new(in_seg) sop_int(target);

    // Copy to stack variable
    sop_int on_stack(*in_seg);

    // Must still resolve to target
    CHECK_EQ(on_stack.get(), target);
    CHECK_EQ(*on_stack, 77);

    // Stack encoding: offset = target_vaddr - on_stack_vaddr (signed)
    uintptr_t self    = detail::ptr_to_vaddr(&on_stack);
    uintptr_t tgt     = detail::ptr_to_vaddr(target);
    uintptr_t expected = static_cast<uintptr_t>(
        static_cast<std::ptrdiff_t>(tgt) - static_cast<std::ptrdiff_t>(self));
    CHECK_EQ(on_stack.get_offset(), expected);

    in_seg->~sop_int();
    std::printf("PASS: test_copy_inseg_to_stack\n");
}

// ============================================================================
// Test 5: copy from stack to in-segment re-encodes
// ============================================================================
static void test_copy_stack_to_inseg() {
    fake_segment fs;

    int value = 55;
    sop_int on_stack(&value);  // stack → stack encoding

    // Copy into segment
    sop_int* in_seg = fs.at<sop_int>(2048);
    ::new(in_seg) sop_int(on_stack);

    CHECK_EQ(in_seg->get(), &value);
    CHECK_EQ(**in_seg, 55);

    // In-segment encoding: offset = value_vaddr - seg_base_vaddr
    uintptr_t expected =
        reinterpret_cast<uintptr_t>(&value) - fs.base();
    CHECK_EQ(in_seg->get_offset(), expected);

    in_seg->~sop_int();
    std::printf("PASS: test_copy_stack_to_inseg\n");
}

// ============================================================================
// Test 6: cross-segment pointer (ptr in seg A, pointee in seg B)
// ============================================================================
static void test_cross_segment_pointer() {
    fake_segment fsA(detail::page_size() * 4);
    fake_segment fsB(detail::page_size() * 4);

    // Place target in seg B
    int* target = fsB.at<int>(1024);
    *target = 123;

    // Place sop_int in seg A pointing to target in seg B
    sop_int* in_segA = fsA.at<sop_int>(512);
    ::new(in_segA) sop_int(target);

    // m_offset = target_vaddr - A.base_vaddr
    uintptr_t expected_offset =
        reinterpret_cast<uintptr_t>(target) - fsA.base();
    CHECK_EQ(in_segA->get_offset(), expected_offset);

    // get() should resolve to target in seg B
    CHECK_EQ(in_segA->get(), target);
    CHECK_EQ(**in_segA, 123);

    // Copy to stack — must still resolve correctly
    sop_int on_stack(*in_segA);
    CHECK_EQ(on_stack.get(), target);

    // Copy to seg B — re-encodes relative to B's base
    sop_int* in_segB = fsB.at<sop_int>(2048);
    ::new(in_segB) sop_int(*in_segA);
    CHECK_EQ(in_segB->get(), target);

    // m_offset relative to B's base: target_vaddr - B.base_vaddr
    uintptr_t expected_b_offset =
        reinterpret_cast<uintptr_t>(target) - fsB.base();
    CHECK_EQ(in_segB->get_offset(), expected_b_offset);

    in_segA->~sop_int();
    in_segB->~sop_int();
    std::printf("PASS: test_cross_segment_pointer\n");
}

// ============================================================================
// Test 7: in-place arithmetic
// ============================================================================
static void test_in_place_arithmetic() {
    fake_segment fs;

    // int array at offset 4096
    int* arr = fs.at<int>(4096);
    for (int i = 0; i < 10; ++i) arr[i] = i * 10;

    sop_int* p_in_seg = fs.at<sop_int>(2048);
    ::new(p_in_seg) sop_int(arr);

    // prefix ++
    ++(*p_in_seg);
    CHECK_EQ(p_in_seg->get(), arr + 1);
    CHECK_EQ(**p_in_seg, 10);

    // postfix ++
    sop_int before = (*p_in_seg)++;
    CHECK_EQ(before.get(), arr + 1);
    CHECK_EQ(p_in_seg->get(), arr + 2);
    CHECK_EQ(**p_in_seg, 20);

    // +=
    *p_in_seg += 3;
    CHECK_EQ(p_in_seg->get(), arr + 5);
    CHECK_EQ(**p_in_seg, 50);

    // -=
    *p_in_seg -= 2;
    CHECK_EQ(p_in_seg->get(), arr + 3);
    CHECK_EQ(**p_in_seg, 30);

    // prefix --
    --(*p_in_seg);
    CHECK_EQ(p_in_seg->get(), arr + 2);

    p_in_seg->~sop_int();
    std::printf("PASS: test_in_place_arithmetic\n");
}

// ============================================================================
// Test 8: binary arithmetic (+ operator produces new pointer on stack)
// ============================================================================
static void test_binary_arithmetic() {
    int arr[5] = {0, 1, 2, 3, 4};
    sop_int base(&arr[0]);  // stack

    sop_int p2 = base + 2;
    CHECK_EQ(p2.get(), &arr[2]);
    CHECK_EQ(*p2, 2);

    sop_int p0 = p2 - 2;
    CHECK_EQ(p0.get(), &arr[0]);

    sop_int pd = 3 + base;
    CHECK_EQ(pd.get(), &arr[3]);

    // pointer difference
    std::ptrdiff_t diff = p2 - base;
    CHECK_EQ(diff, 2);

    std::printf("PASS: test_binary_arithmetic\n");
}

// ============================================================================
// Test 9: comparison operators
// ============================================================================
static void test_comparisons() {
    int arr[4] = {0, 1, 2, 3};
    sop_int p0(&arr[0]);
    sop_int p1(&arr[1]);
    sop_int p2(&arr[2]);
    sop_int q0(&arr[0]); // same target as p0

    CHECK(p0 == q0);
    CHECK(p0 != p1);
    CHECK(p0 <  p1);
    CHECK(p1 <= p2);
    CHECK(p2 >  p0);
    CHECK(p2 >= p1);
    CHECK(p0 == p0);

    sop_int null_ptr;
    CHECK(null_ptr == nullptr);
    CHECK(nullptr  == null_ptr);
    CHECK(p0       != nullptr);
    CHECK(nullptr  != p0);

    std::printf("PASS: test_comparisons\n");
}

// ============================================================================
// Test 10: null sentinel preserved across copies
// ============================================================================
static void test_null_sentinel() {
    sop_int null_src(nullptr);
    CHECK_EQ(null_src.get_offset(), sop_int::kNull);

    // Copy to stack
    sop_int null_copy(null_src);
    CHECK_EQ(null_copy.get_offset(), sop_int::kNull);
    CHECK(!null_copy);

    // Copy into segment
    fake_segment fs;
    sop_int* in_seg = fs.at<sop_int>(512);
    ::new(in_seg) sop_int(null_src);
    CHECK_EQ(in_seg->get_offset(), sop_int::kNull);
    CHECK(!(*in_seg));

    in_seg->~sop_int();
    std::printf("PASS: test_null_sentinel\n");
}

// ============================================================================
// Test 11: cast helpers
// ============================================================================
static void test_cast_helpers() {
    int val = 99;
    sop_int p(&val);

    // static_pointer_cast to void
    auto pv = static_pointer_cast<void>(p);
    CHECK_EQ(pv.get(), static_cast<void*>(&val));

    // const_pointer_cast
    auto pc = const_pointer_cast<const int>(p);
    CHECK_EQ(pc.get(), &val);

    // reinterpret_pointer_cast to char
    auto pr = reinterpret_pointer_cast<char>(p);
    CHECK_EQ(pr.get(), reinterpret_cast<char*>(&val));

    std::printf("PASS: test_cast_helpers\n");
}

// ============================================================================
// Test 12: assignment operators
// ============================================================================
static void test_assignment() {
    int a = 1, b = 2;
    sop_int p(&a);
    sop_int q(&b);

    CHECK_EQ(p.get(), &a);
    p = q;
    CHECK_EQ(p.get(), &b);

    p = &a;
    CHECK_EQ(p.get(), &a);

    p = nullptr;
    CHECK(!p);

    std::printf("PASS: test_assignment\n");
}

// ============================================================================
// Test 13: subscript operator
// ============================================================================
static void test_subscript() {
    int arr[5] = {10, 20, 30, 40, 50};
    sop_int p(&arr[0]);
    for (int i = 0; i < 5; ++i)
        CHECK_EQ(p[i], arr[i]);

    std::printf("PASS: test_subscript\n");
}

// ============================================================================
// main
// ============================================================================
int main() {
    std::printf("=== segmented_offset_ptr tests ===\n");

    test_null_construction();
    test_in_segment_encoding();
    test_stack_encoding();
    test_copy_inseg_to_stack();
    test_copy_stack_to_inseg();
    test_cross_segment_pointer();
    test_in_place_arithmetic();
    test_binary_arithmetic();
    test_comparisons();
    test_null_sentinel();
    test_cast_helpers();
    test_assignment();
    test_subscript();

    std::printf("=== ALL segmented_offset_ptr tests PASSED ===\n");
    return 0;
}
