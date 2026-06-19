// Phase 7 Google Benchmark suite: the custom allocator vs the system allocator
// on representative trading-allocation patterns. Run with:
//
//   ./build/release/bench/memalloc_bench --benchmark_min_time=0.2s
//
// The headline figures for the research note come from bench/metrics.cpp (which
// also measures latency tails and fragmentation); this suite is the canonical
// throughput harness and the regression guard.

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <vector>

#include "hft_workload.hpp"
#include "memalloc/allocator.hpp"

namespace {

using memalloc::Allocator;
namespace hft = memalloc::hft;

Allocator::Config bench_cfg() {
    Allocator::Config c;
    c.arena_bytes = 256u * 1024 * 1024;
    c.tlsf_region_bytes = 4u * 1024 * 1024;
    return c;
}

// --- Fixed-size churn (slab path) -----------------------------------------
void BM_Custom_FixedChurn(benchmark::State& state) {
    Allocator a(bench_cfg());
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<void*> live(n, nullptr);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) live[i] = a.allocate(64);
        for (std::size_t i = 0; i < n; ++i) a.deallocate(live[i]);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n) * 2);
}
BENCHMARK(BM_Custom_FixedChurn)->Arg(1024);

void BM_System_FixedChurn(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<void*> live(n, nullptr);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) live[i] = std::malloc(64);
        for (std::size_t i = 0; i < n; ++i) std::free(live[i]);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n) * 2);
}
BENCHMARK(BM_System_FixedChurn)->Arg(1024);

// --- Variable-size churn (TLSF path) --------------------------------------
void BM_Custom_VarChurn(benchmark::State& state) {
    Allocator a(bench_cfg());
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<void*> live(n, nullptr);
    hft::Rng rng(1234);
    std::vector<std::size_t> sizes(n);
    for (auto& s : sizes) s = rng.range(256, 16384);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) live[i] = a.allocate(sizes[i]);
        for (std::size_t i = 0; i < n; ++i) a.deallocate(live[i]);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n) * 2);
}
BENCHMARK(BM_Custom_VarChurn)->Arg(1024);

void BM_System_VarChurn(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<void*> live(n, nullptr);
    hft::Rng rng(1234);
    std::vector<std::size_t> sizes(n);
    for (auto& s : sizes) s = rng.range(256, 16384);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) live[i] = std::malloc(sizes[i]);
        for (std::size_t i = 0; i < n; ++i) std::free(live[i]);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n) * 2);
}
BENCHMARK(BM_System_VarChurn)->Arg(1024);

// --- Full HFT mixed trace -------------------------------------------------
const std::vector<hft::Event>& shared_trace() {
    static const std::vector<hft::Event> t = hft::make_trace(200000, 4096);
    return t;
}

void BM_Custom_HftTrace(benchmark::State& state) {
    Allocator a(bench_cfg());
    const auto& trace = shared_trace();
    std::vector<void*> h(4096, nullptr);
    for (auto _ : state) {
        for (const auto& ev : trace) {
            if (ev.op == hft::Op::Alloc) {
                h[ev.slot] = a.allocate(ev.size);
                benchmark::DoNotOptimize(h[ev.slot]);
            } else {
                a.deallocate(h[ev.slot]);
                h[ev.slot] = nullptr;
            }
        }
        for (auto& p : h) { if (p) { a.deallocate(p); p = nullptr; } }
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(trace.size()));
}
BENCHMARK(BM_Custom_HftTrace);

void BM_System_HftTrace(benchmark::State& state) {
    const auto& trace = shared_trace();
    std::vector<void*> h(4096, nullptr);
    for (auto _ : state) {
        for (const auto& ev : trace) {
            if (ev.op == hft::Op::Alloc) {
                h[ev.slot] = std::malloc(ev.size);
                benchmark::DoNotOptimize(h[ev.slot]);
            } else {
                std::free(h[ev.slot]);
                h[ev.slot] = nullptr;
            }
        }
        for (auto& p : h) { if (p) { std::free(p); p = nullptr; } }
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(trace.size()));
}
BENCHMARK(BM_System_HftTrace);

}  // namespace

BENCHMARK_MAIN();
