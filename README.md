# Custom Memory Allocator

A hybrid **Slab + TLSF** memory allocator targeting deterministic, O(1)
allocation for low-latency / financial workloads. See [`plan.md`](plan.md) for
the full spec and [`phases.md`](phases.md) for the phased development plan.

> **Status:** All phases complete (see [`phases.md`](phases.md)). Single-thread
> dual-engine allocator (arena + slab + TLSF), a lock-free multi-threaded layer
> (per-thread caches + atomic-CAS global arena + lock-free remote-free queues),
> and a Phase 7 benchmark/metrics suite vs the system allocator. 74 unit tests
> pass under Release, ASan, and TSan.
>
> **Measured (Apple M4, vs libc malloc):** 2.2–3.7× throughput, p99.9 latency
> ~7.5× tighter, O(1) latency across request sizes, 0.24% fragmentation, and
> near-linear scaling to 8 threads. A typeset research note summarizing the
> results is built under [`site/`](site/) (`./site/build_pdf.sh`).

## Requirements
- C++17 compiler (Apple clang / clang / gcc)
- CMake ≥ 3.20
- Network access on first configure (GoogleTest and Google Benchmark are
  fetched via CMake `FetchContent`)

## Build

```sh
# Configure + build a Release tree
cmake --preset release
cmake --build --preset release

# Run the unit tests
ctest --preset release
```

### Available presets
| Preset    | Purpose                          |
|-----------|----------------------------------|
| `release` | Optimized `-O3` build            |
| `debug`   | Unoptimized build with symbols   |
| `asan`    | AddressSanitizer + UBSan         |
| `tsan`    | ThreadSanitizer                  |

Swap `release` for any preset above, e.g.:

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

## Benchmarks

```sh
cmake --build --preset release --target memalloc_bench memalloc_metrics
# Google Benchmark throughput suite vs the system allocator:
./build/release/bench/memalloc_bench
# Full metrics run (throughput, latency tails, fragmentation, scaling) -> JSON:
./build/release/bench/memalloc_metrics /tmp/metrics.json
```

## Layout
```
include/memalloc/   public headers
src/                library implementation
tests/              GoogleTest unit tests
bench/              Google Benchmark suites
```
