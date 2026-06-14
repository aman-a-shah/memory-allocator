# Custom Memory Allocator — Phased Development Plan

This document decomposes the work described in `plan.md` into small, independently
shippable phases. Each phase has a clear scope, concrete deliverables, exit
criteria, and a dependency on prior phases. The goal is to keep every phase small
enough to build, test, and verify in isolation before layering on the next one.

> **Reference targets** (from `plan.md`): O(1) worst-case `malloc`/`free`,
> ≥6× throughput over ptmalloc, <3% fragmentation, zero kernel switches / lock
> contention on the critical path.

---

## Phase 0 — Project Scaffolding & Tooling
**Goal:** A buildable, testable skeleton before any allocator logic exists.

**Scope**
- CMake project (C++17), strict warnings (`-Wall -Wextra -Werror`).
- Directory layout: `include/`, `src/`, `tests/`, `bench/`.
- Wire up a unit-test framework (GoogleTest) and a benchmark framework (Google Benchmark) as dependencies.
- CI hooks for build + test (optional locally).
- Sanitizer build presets: ASan, UBSan, TSan.

**Deliverables**
- `CMakeLists.txt` that builds an empty static lib `memalloc` + a hello-world test.
- `README` snippet describing build commands.

**Exit criteria**
- `cmake --build` succeeds; one trivial passing test; sanitizer presets compile.

**Depends on:** nothing.

---

## Phase 1 — OS Arena Layer (mmap backing store)
**Goal:** Reserve and manage one large contiguous region from the OS in user space.

**Scope**
- `Arena` abstraction wrapping `mmap(..., MAP_PRIVATE | MAP_ANONYMOUS, ...)`.
- Bump/cursor sub-allocation for handing out large chunks to the engines.
- Alignment helpers (8-byte and 64-byte), `align_up` / `align_down`.
- Clean teardown via `munmap`.

**Deliverables**
- `arena.hpp` / `arena.cpp`.
- Unit tests: allocation returns aligned, in-range, non-overlapping pointers; exhaustion handled gracefully.

**Exit criteria**
- Can reserve N MB, carve aligned sub-blocks, and release. No leaks under ASan.

**Depends on:** Phase 0.

---

## Phase 2 — Fixed-Size Slab Allocator (single-thread)
**Goal:** O(1) fixed-size allocation for repetitive trading objects.
*(Maps to plan.md Week 1.)*

**Scope**
- Slab carved from arena into identical fixed-size slots.
- **Intrusive free-list**: free slots store the `next` pointer in their own memory.
- **Zero header overhead**: live allocations return raw pointers, no per-object header.
- `SlabPool<T>` (or size-parameterized pool) with `allocate()` / `deallocate()`.
- Slab growth: request a new slab chunk from the arena when full.

**Deliverables**
- `slab.hpp` / `slab.cpp`.
- Tests: round-trip alloc/free, recycle reuses freed slots, alignment correct, exhaustion + growth path.
- Targeted pools for `Order` and `TradeExecution` sample structs.

**Exit criteria**
- Allocation/free are pointer-chase O(1); recycling produces zero arena growth.

**Depends on:** Phase 1.

---

## Phase 3 — TLSF Block Model & Bitmap Index (single-thread, no coalescing)
**Goal:** The two-level segregated free-list matrix and O(1) bin selection.
*(Maps to plan.md Week 2.)*

**Scope**
- `BlockHeader` layout (`size`, `flags`, `next_free`, `prev_free`) per plan.md §3.
- Size → `(f, s)` mapping using bit math (first-level power-of-two, second-level linear sub-bins).
- Two-level bitmap masks; bit-scan via `__builtin_ctz` / `__builtin_clz` (with portable fallback).
- Insert/remove block from a segregated bin; "find smallest fitting bin" lookup.
- **No splitting/coalescing yet** — fixed pre-seeded blocks to validate indexing.

**Deliverables**
- `tlsf.hpp` / `tlsf.cpp` (indexing + free-list matrix only).
- Tests: mapping correctness across boundary sizes; bitmap set/clear; "good-fit" bin returned in O(1).

**Exit criteria**
- For any request size, the engine returns the correct bin index with no loops.

**Depends on:** Phase 1.

---

## Phase 4 — TLSF Splitting, Coalescing & Allocation Path
**Goal:** Full variable-size allocate/free with fragmentation control.
*(Maps to plan.md Week 3.)*

**Scope**
- `malloc(S)`: locate bin → pop block → **split** remainder back into matrix.
- `free(p)`: read boundary tags → **bi-directional coalesce** with free neighbors → re-insert merged block.
- Maintain `IsAllocated` / `IsPreviousAllocated` flags + boundary tags.
- Strict alignment enforcement on every returned pointer.

**Deliverables**
- Completed `tlsf.cpp` allocation/deallocation path.
- Tests: split correctness, forward/backward/both-sides coalescing, alignment, fragmentation accounting.

**Exit criteria**
- Stress test of mixed sizes shows reclaimed memory merges; no permanent fragmentation; O(1) paths hold.

**Depends on:** Phase 3.

---

## Phase 5 — Unified Allocator Façade
**Goal:** One entry point routing requests to Slab vs TLSF.

**Scope**
- Public API: `allocate(size)`, `deallocate(ptr)`, plus typed slab helpers.
- Routing policy: fixed/registered sizes → Slab; variable sizes → TLSF.
- Ownership tagging so `deallocate` knows which engine owns a pointer.

**Deliverables**
- `allocator.hpp` / `allocator.cpp` façade.
- Integration tests exercising both engines through the single API.

**Exit criteria**
- Mixed workloads route correctly and free to the right engine.

**Depends on:** Phases 2 & 4.

---

## Phase 6 — Thread-Local Caching & Multi-Thread Safety
**Goal:** Lock-free critical path across threads.
*(Maps to plan.md §5 interview defenses.)*

**Scope**
- `thread_local` per-thread Slab + TLSF arenas.
- Global shared arena refill via atomic CAS when a local pool drains.
- **Cross-thread free**: per-arena lock-free SPSC remote-free queue; block header records "home" thread arena; owner drains the queue on its next local alloc.

**Deliverables**
- TLS layer + remote-free queue.
- Tests under TSan: concurrent alloc/free, cross-thread free correctness, no data races.

**Exit criteria**
- No mutex on the hot path; TSan-clean under concurrent stress.

**Depends on:** Phase 5.

---

## Phase 7 — Benchmarking & Financial Trace Analysis
**Goal:** Prove the performance targets.
*(Maps to plan.md Week 4.)*

**Scope**
- Google Benchmark suite vs glibc `malloc`/`free`.
- Synthetic HFT workload generator: burst order creation → rapid cancel/execute churn.
- Metrics: throughput (≥6× target), latency distribution / determinism, fragmentation (<3%), 10,000+ continuous cycles.
- Final TSan/ASan validation pass.

**Deliverables**
- `bench/` suite + workload generator.
- Results report (CSV/markdown) comparing against ptmalloc.

**Exit criteria**
- Documented evidence of meeting the four core performance targets.

**Depends on:** Phase 6 (or Phase 5 for single-thread baseline numbers).

---

## Dependency Graph
```
Phase 0
  └─ Phase 1
       ├─ Phase 2 ─────────────┐
       └─ Phase 3 ─ Phase 4 ───┤
                                └─ Phase 5 ─ Phase 6 ─ Phase 7
```

## Suggested Milestones
- **M1 (Single-thread MVP):** Phases 0–5 — a working dual-engine allocator.
- **M2 (Concurrent):** Phase 6 — lock-free multi-threaded operation.
- **M3 (Validated):** Phase 7 — benchmarked against targets.
