#include <benchmark/benchmark.h>

#include "memalloc/memalloc.hpp"

// Phase 0 smoke benchmark: proves the Google Benchmark harness links and runs.
// The real malloc/free vs. ptmalloc comparison suite arrives in Phase 7.
static void BM_VersionQuery(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(memalloc::version_string());
    }
}
BENCHMARK(BM_VersionQuery);

BENCHMARK_MAIN();
