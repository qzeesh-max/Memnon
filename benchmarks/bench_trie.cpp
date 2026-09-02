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
#include <random>

#include "segmented_interprocess/ncrit_trie.hpp"

using namespace segmented_interprocess;

// A dummy segment pointer type
struct dummy_segment {
    uintptr_t base;
    std::size_t size;
};

// ---------------------------------------------------------------------------
// 1. Single-Threaded Trie Lookup (Hit vs Miss)
// ---------------------------------------------------------------------------
static void BM_Trie_SingleThread_Hit(benchmark::State& state) {
    ncrit_trie<dummy_segment*> trie;
    dummy_segment seg{0x100000000, 1024 * 1024}; // 1 MB
    trie.insert_range(seg.base, seg.size, &seg);

    // Warm cache
    trie.lookup(seg.base + 4096);

    for (auto _ : state) {
        benchmark::DoNotOptimize(trie.lookup(seg.base + 8192));
    }
}
BENCHMARK(BM_Trie_SingleThread_Hit);

static void BM_Trie_SingleThread_Miss(benchmark::State& state) {
    ncrit_trie<dummy_segment*> trie;
    dummy_segment seg{0x100000000, 1024 * 1024};
    trie.insert_range(seg.base, seg.size, &seg);

    for (auto _ : state) {
        benchmark::DoNotOptimize(trie.lookup(0x200000000));
    }
}
BENCHMARK(BM_Trie_SingleThread_Miss);

// ---------------------------------------------------------------------------
// 2. Multi-Threaded Trie Lookup (Contention/Cache effectiveness)
// ---------------------------------------------------------------------------
static void BM_Trie_MultiThread_Hit(benchmark::State& state) {
    static ncrit_trie<dummy_segment*> trie;
    static dummy_segment seg{0x100000000, 1024 * 1024};
    
    if (state.thread_index() == 0) {
        trie.insert_range(seg.base, seg.size, &seg);
    }
    
    // Warm TLS cache for this thread
    trie.lookup(seg.base + 4096);

    for (auto _ : state) {
        benchmark::DoNotOptimize(trie.lookup(seg.base + 8192));
    }
}
BENCHMARK(BM_Trie_MultiThread_Hit)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ---------------------------------------------------------------------------
// 3. LRU Cache Thrashing (Accessing 16+ segments round-robin)
// ---------------------------------------------------------------------------
static void BM_Trie_LRU_Thrashing(benchmark::State& state) {
    ncrit_trie<dummy_segment*> trie;
    std::vector<dummy_segment> segments(32); // Thrash the 16-entry cache
    
    for (size_t i = 0; i < segments.size(); ++i) {
        segments[i].base = 0x100000000 + (i * 1024 * 1024);
        segments[i].size = 1024 * 1024;
        trie.insert_range(segments[i].base, segments[i].size, &segments[i]);
    }

    size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(trie.lookup(segments[i].base));
        i = (i + 1) % segments.size();
    }
}
BENCHMARK(BM_Trie_LRU_Thrashing);
