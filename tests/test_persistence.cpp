#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <string>
#include <iostream>

#include <segmented_interprocess/segmented_managed_memory.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

using namespace segmented_interprocess;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}
#define CHECK(expr)      check((expr), #expr)
#define CHECK_EQ(a, b)   check((a)==(b), #a " == " #b)
#define CHECK_NE(a, b)   check((a)!=(b), #a " != " #b)
#define CHECK_NULL(a)    check((a)==nullptr, #a " == nullptr")
#define CHECK_NONNULL(a) check((a)!=nullptr, #a " != nullptr")

struct MyData {
    int id;
    double value;
};

struct MyDataArray {
    MyData arr[100];
};

struct NewDataArray {
    MyData arr[5000];
};

const char* SHM_NAME = "test_persistence_shm";

static void child1() {
    // Child 1 (Creator)
    segmented_interprocess::detail::shm_remove(SHM_NAME);
    segmented_managed_memory mem(SHM_NAME, create_only, 1024 * 64); // 64 KB

    auto* wrapper = mem.construct<MyDataArray>("my_arr");
    for (int i = 0; i < 100; ++i) {
        wrapper->arr[i].id = i;
        wrapper->arr[i].value = i * 3.14;
    }

    // Allocate raw objects to cause a segment growth
    std::vector<void*> raw_allocs;
    for (int i = 0; i < 50; ++i) {
        raw_allocs.push_back(mem.allocate(4096)); // ~200 KB total, forces multiple growths
    }

    mem.construct<int>("magic_val", 42);
    std::exit(0);
}

static void child2() {
    // Child 2 (Opener)
    segmented_managed_memory mem(SHM_NAME, open_only);

    // Verify "my_arr"
    auto [wrapper, count] = mem.find<MyDataArray>("my_arr");
    if (!wrapper) std::exit(1);
    if (count != 1) std::exit(2);

    for (int i = 0; i < 100; ++i) {
        if (wrapper->arr[i].id != i) std::exit(3);
        if (wrapper->arr[i].value != i * 3.14) std::exit(4);
    }

    // Verify "magic_val"
    auto [val, vcount] = mem.find<int>("magic_val");
    if (!val || vcount != 1 || *val != 42) std::exit(5);

    // Modify
    *val = 99;

    // Deallocate "my_arr"
    mem.destroy<MyDataArray>("my_arr");

    // Allocate a new object that triggers another growth
    auto* new_wrapper = mem.construct<NewDataArray>("new_arr");
    for (int i = 0; i < 5000; ++i) {
        new_wrapper->arr[i].id = i + 1000;
    }

    std::exit(0);
}

static void test_cross_process_lifecycle(const char* argv0) {
    // Quote argv0 in case it contains spaces
    std::string cmd1 = std::string("\"\"") + argv0 + "\" --child1\"";
#ifndef _WIN32
    cmd1 = std::string("\"") + argv0 + "\" --child1";
#endif

    int status1 = std::system(cmd1.c_str());
    CHECK_EQ(status1, 0);

    std::string cmd2 = std::string("\"\"") + argv0 + "\" --child2\"";
#ifndef _WIN32
    cmd2 = std::string("\"") + argv0 + "\" --child2";
#endif

    int status2 = std::system(cmd2.c_str());
    CHECK_EQ(status2, 0);

    // Phase 3: Same process (parent) re-opens to verify child 2's work
    {
        segmented_managed_memory mem(SHM_NAME, open_only);
        
        // Verify "magic_val" was modified
        auto [val, vcount] = mem.find<int>("magic_val");
        CHECK_NONNULL(val);
        CHECK_EQ(*val, 99);

        // Verify "my_arr" is gone
        auto [wrapper, count] = mem.find<MyDataArray>("my_arr");
        CHECK_NULL(wrapper);
        CHECK_EQ(count, 0);

        // Verify "new_arr" exists and is correct
        auto [new_wrapper, ncount] = mem.find<NewDataArray>("new_arr");
        CHECK_NONNULL(new_wrapper);
        CHECK_EQ(ncount, 1);
        CHECK_EQ(new_wrapper->arr[0].id, 1000);
        CHECK_EQ(new_wrapper->arr[4999].id, 5999);

        // Deallocate ALL named objects
        mem.destroy<int>("magic_val");
        mem.destroy<NewDataArray>("new_arr");
    }

    // Phase 4: Final verification (parent)
    {
        segmented_managed_memory mem(SHM_NAME, open_only);
        
        auto [val, vcount] = mem.find<int>("magic_val");
        CHECK_NULL(val);

        auto [new_wrapper, ncount] = mem.find<NewDataArray>("new_arr");
        CHECK_NULL(new_wrapper);
    }

    segmented_interprocess::detail::shm_remove(SHM_NAME);
    std::printf("PASS: test_cross_process_lifecycle\n");
}

int main(int argc, char* argv[]) {
    if (argc == 2) {
        std::string arg = argv[1];
        if (arg == "--child1") {
            child1();
            return 0;
        } else if (arg == "--child2") {
            child2();
            return 0;
        }
    }

    std::printf("=== test_persistence ===\n");
    test_cross_process_lifecycle(argv[0]);
    std::printf("=== ALL test_persistence PASSED ===\n");
    return 0;
}
