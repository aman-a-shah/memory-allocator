Product Requirement Document (PRD) & Engineering Spec
1. Executive Summary & Core Objectives
The objective is to design and implement a highly optimized, deterministic Custom Memory Allocator in C++17 utilizing a hybrid Two-Level Segregated Fit (TLSF) strategy for variable-sized allocations alongside a highly optimized Slab Allocator for fixed-size trading objects (e.g., Order, TradeExecution).
Core Performance Targets
Time Complexity: Strict O(1) worst-case time complexity for both allocation (malloc) and deallocation (free) paths.
Throughput Efficiency: Reduce allocation/deallocation overhead by ≥6× compared to glibc’s general-purpose allocator (ptmalloc).
Memory Utilization: Keep total internal and external memory fragmentation below 3% under simulated financial workload traces.
Deterministic Latency: Zero kernel context switches and zero execution-path lock contention during the critical path.
2. Architectural Design & Allocation Paradigms
The allocator employs a dual-engine layout. At startup, the system requests a large contiguous block of memory from the OS via mmap and manages it entirely in user space.
                  ┌─────────────────────────────────────────┐
                  │       Custom Memory Arena (mmap)        │
                  └────────────────────┬────────────────────┘
                                       │
         ┌─────────────────────────────┴─────────────────────────────┐
         ▼                                                           ▼
┌─────────────────────────────────┐                         ┌─────────────────────────────────┐
│     Slab Allocator Engine       │                         │      TLSF Allocator Engine      │
│  (For Fixed-Size Trading Data)  │                         │ (For Variable-Size Ingestion)   │
├─────────────────────────────────┤                         ├─────────────────────────────────┤
│ • Segregated Object Pools       │                         │ • Two-Level Bitmap Indices      │
│ • Free-List Intrusive Arrays    │                         │ • Segregated Free-List Matrix   │
│ • Zero Header Allocation        │                         │ • Immediate Block Coalescing    │
└─────────────────────────────────┘                         └─────────────────────────────────┘
Component A: The Fixed-Size Slab Allocator
Financial applications handle highly repetitive data structures (orders, tick updates, execution reports). The Slab Allocator pre-carves memory chunks into pools of identical, fixed-size slots.
Intrusive Free-List Array: To eliminate tracking overhead, unallocated slots contain a pointer to the next free slot inside their own unallocated memory space. No external metadata tracking structures are used.
Zero Header Overhead: Because every object in a specific slab is exactly the same size, individual allocations do return raw pointers without prepending an allocation header byte. This saves cache space and prevents alignment disruptions.
Component B: The Variable-Size TLSF Engine
For variable-sized allocations (e.g., parsing varying-length network packets or dynamic string lists), a standard sequential search through a free-list creates O(N) jitter. The Two-Level Segregated Fit (TLSF) design uses a two-tiered matrix of free lists indexed by hardware bitmaps to achieve guaranteed O(1) performance.
The Matrix Structure:
First-Level Category (f): Segregates memory blocks by coarse-grained powers of two (e.g., 2 
5
 =32 bytes, 2 
6
 =64 bytes, 2 
7
 =128 bytes, etc.).
Second-Level Category (s): Linearly subdivides each first-level range into a fixed number of sub-bins (e.g., 4 or 8 subdivisions) to minimize internal fragmentation.
Hardware-Accelerated Allocation Algorithm:
When an allocation request of size S occurs, the engine maps S to its exact (f,s) matrix coordinate using mathematical bitwise manipulation.
It checks a Two-Level Bitmap Mask where a set bit (1) indicates that a free memory block exists in that specific bin.
To find the smallest available block that fits the request, it executes an x86 assembly instruction sequence (bsfl / Bit Scan Forward or lzcnt / Leading Zero Count via compiler intrinsics like __builtin_clz). This finds the correct free list index in a single CPU clock cycle, bypassing loops completely.
3. Advanced Memory Mechanics & Fragmentation Control
Data Structure Topology & Alignment
All allocations must be strictly aligned to 8-byte or 64-byte boundaries (alignof(std::max_align_t)) to ensure the underlying hardware can read the data in a single memory bus cycle without triggering unaligned access penalties.
C++
#include <cstdint>
#include <cstddef>

// Meta-header for variable-sized blocks inside the TLSF engine
struct BlockHeader {
    uint32_t size;             // Size of the current chunk
    uint32_t flags;            // Bit 0: IsAllocated, Bit 1: IsPreviousAllocated
    BlockHeader* next_free;    // Only utilized if the block is currently free
    BlockHeader* prev_free;    // Only utilized if the block is currently free
};
Boundary-Tag Coalescing
To eliminate external fragmentation (where memory is split into tiny, unusable fragments), the TLSF engine performs immediate bi-directional coalescing whenever a block is freed:
The allocator examines the flags field of the current block header.
If the adjacent physical neighbor blocks (either immediately before or after in memory) are also free, they are decoupled from their current segregation bins, merged into a single larger contiguous block, and re-inserted into the appropriate higher-tier (f,s) bin.
4. Execution & Implementation Timeline (4-Week Plan)
Week 1: Fixed-Size Slab Engine & Arena Initialization
Deliverables: Thread-safe, fixed-size chunk pools using intrusive pointers and zero allocation headers.
Tasks: Implement the OS virtual memory allocation layer using mmap(..., MAP_PRIVATE | MAP_ANONYMOUS, ...). Construct the Slab interface targeting standard financial structures (Order, Trade). Ensure that object recycling incurs zero runtime allocation side-effects.
Week 2: TLSF Matrix Design & Bitmask Control
Deliverables: Two-level matrix tracking infrastructure integrated with x86 hardware bit-scan intrinsics.
Tasks: Program the bit-mapping functions converting allocation sizes directly to matrix coordinates. Implement the bitmask search logic utilizing __builtin_ctz or _BitScanForward to achieve hardware-level O(1) bin detection.
Week 3: Variable Block Management & Coalescing Logic
Deliverables: Boundary-tag allocation blocks with automated forward and backward merging functionality.
Tasks: Write the dynamic splitting logic (splitting a large block when a smaller allocation request arrives) and the inverse coalescing logic triggered on a free invocation. Enforce strict data alignment rules.
Week 4: Benchmark Engineering & Financial Trace Analysis
Deliverables: Google Benchmark integration suite demonstrating execution comparisons against glibc malloc.
Tasks: Construct a synthetic high-frequency trading workload generator mimicking real-world traffic profiles (massive bursts of order creations followed by rapid cancellations and executions). Profile the engine with ThreadSanitizer to guarantee safety and evaluate performance statistics across 10,000+ continuous cycles.
5. Interview Strategy: How to Defend Your Architecture
When facing senior infrastructure quants, anticipate deep questions about memory safety and hardware-level performance. Use these technical defenses:
How does your allocator handle multi-threaded workloads without locking up?
The Answer: "To bypass the lock contention common in general-purpose allocators, this design uses a Thread-Local Storage (TLS) cache layer. Each execution thread owns its own dedicated, isolated Slab and TLSF arena pool via the thread_local keyword. Threads allocate and free from their personal memory pool without acquiring any mutexes. If a thread's local pool runs out of memory, it requests a large chunk from a global shared arena using an atomic compare-and-swap (CAS) operation, keeping cross-thread interference to a minimum."
Why did you choose a TLSF allocator over a Buddy Allocator system?
The Answer: "A Buddy Allocator splits memory blocks in halves, meaning block sizes must scale as powers of two (2 
N
 ). This can introduce up to 50% internal fragmentation if an application requests an allocation size just slightly above a power-of-two boundary. TLSF solves this by adding a second level of segregation that subdivides those power-of-two ranges into smaller, linear steps. This reduces internal fragmentation to less than 3% while still maintaining strict O(1) performance through bitmap tracking."
How do you safely handle an object that was allocated by Thread A but freed by Thread B?
The Answer: "Cross-thread deallocation is a common source of performance degradation. To address this, each memory block's header contains a reference identifying its originating 'home' thread arena. When Thread B frees an object belonging to Thread A, it does not modify Thread A’s pool directly. Instead, it pushes the pointer into a lock-free, atomic SPSC queue linked to Thread A's arena. When Thread A performs its next local allocation, it drains its remote-free queue first and reclaims those slots locally, keeping thread execution fast and decoupled."