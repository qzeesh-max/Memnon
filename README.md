# Boost Interprocess Growing Memory Segment

This project provides a robust, scalable, lock-free approach to managing segmented, growable shared memory in C++, designed as a modern enhancement over Boost.Interprocess's standard managed shared memory.

## Key Features

1. **Segmented Memory Architecture (`segmented_segment_manager`)**
   - Allows virtually unbounded, disjoint shared memory mapping by dynamically creating and linking new underlying memory segments when capacity is reached.
   - Fully compatible with `boost::interprocess::offset_ptr` equivalents via a custom smart pointer (`segmented_offset_ptr`).

2. **Wait-Free Page-Granular Trie (`ncrit_trie`)**
   - Implements a fixed-depth radix trie to map arbitrary memory addresses back to their managing `sub_segment` in $O(1)$ lock-free read time.
   - Includes a thread-local, 16-entry LRU Cache (`tls_cache_t`).

3. **Lockless Passive Eviction in TLS Cache**
   - The thread-local LRU cache avoids costly atomic syncs during read hits by storing a pointer to the leaf node slot (`std::atomic<uintptr_t>*`) of the trie.
   - When a segment is removed, the trie locklessly zeroes the slot. The cache validates the slot during lookup, providing passive, zero-overhead invalidation without ABA problems.

4. **Transparent Multi-Process Shared Memory Management (`segmented_managed_memory`)**
   - Provides an identical API to `boost::interprocess::managed_shared_memory`.
   - Safely allocates and constructs objects across boundaries.
   - Safely catches exceptions when `segmented_offset_ptr` is misused to point across distinct segmented managers.

5. **Multi-Thread & Multi-Process Concurrency**
   - Lock-free lookups allow unbounded concurrent readers across different processes.
   - `std::recursive_mutex` guards ensure deadlocks cannot occur during nested allocations and recursive segment growth.

6. **Lazy Interprocess Discovery**
   - Processes opening the shared memory dynamically detect when segments are grown and appropriately update their local mapping tables using native Boost robust named object tracking.

## Building and Testing

### Prerequisites
- CMake 3.10+
- Boost C++ Libraries
- A C++17 compatible compiler

### Build Instructions
```sh
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
```

### Running Tests
The project features a comprehensive test suite covering basic memory allocations, recursive growths, multi-threading with contention, cross-manager bounds checking, and multi-process lazy discovery.

```sh
cd build
make test
```

### Running Benchmarks
Google Benchmark is automatically fetched via `FetchContent` in CMake.

```sh
cd build
make benchmarks
./benchmarks/benchmarks
```

Benchmarks evaluate:
- Single-threaded vs Multi-threaded Trie Lookups
- LRU Cache Thrashing Scenarios
- Single-threaded and Contended Memory Allocations
- Multi-process memory traversal using `fork()`

### Sanitizers
A script is provided to automatically compile and run the full test suite under AddressSanitizer (ASAN), UndefinedBehaviorSanitizer (UBSAN), and ThreadSanitizer (TSAN) to guarantee memory correctness and data-race freedom.

```sh
./tests/test_sanitizers.sh
```

## Architecture Layout

- `include/segmented_interprocess/`: Core headers (`ncrit_trie.hpp`, `segmented_segment_manager.hpp`, `segmented_managed_memory.hpp`, `segmented_offset_ptr.hpp`).
- `tests/`: Extensive unit tests.
- `benchmarks/`: Google Benchmark test suite.

## License

This project is open-source. Please see the LICENSE file for more information.
