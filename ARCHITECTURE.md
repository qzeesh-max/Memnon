# N-CRIT Trie & Segment Management Architecture

This document visualizes the lock-free N-CRIT Radix Trie and its interaction with multiple threads and the segmented memory manager. It is designed to quickly map virtual addresses to sub-segments without relying on OS-level locks.

## Lock-Free Trie and Thread-Local Caches

The N-CRIT (N-level Concurrent Radix Indexing Trie) maps a virtual address to a specific sub-segment manager. Because resolving this mapping via tree traversal is expensive on the hot path (allocation), each thread maintains a Thread-Local LRU Cache (TLS Cache) that remembers the bounds of recently accessed sub-segments.

```mermaid
flowchart TB
    %% Definitions
    classDef thread fill:#2c3e50,stroke:#34495e,stroke-width:2px,color:#fff
    classDef tls fill:#8e44ad,stroke:#9b59b6,stroke-width:2px,color:#fff
    classDef trie fill:#2980b9,stroke:#3498db,stroke-width:2px,color:#fff
    classDef seg fill:#27ae60,stroke:#2ecc71,stroke-width:2px,color:#fff
    
    subgraph Threads ["Concurrent Application Threads"]
        direction LR
        T1((Thread 1)):::thread
        T2((Thread 2)):::thread
        T3((Thread 3)):::thread
    end

    subgraph TLS_Cache ["Thread-Local LRU Caches (TLS)"]
        direction LR
        TLS1["TLS Cache (T1)<br><br>0: [0x1000, 0x2000) -> Seg 0<br>1: [0x5000, 0x6000) -> Seg 2<br>..."]:::tls
        TLS2["TLS Cache (T2)<br><br>0: [0x3000, 0x4000) -> Seg 1<br>1: [0x1000, 0x2000) -> Seg 0<br>..."]:::tls
        TLS3["TLS Cache (T3)<br><br>0: Empty<br>1: Empty<br>..."]:::tls
    end

    subgraph Radix_Trie ["Global Lock-Free N-CRIT Trie"]
        direction TB
        Root["Root Node<br>(Level 0)"]:::trie
        L1_A["Internal Node<br>(Level 1)"]:::trie
        L1_B["Internal Node<br>(Level 1)"]:::trie
        Leaf1["Leaf Node<br>(Level 3)"]:::trie
        Leaf2["Leaf Node<br>(Level 3)"]:::trie
        
        Root --"Index A"--> L1_A
        Root --"Index B"--> L1_B
        
        L1_A --"..."--> Leaf1
        L1_B --"..."--> Leaf2
    end

    subgraph Segments ["Segmented Managed Memory"]
        direction LR
        Seg0["Sub-Segment 0<br>(Boost segment_manager)"]:::seg
        Seg1["Sub-Segment 1<br>(Boost segment_manager)"]:::seg
        Seg2["Sub-Segment 2<br>(Boost segment_manager)"]:::seg
    end

    %% Connections
    T1 -.->|1. Lookup VA| TLS1
    T2 -.->|1. Lookup VA| TLS2
    T3 -.->|1. Lookup VA| TLS3

    TLS1 -.->|2. Cache Hit!| Seg0
    TLS2 -.->|2. Cache Hit!| Seg1
    TLS3 -.->|2. Cache Miss!| Root

    Leaf1 ===>|Tagged Ptr| Seg0
    Leaf2 ===>|Tagged Ptr| Seg1
    Leaf2 ===>|Tagged Ptr| Seg2
```

### Memory Request Workflow
1. **Thread Local Cache (TLS) Check**: When a thread needs to resolve an offset pointer (VA -> Sub-Segment), it checks its Thread Local LRU cache. The cache stores `[base_addr, end_addr) -> Sub-Segment Pointer`.
2. **Passive Eviction**: If the target slot is found, the thread checks the leaf node slot in the trie atomically to verify the segment wasn't unmapped (lock-free passive eviction check).
3. **Cache Hit**: The thread resolves the pointer and proceeds instantly. 
4. **Cache Miss**: The thread performs an $O(Levels)$ atomic lock-free traversal through the Trie to find the correct sub-segment pointer. It inserts this pointer into index `0` of its TLS Cache, pushing older entries down (LRU).

## Concurrent Growth and Trie Registration

When the Segmented Memory Manager runs out of space, it must register new `sub_segment`s into the N-CRIT trie.

```mermaid
sequenceDiagram
    participant Allocator as Segment Manager
    participant Trie as N-CRIT Trie
    participant Threads as Concurrent Threads
    
    Allocator->>Allocator: Check free memory < request
    Allocator->>Allocator: Allocate new chunk / map SHM
    Allocator->>Trie: insert_range(base, size, &seg)
    
    loop Per Page
        Trie->>Trie: Walk Trie levels
        alt Node Missing
            Trie->>Trie: CAS allocate node (acq_rel)
        end
        Trie->>Trie: store(seg | kLeafTag, release)
    end
    
    Trie->>Threads: invalidate_cache()
    
    note right of Threads: Next VA lookup will cache miss<br>and fetch the new segment from Trie
    
    Allocator->>Allocator: Initialize Boost rbtree_best_fit
    Allocator->>Allocator: Proceed with user allocation
```

### Trie Concurrency Safety Mechanics
- **Insertions (Growths)**:
  - Internal nodes are allocated via compare-and-swap (`CAS`). If two threads race to map memory into the same trie branch, the winner's node is used and the loser deletes its node. `memory_order_acq_rel` establishes happens-before edges.
  - Leaf nodes are updated via `memory_order_release`.
- **Lookups (Offset Pointer Resolution)**:
  - Threads read nodes using `memory_order_acquire`.
  - Stale readers reading mid-insertion might see a missing page (`nullptr`) or the new segment. Since the allocator won't return pointers in unmapped ranges, no application thread will ever access a missing trie branch during normal pointer resolution.
- **Node Lifecycle**:
  - The trie operates without Hazard Pointers or RCU. Internal nodes are never destroyed until the entire `ncrit_trie` is torn down.
  - Sub-segments can be unmapped, but they are replaced with tombstone entries (`0`) at the leaf level using an atomic release. Wait-free TLS invalidation protects cached lookups.
