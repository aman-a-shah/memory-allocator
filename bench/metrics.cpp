// Phase 7 metrics generator.
//
// Replays a synthetic HFT trace through the custom allocator and the system
// (libc) allocator and measures the four PRD targets from real runs:
//   * throughput vs the system allocator (slab, TLSF, and mixed workloads),
//   * latency determinism (full per-op distribution + tail percentiles),
//   * O(1) behaviour (latency vs request size),
//   * fragmentation under the workload,
// plus multi-thread scalability and 10k-cycle stability.
//
// It writes a single JSON document (to argv[1], or stdout) that the PDF's
// export_data.py consumes. NOTHING here is mocked: every number is timed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "hft_workload.hpp"
#include "memalloc/allocator.hpp"
#include "memalloc/concurrent.hpp"

namespace {

using memalloc::Allocator;
using memalloc::ConcurrentAllocator;
namespace hft = memalloc::hft;
using Clock = std::chrono::steady_clock;
using hft::Event;
using hft::Op;

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

// Prevent the optimizer from eliding allocations whose result is unused.
inline void escape(void* p) { asm volatile("" : : "g"(p) : "memory"); }

// Measured floor of the timing harness itself (one clock read pair), subtracted
// from per-op latency samples so we report allocator cost, not timer cost.
double measure_timer_overhead_ns() {
    constexpr int kN = 200000;
    std::vector<double> s;
    s.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        std::uint64_t a = now_ns();
        escape(&a);
        std::uint64_t b = now_ns();
        s.push_back(static_cast<double>(b - a));
    }
    std::sort(s.begin(), s.end());
    return s[s.size() / 2];  // median single-read overhead
}

double percentile(std::vector<double>& v, double q) {
    if (v.empty()) return 0.0;
    const std::size_t idx = static_cast<std::size_t>(
        std::llround(q * static_cast<double>(v.size() - 1)));
    return v[idx];
}

// ---- A uniform allocator interface so both engines replay the same trace ----
struct CustomEngine {
    Allocator a;
    explicit CustomEngine(const Allocator::Config& c) : a(c) {}
    void* alloc(std::size_t n) { return a.allocate(n); }
    void free(void* p) { a.deallocate(p); }
};
struct SystemEngine {
    void* alloc(std::size_t n) { return std::malloc(n); }
    void free(void* p) { std::free(p); }
};

Allocator::Config big_cfg() {
    Allocator::Config c;
    c.arena_bytes = 512u * 1024 * 1024;   // generous single-thread reservation
    c.tlsf_region_bytes = 4u * 1024 * 1024;
    return c;
}

// Replay a trace, returning operations per second (each alloc and free is one op).
template <class Engine>
double replay_ops_per_sec(Engine& e, const std::vector<Event>& trace,
                          std::size_t max_live) {
    std::vector<void*> h(max_live, nullptr);
    const std::uint64_t t0 = now_ns();
    for (const Event& ev : trace) {
        if (ev.op == Op::Alloc) {
            void* p = e.alloc(ev.size);
            escape(p);
            h[ev.slot] = p;
        } else {
            e.free(h[ev.slot]);
            h[ev.slot] = nullptr;
        }
    }
    const std::uint64_t t1 = now_ns();
    for (void* p : h) {
        if (p) e.free(p);
    }
    const double secs = static_cast<double>(t1 - t0) / 1e9;
    return static_cast<double>(trace.size()) / secs;
}

// Replay a trace, sampling the latency (ns) of every individual ALLOC call.
template <class Engine>
void replay_collect_latency(Engine& e, const std::vector<Event>& trace,
                            std::size_t max_live, double overhead_ns,
                            std::vector<double>& out) {
    std::vector<void*> h(max_live, nullptr);
    out.clear();
    out.reserve(trace.size());
    // Warm-up pass (untimed): fault in the pages the engine will use so the
    // timed pass measures allocator jitter, not first-touch page faults.
    for (const Event& ev : trace) {
        if (ev.op == Op::Alloc) {
            h[ev.slot] = e.alloc(ev.size);
            escape(h[ev.slot]);
        } else if (h[ev.slot]) {
            e.free(h[ev.slot]);
            h[ev.slot] = nullptr;
        }
    }
    for (void* p : h) {
        if (p) e.free(p);
    }
    std::fill(h.begin(), h.end(), nullptr);
    for (const Event& ev : trace) {
        if (ev.op == Op::Alloc) {
            const std::uint64_t a = now_ns();
            void* p = e.alloc(ev.size);
            const std::uint64_t b = now_ns();
            escape(p);
            h[ev.slot] = p;
            double ns = static_cast<double>(b - a) - overhead_ns;
            if (ns < 0) ns = 0;
            out.push_back(ns);
        } else {
            e.free(h[ev.slot]);
            h[ev.slot] = nullptr;
        }
    }
    for (void* p : h) {
        if (p) e.free(p);
    }
}

// Mean alloc+free latency for a fixed request size under a steady churn.
// Batch-timed (total / iterations): individual ops are far below the host
// timer's ~41 ns granularity, so per-op sampling cannot resolve them -- the
// batch mean is the honest, resolvable figure for the O(1) curve.
template <class Engine>
double size_latency_ns(Engine& e, std::size_t size) {
    constexpr std::size_t kLive = 256;
    constexpr int kIters = 400000;
    std::vector<void*> ring(kLive, nullptr);
    // Warm the working set so the timed loop reflects steady-state allocator
    // cost, not one-time OS page faults.
    for (std::size_t i = 0; i < kLive; ++i) {
        ring[i] = e.alloc(size);
        escape(ring[i]);
    }
    const std::uint64_t t0 = now_ns();
    for (int i = 0; i < kIters; ++i) {
        const std::size_t slot = static_cast<std::size_t>(i) % kLive;
        e.free(ring[slot]);
        void* p = e.alloc(size);
        escape(p);
        ring[slot] = p;
    }
    const std::uint64_t t1 = now_ns();
    for (void* p : ring) {
        if (p) e.free(p);
    }
    // Each iteration is one free + one alloc.
    return static_cast<double>(t1 - t0) / static_cast<double>(kIters);
}

// ---- Tiny JSON builder ----------------------------------------------------
struct Json {
    std::string s;
    void raw(const std::string& x) { s += x; }
    static std::string num(double v) {
        char buf[64];
        if (v == std::floor(v) && std::fabs(v) < 1e15)
            std::snprintf(buf, sizeof(buf), "%.0f", v);
        else
            std::snprintf(buf, sizeof(buf), "%.6g", v);
        return buf;
    }
    static std::string arr(const std::vector<double>& v) {
        std::string out = "[";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ",";
            out += num(v[i]);
        }
        return out + "]";
    }
    static std::string sarr(const std::vector<std::string>& v) {
        std::string out = "[";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ",";
            out += "\"" + v[i] + "\"";
        }
        return out + "]";
    }
};

std::vector<double> pct_tuple(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return {percentile(v, 0.50), percentile(v, 0.90), percentile(v, 0.99),
            percentile(v, 0.999), percentile(v, 1.0)};
}

}  // namespace

int main(int argc, char** argv) {
    const double overhead = measure_timer_overhead_ns();
    std::fprintf(stderr, "[metrics] timer overhead ~ %.1f ns\n", overhead);

    // Build one HFT trace; derive a slab-only and a TLSF-only variant that share
    // its alloc/free *structure* but force the size into one engine.
    const std::size_t kMaxLive = 4096;
    const std::size_t kEvents = 1200000;
    std::vector<Event> mixed = hft::make_trace(kEvents, kMaxLive);

    std::vector<Event> slabt = mixed;
    for (Event& e : slabt)
        if (e.op == Op::Alloc) e.size = 64;  // fixed record -> slab path

    std::vector<Event> tlsft = mixed;
    {
        hft::Rng r(7);
        for (Event& e : tlsft)
            if (e.op == Op::Alloc)
                e.size = static_cast<std::uint32_t>(r.range(512, 16384));
    }

    Json j;
    j.raw("{\n");

    // ---- 1. Throughput by scenario ---------------------------------------
    std::fprintf(stderr, "[metrics] throughput...\n");
    std::vector<std::string> scen = {"Slab (fixed 64B)", "TLSF (variable)",
                                     "HFT mixed"};
    std::vector<double> cust_mops, sys_mops, speedup;
    const std::vector<const std::vector<Event>*> traces = {&slabt, &tlsft, &mixed};
    constexpr int kTrials = 7;  // best-of filters OS scheduling/contention noise
    for (const auto* tr : traces) {
        double c = 0, m = 0;
        for (int t = 0; t < kTrials; ++t) {
            CustomEngine ce(big_cfg());
            c = std::max(c, replay_ops_per_sec(ce, *tr, kMaxLive) / 1e6);
        }
        for (int t = 0; t < kTrials; ++t) {
            SystemEngine se;
            m = std::max(m, replay_ops_per_sec(se, *tr, kMaxLive) / 1e6);
        }
        cust_mops.push_back(c);
        sys_mops.push_back(m);
        speedup.push_back(c / m);
    }

    // ---- 2. Latency distribution + percentiles (HFT mixed) ----------------
    std::fprintf(stderr, "[metrics] latency...\n");
    std::vector<double> lat_c, lat_s;
    {
        CustomEngine ce(big_cfg());
        replay_collect_latency(ce, mixed, kMaxLive, overhead, lat_c);
    }
    {
        SystemEngine se;
        replay_collect_latency(se, mixed, kMaxLive, overhead, lat_s);
    }
    std::vector<double> pc = pct_tuple(lat_c);
    std::vector<double> ps = pct_tuple(lat_s);

    // Log-spaced histogram over both series (for the determinism figure).
    auto clean = [](std::vector<double> v) {
        std::vector<double> o;
        o.reserve(v.size());
        for (double x : v)
            if (x > 0) o.push_back(x);
        return o;
    };
    std::vector<double> ac = clean(lat_c), as = clean(lat_s);
    double lo = 1e9, hi = 0;
    for (double x : ac) { lo = std::min(lo, x); hi = std::max(hi, x); }
    for (double x : as) { lo = std::min(lo, x); hi = std::max(hi, x); }
    if (lo < 1) lo = 1;
    const int kBins = 40;
    const double llo = std::log10(lo), lhi = std::log10(hi);
    std::vector<double> edges, hc(kBins, 0), hs(kBins, 0);
    for (int i = 0; i <= kBins; ++i)
        edges.push_back(std::pow(10.0, llo + (lhi - llo) * i / kBins));
    auto bin = [&](const std::vector<double>& v, std::vector<double>& h) {
        for (double x : v) {
            int b = static_cast<int>((std::log10(x) - llo) / (lhi - llo) * kBins);
            if (b < 0) b = 0;
            if (b >= kBins) b = kBins - 1;
            h[static_cast<std::size_t>(b)] += 1;
        }
    };
    bin(ac, hc);
    bin(as, hs);

    // ---- 3. O(1): latency vs request size --------------------------------
    std::fprintf(stderr, "[metrics] by-size...\n");
    std::vector<double> sizes = {16,   32,   64,   128,  256,
                                 512,  1024, 4096, 16384, 65536};
    std::vector<double> bysz_c, bysz_s;
    for (double sz : sizes) {
        CustomEngine ce(big_cfg());
        bysz_c.push_back(size_latency_ns(ce, static_cast<std::size_t>(sz)));
        SystemEngine se;
        bysz_s.push_back(size_latency_ns(se, static_cast<std::size_t>(sz)));
    }

    // ---- 4. Scalability: throughput vs thread count ----------------------
    std::fprintf(stderr, "[metrics] scalability...\n");
    std::vector<double> threads = {1, 2, 4, 8};
    std::vector<double> scale_c, scale_s;
    auto run_threads = [&](int nt, bool custom, ConcurrentAllocator* ca) -> double {
        std::vector<std::thread> ts;
        const std::uint64_t t0 = now_ns();
        for (int t = 0; t < nt; ++t) {
            ts.emplace_back([&] {
                std::vector<void*> h(kMaxLive, nullptr);
                for (const Event& ev : mixed) {
                    if (ev.op == Op::Alloc) {
                        void* p = custom ? ca->allocate(ev.size) : std::malloc(ev.size);
                        escape(p);
                        h[ev.slot] = p;
                    } else if (h[ev.slot]) {
                        if (custom) ca->deallocate(h[ev.slot]); else std::free(h[ev.slot]);
                        h[ev.slot] = nullptr;
                    }
                }
                for (void* p : h) if (p) { if (custom) ca->deallocate(p); else std::free(p); }
            });
        }
        for (auto& th : ts) th.join();
        const double secs = static_cast<double>(now_ns() - t0) / 1e9;
        return static_cast<double>(mixed.size()) * static_cast<double>(nt) / secs / 1e6;
    };
    for (double tdf : threads) {
        const int nt = static_cast<int>(tdf);
        double c = 0, m = 0;
        for (int rep = 0; rep < 3; ++rep) {
            ConcurrentAllocator::Config cc;
            cc.superblock_bytes = 48u * 1024 * 1024;
            cc.global_arena_bytes = static_cast<std::size_t>(nt + 1) * cc.superblock_bytes;
            ConcurrentAllocator ca(cc);
            c = std::max(c, run_threads(nt, true, &ca));
            m = std::max(m, run_threads(nt, false, nullptr));
        }
        scale_c.push_back(c);
        scale_s.push_back(m);
    }

    // ---- 5. Fragmentation over the workload (TLSF variable-size path) -----
    std::fprintf(stderr, "[metrics] fragmentation...\n");
    std::vector<double> frag_x, frag_y;
    double frag_final = 0;
    {
        CustomEngine ce(big_cfg());
        std::vector<void*> h(kMaxLive, nullptr);
        std::vector<std::size_t> req(kMaxLive, 0);
        std::size_t live_req = 0, live_n = 0;
        std::size_t step = 0;
        for (const Event& ev : tlsft) {
            if (ev.op == Op::Alloc) {
                void* p = ce.alloc(ev.size);
                escape(p);
                if (p) {
                    h[ev.slot] = p;
                    req[ev.slot] = ev.size;
                    live_req += ev.size;
                    ++live_n;
                }
            } else if (h[ev.slot]) {
                ce.free(h[ev.slot]);
                live_req -= req[ev.slot];
                --live_n;
                h[ev.slot] = nullptr;
            }
            if (++step % 20000 == 0 && live_n > 0) {
                // Engine footprint backing the live set: allocated payloads
                // (size rounded to 8B + split remainders) + 16B header/block.
                const double footprint =
                    static_cast<double>(ce.a.tlsf_used_bytes()) +
                    16.0 * static_cast<double>(live_n);
                const double frag =
                    100.0 * (footprint - static_cast<double>(live_req)) / footprint;
                frag_x.push_back(static_cast<double>(step) / 1000.0);
                frag_y.push_back(frag);
                frag_final = frag;
            }
        }
        for (void* p : h) if (p) ce.free(p);
    }

    // ---- 6. Stability across 10k+ cycles ---------------------------------
    std::fprintf(stderr, "[metrics] stability...\n");
    std::vector<double> cyc_x, cyc_y, cyc_mb;
    {
        CustomEngine ce(big_cfg());
        constexpr int kCycles = 12000;
        constexpr int kPer = 256;  // objects created+freed per cycle
        std::vector<void*> h(kPer, nullptr);
        hft::Rng r(99);
        const int kWindow = 1000;
        std::uint64_t win_t0 = now_ns();
        for (int c = 0; c < kCycles; ++c) {
            for (int i = 0; i < kPer; ++i) {
                std::size_t sz = r.chance(80) ? r.range(16, 128) : r.range(256, 8192);
                void* p = ce.alloc(sz);
                escape(p);
                h[static_cast<std::size_t>(i)] = p;
            }
            for (int i = 0; i < kPer; ++i) {
                ce.free(h[static_cast<std::size_t>(i)]);
                h[static_cast<std::size_t>(i)] = nullptr;
            }
            if ((c + 1) % kWindow == 0) {
                std::uint64_t t = now_ns();
                double secs = static_cast<double>(t - win_t0) / 1e9;
                double ops = static_cast<double>(kWindow) * kPer * 2.0;
                cyc_x.push_back(static_cast<double>(c + 1));
                cyc_y.push_back(ops / secs / 1e6);
                // Committed footprint: must plateau (no leak) once warmed.
                cyc_mb.push_back(static_cast<double>(ce.a.arena_used_bytes()) /
                                 (1024.0 * 1024.0));
                win_t0 = now_ns();
            }
        }
    }

    // ---- Assemble JSON ----------------------------------------------------
    auto kv = [&](const std::string& k, const std::string& v, bool comma = true) {
        j.raw("  \"" + k + "\": " + v + (comma ? ",\n" : "\n"));
    };

    kv("headline",
       std::string("{") + "\"speedup_mixed\":" + Json::num(speedup[2]) +
           ",\"speedup_slab\":" + Json::num(speedup[0]) +
           ",\"speedup_tlsf\":" + Json::num(speedup[1]) +
           ",\"throughput_custom_mops\":" + Json::num(cust_mops[2]) +
           ",\"throughput_sys_mops\":" + Json::num(sys_mops[2]) +
           ",\"p999_custom_ns\":" + Json::num(pc[3]) +
           ",\"p999_sys_ns\":" + Json::num(ps[3]) +
           ",\"tail_ratio\":" + Json::num(ps[3] / (pc[3] > 0 ? pc[3] : 1)) +
           ",\"fragmentation_pct\":" + Json::num(frag_final) +
           ",\"events\":" + Json::num(static_cast<double>(mixed.size())) + "}");

    kv("throughput",
       std::string("{") + "\"scenarios\":" + Json::sarr(scen) +
           ",\"custom_mops\":" + Json::arr(cust_mops) +
           ",\"sys_mops\":" + Json::arr(sys_mops) +
           ",\"speedup\":" + Json::arr(speedup) + "}");

    kv("latency_pct",
       std::string("{") +
           "\"labels\":" +
           Json::sarr({"p50", "p90", "p99", "p99.9", "max"}) +
           ",\"custom_ns\":" + Json::arr(pc) +
           ",\"sys_ns\":" + Json::arr(ps) + "}");

    kv("latency_hist",
       std::string("{") + "\"edges_ns\":" + Json::arr(edges) +
           ",\"custom\":" + Json::arr(hc) + ",\"sys\":" + Json::arr(hs) + "}");

    kv("by_size",
       std::string("{") + "\"sizes\":" + Json::arr(sizes) +
           ",\"custom_ns\":" + Json::arr(bysz_c) +
           ",\"sys_ns\":" + Json::arr(bysz_s) + "}");

    kv("scalability",
       std::string("{") + "\"threads\":" + Json::arr(threads) +
           ",\"custom_mops\":" + Json::arr(scale_c) +
           ",\"sys_mops\":" + Json::arr(scale_s) + "}");

    kv("fragmentation",
       std::string("{") + "\"cycle_k\":" + Json::arr(frag_x) +
           ",\"frag_pct\":" + Json::arr(frag_y) +
           ",\"final_pct\":" + Json::num(frag_final) + "}");

    kv("stability",
       std::string("{") + "\"cycle\":" + Json::arr(cyc_x) +
           ",\"mops\":" + Json::arr(cyc_y) +
           ",\"footprint_mb\":" + Json::arr(cyc_mb) + "}",
       false);

    j.raw("}\n");

    if (argc > 1) {
        FILE* f = std::fopen(argv[1], "w");
        if (!f) {
            std::perror("fopen");
            return 1;
        }
        std::fwrite(j.s.data(), 1, j.s.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "[metrics] wrote %s (%zu bytes)\n", argv[1], j.s.size());
    } else {
        std::fwrite(j.s.data(), 1, j.s.size(), stdout);
    }
    return 0;
}
