# Custom Memory Allocator — Complete Project Summary

A self-contained reference for the whole project: the problem, the architecture,
the implementation of every component, the math, the concurrency model, the
benchmark methodology, the measured results, and how the research-note PDF is
built. Reading this file should be enough to understand the system end to end
without opening the source.

---

## 1. Problem & objectives

General-purpose allocators (glibc `ptmalloc`, Apple's libc `malloc`) are tuned
for the average case. Low-latency / high-frequency-trading (HFT) systems instead
need **deterministic** allocation: a single multi-microsecond stall during a
market burst is a missed fill. They also churn through a very specific traffic
profile — a flood of small, fixed-shape records (orders, executions, tick
updates) plus a smaller stream of variable-length ingestion buffers.

**Goal:** a hybrid allocator with

| Target | Goal |
|---|---|
| Time complexity | **O(1) worst case** for both `malloc` and `free` |
| Throughput | ≥6× over glibc ptmalloc (aspirational; see results) |
| Fragmentation | **< 3%** internal + external under a financial workload |
| Determinism | **zero** kernel context switches and **zero** lock contention on the hot path |

Language: **C++17**, no third-party allocator code.

---

## 2. High-level architecture

At startup the allocator reserves one large contiguous region from the OS via
`mmap(MAP_PRIVATE | MAP_ANONYMOUS)` and manages it entirely in user space (no
kernel transitions on the critical path). A **size router** sends each request
to one of two purpose-built engines:

```
                 ┌───────────────────────────────┐
                 │   OS Arena  (one mmap region)  │
                 └───────────────┬───────────────┘
                                 │  bump / CAS-carved sub-blocks
              ┌──────────────────┴──────────────────┐
              ▼                                      ▼
   ┌────────────────────┐                ┌────────────────────────┐
   │   Slab engine      │                │   TLSF engine          │
   │ fixed-size records │                │ variable-size requests │
   │ intrusive freelist │                │ 2-level bitmap index   │
   │ zero header        │                │ split + coalesce       │
   └────────────────────┘                └────────────────────────┘
        ≤ 256 B fixed                          everything else
```

A single-thread façade (`Allocator`) wraps both engines; a lock-free layer
(`ConcurrentAllocator`) replicates the façade per thread for multithreaded use.

### Source layout
```
include/memalloc/   align.hpp arena.hpp slab.hpp tlsf.hpp allocator.hpp concurrent.hpp memalloc.hpp
src/                arena.cpp slab.cpp tlsf.cpp allocator.cpp concurrent.cpp memalloc.cpp
tests/              align_ arena_ slab_ tlsf_ tlsf_alloc_ allocator_ concurrent_ test.cpp  (74 tests)
bench/              hft_workload.hpp  alloc_bench.cpp (Google Benchmark)  metrics.cpp (JSON generator)
site/               index.html styles.css charts.js export_data.py render_math.cjs build_pdf.sh  -> research-note.pdf
```

---

## 3. Component A — OS Arena (`arena.hpp/.cpp`)

The backing store. `Arena(capacity_bytes)` rounds up to a page multiple and
`mmap`s an anonymous region (zero-filled, lazily committed). It hands out aligned
sub-blocks with a **bump (cursor) allocator**:

- `allocate(bytes, alignment)` — aligns the cursor up (`align_up`), bounds-checks
  (overflow-safe), advances the cursor, returns the pointer. O(1). The arena does
  **not** track or reclaim individual sub-blocks; only `reset()` (rewind) or
  destruction (`munmap`) frees memory. The engines layered on top do reclamation.
- `owns(p)` — is `p` inside this mapping? (used to route frees).
- A **borrowed-memory constructor** `Arena(void* base, size_t cap)` adopts an
  already-mapped region without owning it (no `munmap` on destruction). This is
  how the concurrent layer runs an engine over a superblock carved from the one
  global mapping. A `bool owns_mapping_` flag, carried through the move ctor/assign,
  controls whether the destructor unmaps.

Alignment helpers (`align.hpp`): `align_up`, `align_down`, `is_power_of_two`.
Minimum alignment is `alignof(std::max_align_t)`; 8- and 64-byte alignment supported.

---

## 4. Component B — Slab engine (`slab.hpp/.cpp`)

Fast path for repetitive fixed-size records. A `SlabPool` carves a block from
the arena into identical slots. Two properties make it cheap:

- **Intrusive free list** — a free slot stores the "next free" pointer *inside
  its own unused memory* (`struct FreeNode { FreeNode* next; }`), so there is no
  external bookkeeping structure. `allocate()` pops the head; `deallocate()`
  pushes onto the head. Both are single pointer swaps — **O(1)**.
- **Zero header overhead** — a live allocation is a raw slot pointer with no
  prepended metadata. Every slot in a pool is the same size, so `free` just
  pushes the pointer back; it never needs to read a per-object header. This keeps
  cache lines dense and alignment intact.

Slot size is rounded up to hold the free-list pointer and to a multiple of the
alignment. When a pool runs dry it requests one more block from the arena
(`grow()`), threads all its slots onto the free list, and (optionally) notifies a
`SlabBlockObserver` of the new block's address range. `ObjectPool<T>` is a typed
wrapper adding in-place `create()`/`destroy()`.

Because the slab path is zero-header, `free()` can't read a tag to learn the
owning engine — that problem is solved by the façade's address-range registry (§6).

---

## 5. Component C — TLSF engine (`tlsf.hpp/.cpp`)

Two-Level Segregated Fit: O(1) good-fit allocation for variable sizes, with
splitting and boundary-tag coalescing.

### Block layout
```
+--------+--------+-----------+------------------- payload -------------------+
|  size  | flags  | prev_phys |  (next_free / prev_free overlap here if free) |
+--------+--------+-----------+----------------------------------------------+
^                             ^
block header (16 B)           pointer returned to the caller
```
`BlockHeader { uint32_t size; uint32_t flags; BlockHeader* prev_phys; BlockHeader* next_free; BlockHeader* prev_free; }`.
- `size` = payload bytes (excludes the 16-byte header).
- `flags` bit 0 = `IsAllocated`, bit 1 = `IsPreviousAllocated`.
- `prev_phys` is the boundary tag for backward coalescing; the next physical
  block is reached by `block + 16 + size`.
- `next_free`/`prev_free` are valid only while the block is free and **overlap
  the payload** (zero cost while allocated).

### Size → bin mapping (the math)
Each size maps to a 2-D bin coordinate `(fl, sl)`:

- First level (coarse, power-of-two class):  `fl = floor(log2(size))`
- Second level (linear subdivision into `SL = 32` sub-bins):
  `sl = floor( (size − 2^fl) / 2^(fl − L) )`,  where `L = log2(SL) = 5`.

Computed with `__builtin_clz`/`__builtin_ctz` (portable fallbacks provided).
Tuning constants: `kAlignSize = 8`, `kSLIndexCount = 32`, `kFLIndexCount = 25`,
`kSmallBlockSize = 256` (below it the linear first level applies),
header overhead = 16 B, min payload = 16 B.

### Two-level bitmap + O(1) good-fit
`FreeListMatrix` holds `BlockHeader* blocks[FL][SL]` plus a first-level bitmap
(`fl_bitmap`, bit `fl` set ⇒ that first level has a non-empty bin) and per-level
second-level bitmaps (`sl_bitmap[fl]`). Finding the smallest block that fits a
request is two bit-scans, no loop:

```
mask   = sl_bitmap[fl] & (~0 << sl)      // sub-bins ≥ requested in this level
if mask: sl' = ctz(mask)                 // smallest fitting sub-bin
else:    fl' = ctz(fl_bitmap & (~0 << (fl+1)))   // next non-empty first level
         sl' = ctz(sl_bitmap[fl'])
```
`insert`/`remove` set/clear the bitmap bits as bins fill and empty.

### Allocate / free path
- **allocate(S):** round the request up within its class, find the suitable bin,
  pop a block, **split** off the remainder (if it can hold a header + min payload)
  and re-insert it, mark the block allocated, fix the next block's
  `IsPreviousAllocated` flag, return the payload pointer.
- **free(p):** read boundary tags; if the previous and/or next physical neighbor
  is free, **remove them from their bins and merge** into one larger block
  (bidirectional coalescing), then insert the merged block. Keeps external
  fragmentation flat.

The engine carves regions from the arena, lays each out as one big free block
bounded by a zero-size allocated sentinel, and grows by adding regions when dry.
`validate()` walks every region checking all invariants (used by tests).

### Why TLSF over a buddy allocator
A buddy allocator rounds to powers of two → up to ~50% internal waste for a
request just above a boundary. TLSF's second level subdivides each power-of-two
class into `SL` linear steps, bounding internal waste:

`internal_waste / block_size  <  1 / SL  =  1/32  ≈  3.1%`,  independent of size,
while keeping O(1) via the bitmaps.

---

## 6. Component D — Unified façade (`allocator.hpp/.cpp`)

`Allocator` owns one `Arena`, one `Tlsf`, and a `SlabPool` per size class
(default classes `{16, 32, 64, 128, 256}`). Routing:

- **allocate(bytes):** if `bytes ≤ 256` → smallest fitting slab pool (fall through
  to TLSF if that pool can't grow); else → TLSF.
- **deallocate(p):** the slab path is zero-header, so the façade keeps a
  **sorted address-range registry**. Each slab pool reports every block it carves
  (`SlabBlockObserver::on_slab_block`); `deallocate` binary-searches the ranges —
  a hit routes to the owning pool, a miss routes to TLSF. This preserves the
  zero-header slab fast path while still routing frees correctly.

Also exposes typed `create<T>()/destroy<T>()`, live counters, `owns(p)`, and
introspection (`tlsf_used_bytes`, `arena_used_bytes`) used by the benchmarks.
An external-arena constructor `Allocator(Arena&&, Config)` lets the concurrent
layer build a façade over a borrowed superblock.

---

## 7. Component E — Lock-free multithreading (`concurrent.hpp/.cpp`)

`ConcurrentAllocator` makes the hot path lock-free across threads.

- **Global arena + CAS superblocks.** One large `mmap` region; threads carve
  fixed-size **superblocks** (default 32 MiB; global default 1 GiB) via an atomic
  compare-and-swap bump cursor — the only synchronization on the cold path,
  hit once per thread.
- **Per-thread cache.** Each thread lazily creates a `ThreadCache` = a full
  single-thread `Allocator` (slab + TLSF) over a borrowed superblock. Allocation
  and same-thread free therefore take **no locks and touch no shared state** —
  they are exactly the Phase-5 fast paths.
- **Cross-thread free (the hard case).** A block freed by a thread other than its
  owner is **not** touched in place. It is pushed onto the owning cache's
  **lock-free MPSC "remote-free" stack** — a Treiber stack threaded through the
  freed block's own memory (the intrusive `next` link is stored in `*(void**)p`,
  so zero extra storage; every block is ≥ 8/16 B). The owner **drains** the whole
  list in one atomic `exchange` at the top of its next allocation and recycles the
  slots locally. Memory ordering: `release` on push, `acquire` on drain.
- **Ownership by address.** Each cache's superblock is a disjoint range, so a free
  resolves its owner by walking a lock-free registry of caches and asking each
  `owns(p)` (arena bounds are immutable ⇒ no lock needed). Local frees skip the
  walk entirely (the common case).

The whole layer is verified race-free under **ThreadSanitizer**, and leak-free
under **AddressSanitizer**.

### Interview-defense mapping
- *Multithreading without locking up* → per-thread TLS caches + CAS-only refill.
- *TLSF vs buddy* → second-level subdivision bounds internal frag to <3.1% at O(1).
- *Object allocated by A, freed by B* → lock-free remote-free queue keyed to A's
  home arena; A drains it on its next alloc.

---

## 8. Phased development (all complete)

```
Phase 0  Scaffolding (CMake C++17, GoogleTest + Google Benchmark, asan/tsan presets)
Phase 1  OS arena (mmap, bump, alignment, teardown)
Phase 2  Fixed-size slab (intrusive freelist, zero header)
Phase 3  TLSF block model + 2-level bitmap index (no coalescing)
Phase 4  TLSF splitting + bidirectional coalescing + full alloc/free path
Phase 5  Unified façade (slab/TLSF routing + address-range registry)
Phase 6  Thread-local caches + CAS global arena + lock-free remote-free queue   ← added
Phase 7  HFT workload + Google Benchmark suite + metrics generator              ← added
```

---

## 9. Benchmark methodology (`bench/`)

- **`hft_workload.hpp`** — a deterministic (xorshift64*) generator producing a
  bursty trace of `Alloc`/`Free` events with a bounded live set: ~80% small
  fixed-shape records (Order/TradeExecution-shaped, slab path) and ~20%
  variable-length buffers (200 B–16 KB, TLSF path), in a burst-then-drain shape.
  The same trace is replayed through every allocator under test.
- **`alloc_bench.cpp`** — Google Benchmark suite: fixed churn, variable churn,
  and the full HFT trace, custom vs system `malloc`.
- **`metrics.cpp` → JSON** — the figure generator for the PDF. Measures
  (best-of-N to suppress scheduler noise; warm-up passes to exclude first-touch
  page faults):
  1. throughput per scenario (slab / TLSF / mixed),
  2. per-op latency distribution + tail percentiles (p50…max),
  3. latency vs request size (the O(1) curve; batch-timed because individual ops
     are below the host's ~41 ns timer granularity),
  4. throughput vs thread count (1–8),
  5. fragmentation over the workload,
  6. throughput + resident footprint over 12,000 cycles.

Caveat captured in the note: on Apple Silicon the cheap timer granularity is
~41 ns, so sub-tick latencies are reported as `<41`; the determinism story lives
in the resolvable tail.

---

## 10. Measured results (Apple M4, vs libc malloc)

| Metric | Result | Target | Met |
|---|---|---|---|
| Worst-case time complexity | O(1) — flat across 16 B–64 KB | O(1) | ✅ |
| Throughput (slab / TLSF / mixed) | ~3.3× / ~4.2× / ~2.2–2.4× | ≥6× (vs glibc) | ⚠️ partial |
| Memory fragmentation | **0.24%** of live bytes | <3% | ✅ |
| Latency tail (p99.9) | ~6–10× tighter than malloc | tight | ✅ |
| Hot-path kernel switches | 0 (user-space arena) | 0 | ✅ |
| Hot-path lock contention | lock-free (TSan-clean) | 0 | ✅ |
| Scaling | near-linear to 8 threads, ~2.4× malloc throughout | — | ✅ |
| Stability | footprint flat over 12k cycles (no leak/growth) | — | ✅ |

**Honesty note:** the ≥6× target was framed against glibc `ptmalloc`. Apple's
libc allocator is a much stronger baseline, so the measured 2–4× is conservative
relative to the classic comparison; that one target is marked unmet in the note
rather than massaged. Run-to-run figures vary by a few tenths (real timing noise);
nothing is mocked — every number comes from a live run.

74 unit tests pass under **release, ASan, and TSan**.

---

## 11. Build & run

```sh
cmake --preset release && cmake --build --preset release
ctest --preset release                       # 74 tests; also: --preset asan / tsan
./build/release/bench/memalloc_bench         # Google Benchmark throughput suite
./build/release/bench/memalloc_metrics out.json   # full metrics → JSON
```
Presets: `release` (-O3), `debug`, `asan` (ASan+UBSan), `tsan`.
GoogleTest and Google Benchmark are fetched via CMake `FetchContent`
(network needed on first configure).

---

## 12. The research-note PDF (`site/`)

An editorial, research-paper-style PDF built per `QUANT_SHOWCASE_STYLE.md`:
cool near-white paper, single crimson accent (slate-blue only as the 2nd chart
series), Source Serif 4 + JetBrains Mono, hand-rolled dependency-free SVG charts,
KaTeX math, hairline-separated layout, no cards/shadows.

**Pipeline** (`build_pdf.sh`): `export_data.py` runs the real `memalloc_metrics`
binary → `data.json` (+ inlined `data.js`); `render_math.cjs` pre-renders the
KaTeX equations server-side → `print.html`; headless Chrome prints it to
`research-note.pdf`; `verify_pdf.py` measures per-page whitespace. No numbers are
hand-entered.

**Content:** masthead (dateline → h1 → lede → 4 headline stats → hero latency
distribution → TOC), then five numbered sections — 01 Dual-engine architecture
(pipeline diagram, parameter grid, TLSF mapping + bit-scan equations), 02
Throughput (bar chart + hero-number callout), 03 Latency & determinism
(percentile table + O(1)-by-size chart), 04 Concurrency & scaling (scalability
chart + lock-free push/drain equation), 05 Fragmentation & stability (fragmentation
curve, footprint plateau, PRD-targets scorecard) — and a colophon with the exact
reproduce command. Output: 6 Letter pages, interior whitespace ≤11%, no orphans.

**Build-environment note:** assets (KaTeX + both fonts) are vendored into
`site/vendor/` so the page renders fully offline; on this machine CDP `printToPDF`
and `--virtual-time-budget` are unreliable, so the build uses the plain
command-line `--print-to-pdf` with an absolute output path and server-side math.
Rebuild anytime with `./site/build_pdf.sh`.
