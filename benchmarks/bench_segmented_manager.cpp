#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include "segmented_interprocess/segmented_managed_memory.hpp"
#include "segmented_interprocess/segmented_offset_ptr.hpp"

using namespace segmented_interprocess;

struct Node {
    int value;
    segmented_offset_ptr<Node> next;
    Node(int v) : value(v), next(nullptr) {}
};

struct BoostNode {
    int value;
    boost::interprocess::offset_ptr<BoostNode> next;
    BoostNode(int v) : value(v), next(nullptr) {}
};

// ---------------------------------------------------------------------------
// 1. Single Threaded Allocations
// ---------------------------------------------------------------------------
static void BM_SegMgr_SingleThread_Alloc_Anon(benchmark::State& state) {
    segmented_segment_manager mgr;
    for (auto _ : state) {
        void* mem = mgr.allocate(64);
        benchmark::DoNotOptimize(mem);
    }
}
BENCHMARK(BM_SegMgr_SingleThread_Alloc_Anon);

static void BM_SegMgr_SingleThread_Alloc_FileBacked(benchmark::State& state) {
    // We recreate the manager inside the loop because of SHM files
    for (auto _ : state) {
        state.PauseTiming();
        boost::interprocess::shared_memory_object::remove("bench_shm_alloc");
        segmented_managed_memory mgr("bench_shm_alloc", create_only, 1024 * 1024);
        state.ResumeTiming();

        for (int i = 0; i < 1000; ++i) {
            void* mem = mgr.allocate(64);
            benchmark::DoNotOptimize(mem);
        }
    }
    boost::interprocess::shared_memory_object::remove("bench_shm_alloc");
}
BENCHMARK(BM_SegMgr_SingleThread_Alloc_FileBacked);

static void BM_Boost_SingleThread_Alloc_FileBacked(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        boost::interprocess::shared_memory_object::remove("bench_boost_alloc");
        boost::interprocess::managed_shared_memory mgr(boost::interprocess::create_only, "bench_boost_alloc", 1024 * 1024);
        state.ResumeTiming();

        for (int i = 0; i < 1000; ++i) {
            void* mem = mgr.allocate(64);
            benchmark::DoNotOptimize(mem);
        }
    }
    boost::interprocess::shared_memory_object::remove("bench_boost_alloc");
}
BENCHMARK(BM_Boost_SingleThread_Alloc_FileBacked);

// ---------------------------------------------------------------------------
// 2. Multi-Threaded Allocations (Contention)
// ---------------------------------------------------------------------------
static void BM_SegMgr_MultiThread_Alloc_Anon(benchmark::State& state) {
    static segmented_segment_manager mgr;
    for (auto _ : state) {
        void* mem = mgr.allocate(64);
        benchmark::DoNotOptimize(mem);
    }
}
BENCHMARK(BM_SegMgr_MultiThread_Alloc_Anon)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ---------------------------------------------------------------------------
// 3. Multi-Process Traversal (fork + exec equivalent via fork)
// ---------------------------------------------------------------------------
static void BM_SegMgr_MultiProcess_Traversal(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        boost::interprocess::shared_memory_object::remove("bench_shm_multiproc");
        segmented_managed_memory mgr("bench_shm_multiproc", create_only, 1024 * 1024);
        
        segmented_offset_ptr<Node>* root = mgr.construct<segmented_offset_ptr<Node>>("Root", nullptr);
        Node* curr = mgr.construct<Node>("Node0", 0);
        *root = curr;

        for (int i = 1; i < 5000; ++i) {
            void* mem = mgr.allocate(sizeof(Node));
            Node* next = new(mem) Node(i);
            curr->next = next;
            curr = next;
        }
        state.ResumeTiming();

        // Fork to simulate a second process opening the memory and traversing
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            segmented_managed_memory child_mgr("bench_shm_multiproc", open_only);
            auto res = child_mgr.find<segmented_offset_ptr<Node>>("Root");
            if (res.first) {
                Node* c = res.first->get();
                while (c != nullptr) {
                    benchmark::DoNotOptimize(c->value);
                    c = c->next.get();
                }
            }
            _exit(0);
        } else {
            // Parent waits for child
            int status;
            waitpid(pid, &status, 0);
        }
    }
    boost::interprocess::shared_memory_object::remove("bench_shm_multiproc");
}
BENCHMARK(BM_SegMgr_MultiProcess_Traversal);

static void BM_Boost_MultiProcess_Traversal(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        boost::interprocess::shared_memory_object::remove("bench_boost_multiproc");
        boost::interprocess::managed_shared_memory mgr(boost::interprocess::create_only, "bench_boost_multiproc", 1024 * 1024);
        
        boost::interprocess::offset_ptr<BoostNode>* root = mgr.construct<boost::interprocess::offset_ptr<BoostNode>>("Root")(nullptr);
        BoostNode* curr = mgr.construct<BoostNode>("Node0")(0);
        *root = curr;

        for (int i = 1; i < 5000; ++i) {
            void* mem = mgr.allocate(sizeof(BoostNode));
            BoostNode* next = new(mem) BoostNode(i);
            curr->next = next;
            curr = next;
        }
        state.ResumeTiming();

        // Fork to simulate a second process opening the memory and traversing
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            boost::interprocess::managed_shared_memory child_mgr(boost::interprocess::open_only, "bench_boost_multiproc");
            auto res = child_mgr.find<boost::interprocess::offset_ptr<BoostNode>>("Root");
            if (res.first) {
                BoostNode* c = res.first->get();
                while (c != nullptr) {
                    benchmark::DoNotOptimize(c->value);
                    c = c->next.get();
                }
            }
            _exit(0);
        } else {
            // Parent waits for child
            int status;
            waitpid(pid, &status, 0);
        }
    }
    boost::interprocess::shared_memory_object::remove("bench_boost_multiproc");
}
BENCHMARK(BM_Boost_MultiProcess_Traversal);
