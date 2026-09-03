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

#include <benchmark/benchmark.h>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

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
// 3. Allocation and Deallocation (Mixed Workload)
// ---------------------------------------------------------------------------
static void BM_SegMgr_AllocDealloc(benchmark::State& state) {
    segmented_segment_manager mgr;
    std::vector<void*> ptrs;
    ptrs.reserve(1000);
    for (auto _ : state) {
        for (int i = 0; i < 1000; ++i) {
            ptrs.push_back(mgr.allocate(64));
        }
        for (void* p : ptrs) {
            mgr.deallocate(p);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_SegMgr_AllocDealloc);

static void BM_Boost_AllocDealloc(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        boost::interprocess::shared_memory_object::remove("bench_boost_alloc_dealloc");
        boost::interprocess::managed_shared_memory mgr(boost::interprocess::create_only, "bench_boost_alloc_dealloc", 1024 * 1024);
        state.ResumeTiming();

        std::vector<void*> ptrs;
        ptrs.reserve(1000);
        for (int i = 0; i < 1000; ++i) {
            ptrs.push_back(mgr.allocate(64));
        }
        for (void* p : ptrs) {
            mgr.deallocate(p);
        }
    }
    boost::interprocess::shared_memory_object::remove("bench_boost_alloc_dealloc");
}
BENCHMARK(BM_Boost_AllocDealloc);

// ---------------------------------------------------------------------------
// 4. Multi-Threaded Traversal
// ---------------------------------------------------------------------------
static segmented_managed_memory* g_seg_mgr_traversal = nullptr;
static segmented_offset_ptr<Node>* g_seg_mgr_root = nullptr;

static void SetupSegMgrTraversal() {
    boost::interprocess::shared_memory_object::remove("bench_seg_traversal");
    g_seg_mgr_traversal = new segmented_managed_memory("bench_seg_traversal", create_only, 1024 * 1024);
    g_seg_mgr_root = g_seg_mgr_traversal->construct<segmented_offset_ptr<Node>>("Root", nullptr);
    Node* curr = g_seg_mgr_traversal->construct<Node>("Node0", 0);
    *g_seg_mgr_root = curr;

    for (int i = 1; i < 5000; ++i) {
        void* mem = g_seg_mgr_traversal->allocate(sizeof(Node));
        Node* next = new(mem) Node(i);
        curr->next = next;
        curr = next;
    }
}

static void TeardownSegMgrTraversal() {
    delete g_seg_mgr_traversal;
    boost::interprocess::shared_memory_object::remove("bench_seg_traversal");
}

static void BM_SegMgr_MultiThread_Traversal(benchmark::State& state) {
    if (state.thread_index() == 0) SetupSegMgrTraversal();
    
    for (auto _ : state) {
        Node* c = g_seg_mgr_root->get();
        while (c != nullptr) {
            benchmark::DoNotOptimize(c->value);
            c = c->next.get();
        }
    }
    
    if (state.thread_index() == 0) TeardownSegMgrTraversal();
}
BENCHMARK(BM_SegMgr_MultiThread_Traversal)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static boost::interprocess::managed_shared_memory* g_boost_mgr_traversal = nullptr;
static boost::interprocess::offset_ptr<BoostNode>* g_boost_mgr_root = nullptr;

static void SetupBoostTraversal() {
    boost::interprocess::shared_memory_object::remove("bench_boost_traversal");
    g_boost_mgr_traversal = new boost::interprocess::managed_shared_memory(boost::interprocess::create_only, "bench_boost_traversal", 1024 * 1024);
    g_boost_mgr_root = g_boost_mgr_traversal->construct<boost::interprocess::offset_ptr<BoostNode>>("Root")(nullptr);
    BoostNode* curr = g_boost_mgr_traversal->construct<BoostNode>("Node0")(0);
    *g_boost_mgr_root = curr;

    for (int i = 1; i < 5000; ++i) {
        void* mem = g_boost_mgr_traversal->allocate(sizeof(BoostNode));
        BoostNode* next = new(mem) BoostNode(i);
        curr->next = next;
        curr = next;
    }
}

static void TeardownBoostTraversal() {
    delete g_boost_mgr_traversal;
    boost::interprocess::shared_memory_object::remove("bench_boost_traversal");
}

static void BM_Boost_MultiThread_Traversal(benchmark::State& state) {
    if (state.thread_index() == 0) SetupBoostTraversal();
    
    for (auto _ : state) {
        BoostNode* c = g_boost_mgr_root->get();
        while (c != nullptr) {
            benchmark::DoNotOptimize(c->value);
            c = c->next.get();
        }
    }
    
    if (state.thread_index() == 0) TeardownBoostTraversal();
}
BENCHMARK(BM_Boost_MultiThread_Traversal)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ---------------------------------------------------------------------------
// 5. Multi-Process Traversal (fork + exec equivalent via fork)
// ---------------------------------------------------------------------------
#ifndef _WIN32
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
#endif

