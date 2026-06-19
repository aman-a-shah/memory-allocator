#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Synthetic high-frequency-trading workload generator (Phase 7).
//
// Real trading engines churn through a small set of repetitive, fixed-shape
// records (orders, executions, tick updates) plus variable-length ingestion
// (market-data packets, symbol lists). The traffic profile is bursty: a flood
// of order creations, then rapid cancel/execute events that free those records
// in a roughly FIFO-with-jitter order, while a steady working set stays live.
//
// This header is allocator-agnostic: it produces a deterministic *sequence of
// events* (ALLOC size / FREE handle) from a fixed seed, so every allocator
// under test replays the identical trace.

namespace memalloc {
namespace hft {

// Repetitive fixed-size trading records (the slab path's reason to exist).
struct Order {
    std::uint64_t id;
    std::int64_t price;        // fixed-point ticks
    std::uint32_t quantity;
    std::uint32_t flags;
    std::uint64_t timestamp_ns;
    char symbol[8];
};

struct TradeExecution {
    std::uint64_t exec_id;
    std::uint64_t order_id;
    std::int64_t fill_price;
    std::uint32_t fill_qty;
    std::uint32_t venue;
};

// A deterministic xorshift64* PRNG so traces are reproducible across runs and
// across allocators (no std::mt19937 cost in the hot loop).
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() noexcept {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1Dull;
    }
    // Uniform in [lo, hi].
    std::size_t range(std::size_t lo, std::size_t hi) noexcept {
        return lo + static_cast<std::size_t>(next() % (hi - lo + 1));
    }
    // True with probability pct/100.
    bool chance(unsigned pct) noexcept { return (next() % 100u) < pct; }

private:
    std::uint64_t s_;
};

enum class Op : std::uint8_t { Alloc, Free };

// One trace step. For Alloc, `size` is the request; the i-th Alloc fills handle
// slot `slot`. For Free, `slot` is the handle to release.
struct Event {
    Op op;
    std::uint32_t slot;     // index into the runner's live-pointer table
    std::uint32_t size;     // request bytes (Alloc only)
};

// Generates a bursty HFT trace of approximately `target_events` events with a
// bounded number of simultaneously-live handles (`max_live`). The size mix is
// ~80% small fixed records (Order/TradeExecution-shaped, slab path) and ~20%
// variable-length ingestion buffers (TLSF path), matching the dual-engine
// rationale.
inline std::vector<Event> make_trace(std::size_t target_events,
                                     std::size_t max_live,
                                     std::uint64_t seed = 0xC0FFEEu) {
    Rng rng(seed);
    std::vector<Event> trace;
    trace.reserve(target_events + max_live);

    std::vector<std::uint32_t> free_slots;   // recyclable handle indices
    std::vector<std::uint32_t> live_slots;   // currently-allocated handles
    free_slots.reserve(max_live);
    live_slots.reserve(max_live);
    for (std::uint32_t i = 0; i < max_live; ++i) {
        free_slots.push_back(max_live - 1 - i);  // hand out 0,1,2,... first
    }

    auto pick_size = [&rng]() -> std::uint32_t {
        if (rng.chance(80)) {
            // Fixed-shape records: cluster around the slab size classes.
            static const std::uint32_t small[] = {
                sizeof(Order), sizeof(TradeExecution), 16, 32, 48, 64, 96, 128};
            return small[rng.next() % (sizeof(small) / sizeof(small[0]))];
        }
        // Variable ingestion buffers: 200 B .. 16 KiB.
        return static_cast<std::uint32_t>(rng.range(200, 16384));
    };

    while (trace.size() < target_events) {
        // Bursty creation: when capacity allows, emit a burst of allocations.
        const bool can_alloc = !free_slots.empty();
        const bool can_free = !live_slots.empty();

        // Bias toward allocation while the book is filling, toward free once it
        // is near capacity -- producing the burst-then-drain shape.
        const std::size_t fill = live_slots.size();
        unsigned alloc_pct = fill < max_live / 2 ? 75u : 40u;

        if (can_alloc && (!can_free || rng.chance(alloc_pct))) {
            std::uint32_t slot = free_slots.back();
            free_slots.pop_back();
            live_slots.push_back(slot);
            trace.push_back({Op::Alloc, slot, pick_size()});
        } else if (can_free) {
            // Free with FIFO-ish jitter: usually the oldest live handle, but
            // occasionally a random one (cancels arriving out of order).
            std::size_t idx = rng.chance(70) ? 0 : (rng.next() % live_slots.size());
            std::uint32_t slot = live_slots[idx];
            live_slots.erase(live_slots.begin() + static_cast<std::ptrdiff_t>(idx));
            free_slots.push_back(slot);
            trace.push_back({Op::Free, slot, 0});
        }
    }
    return trace;
}

}  // namespace hft
}  // namespace memalloc
