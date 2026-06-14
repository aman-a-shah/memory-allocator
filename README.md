# Custom Memory Allocator

A hybrid **Slab + TLSF** memory allocator targeting deterministic, O(1)
allocation for low-latency / financial workloads. See [`plan.md`](plan.md) for
the full spec and [`phases.md`](phases.md) for the phased development plan.

> **Status:** Phase 0 (project scaffolding) complete. The allocator itself is
> introduced in subsequent phases.

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
cmake --build --preset release --target memalloc_bench
./build/release/bench/memalloc_bench
```

## Layout
```
include/memalloc/   public headers
src/                library implementation
tests/              GoogleTest unit tests
bench/              Google Benchmark suites
```
