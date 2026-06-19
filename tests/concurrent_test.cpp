#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "memalloc/concurrent.hpp"

namespace memalloc {
namespace {

// A modest global arena keeps the tests fast while still exercising several
// per-thread superblocks.
ConcurrentAllocator::Config small_config(std::size_t threads) {
    ConcurrentAllocator::Config cfg;
    cfg.superblock_bytes = 4u * 1024 * 1024;            // 4 MiB / thread
    cfg.global_arena_bytes = (threads + 2) * cfg.superblock_bytes;
    return cfg;
}

// ---- Single-thread sanity ------------------------------------------------

TEST(Concurrent, SingleThreadRoundTrip) {
    ConcurrentAllocator a(small_config(1));
    void* p = a.allocate(64);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 64);
    a.deallocate(p);
    EXPECT_EQ(a.thread_count(), 1u);
}

TEST(Concurrent, MixedSizesDistinctAndWritable) {
    ConcurrentAllocator a(small_config(1));
    std::vector<void*> ps;
    const std::size_t sizes[] = {8, 24, 64, 200, 1024, 9000};
    for (std::size_t s : sizes) {
        void* p = a.allocate(s);
        ASSERT_NE(p, nullptr) << "size " << s;
        std::memset(p, 0x7E, s);  // full write must stay in-bounds
        ps.push_back(p);
    }
    // All pointers distinct.
    for (std::size_t i = 0; i < ps.size(); ++i) {
        for (std::size_t j = i + 1; j < ps.size(); ++j) {
            EXPECT_NE(ps[i], ps[j]);
        }
    }
    for (void* p : ps) {
        a.deallocate(p);
    }
}

// ---- Each thread allocates and frees its own (no sharing) ----------------

TEST(Concurrent, PerThreadAllocFreeNoSharing) {
    constexpr int kThreads = 8;
    constexpr int kIters = 20000;
    ConcurrentAllocator a(small_config(kThreads));
    std::atomic<int> failures{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            std::vector<void*> live;
            live.reserve(64);
            std::uint64_t rng = 0x9E3779B97F4A7C15ull * (t + 1);
            for (int i = 0; i < kIters; ++i) {
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                const std::size_t sz = 8 + (rng % 2048);
                void* p = a.allocate(sz);
                if (p == nullptr) {
                    failures.fetch_add(1);
                    continue;
                }
                // Touch the whole allocation; a bad size/overlap trips ASan.
                std::memset(p, static_cast<int>(i & 0xFF), sz);
                live.push_back(p);
                if (live.size() > 32) {
                    a.deallocate(live.front());
                    live.erase(live.begin());
                }
            }
            for (void* p : live) {
                a.deallocate(p);
            }
        });
    }
    for (auto& th : ts) {
        th.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(a.thread_count(), static_cast<std::size_t>(kThreads));
}

// ---- Cross-thread free: producer threads alloc, consumer frees -----------
//
// Producers hand pointers to a single consumer through a lock-free-ish handoff;
// the consumer (a different thread than the owner) frees them, exercising the
// remote-free path. TSan must report no data races.

TEST(Concurrent, CrossThreadFree) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 15000;
    ConcurrentAllocator a(small_config(kProducers + 1));

    struct Slot {
        std::atomic<void*> ptr{nullptr};
    };
    constexpr int kRing = 1024;
    std::vector<Slot> ring(kRing);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> done{false};

    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] {
            std::uint64_t rng = 0xD1B54A32D192ED03ull * (t + 1);
            for (int i = 0; i < kPerProducer; ++i) {
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                const std::size_t sz = 8 + (rng % 1000);
                void* p = a.allocate(sz);
                if (p == nullptr) {
                    continue;
                }
                std::memset(p, 0x5A, sz);
                // Publish into the ring (spin until a slot is free).
                int idx = (produced.fetch_add(1)) % kRing;
                void* expected = nullptr;
                while (!ring[idx].ptr.compare_exchange_weak(expected, p)) {
                    expected = nullptr;
                    std::this_thread::yield();
                }
            }
        });
    }

    // Single consumer frees everything from a thread that did not allocate it.
    std::thread consumer([&] {
        int idx = 0;
        while (!done.load() || consumed.load() < produced.load()) {
            void* p = ring[idx % kRing].ptr.exchange(nullptr);
            if (p != nullptr) {
                a.deallocate(p);
                consumed.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
            ++idx;
        }
    });

    for (auto& th : producers) {
        th.join();
    }
    done.store(true);
    consumer.join();

    EXPECT_EQ(consumed.load(), produced.load());
}

// ---- Churn: prove remote frees are actually reclaimed --------------------
//
// One owner thread keeps allocating; helper threads free its blocks. If remote
// frees were never recycled the owner's superblock would exhaust long before
// the iteration count. Reaching the end with no nullptrs proves reclamation.

TEST(Concurrent, RemoteFreesAreReclaimed) {
    ConcurrentAllocator a(small_config(2));  // owner + helper share global
    constexpr int kRounds = 50;
    constexpr int kBatch = 4096;
    constexpr std::size_t kSize = 256;  // 4MiB / 256 = 16k slots; batch*rounds >> that

    std::atomic<int> failures{0};
    for (int r = 0; r < kRounds; ++r) {
        std::vector<void*> batch;
        batch.reserve(kBatch);
        for (int i = 0; i < kBatch; ++i) {
            void* p = a.allocate(kSize);
            if (p == nullptr) {
                failures.fetch_add(1);
            } else {
                batch.push_back(p);
            }
        }
        // Free them all from a *different* thread (remote-free path), then the
        // next round's allocations on this thread must reclaim them via drain.
        std::thread helper([&] {
            for (void* p : batch) {
                a.deallocate(p);
            }
        });
        helper.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

}  // namespace
}  // namespace memalloc
