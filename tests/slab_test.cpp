#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "memalloc/align.hpp"
#include "memalloc/arena.hpp"
#include "memalloc/slab.hpp"

namespace memalloc {
namespace {

constexpr std::size_t kMiB = 1024 * 1024;

// Representative fixed-size trading structs (Phase 2 deliverable).
struct Order {
    std::uint64_t id;
    std::uint64_t instrument;
    std::int64_t price;
    std::uint32_t quantity;
    std::uint8_t side;
};

struct TradeExecution {
    std::uint64_t order_id;
    std::uint64_t exec_id;
    std::int64_t fill_price;
    std::uint32_t fill_qty;
};

// ---- SlabPool (raw, size-parameterized) ----------------------------------

TEST(SlabPool, SlotSizeRoundedForPointerAndAlignment) {
    Arena arena(kMiB);
    SlabPool pool(arena, /*slot_size=*/1, /*slot_alignment=*/1);
    // Must hold a pointer and be a multiple of the (bumped) alignment.
    EXPECT_GE(pool.slot_size(), sizeof(void*));
    EXPECT_GE(pool.slot_alignment(), alignof(void*));
    EXPECT_EQ(pool.slot_size() % pool.slot_alignment(), 0u);
}

TEST(SlabPool, AllocateReturnsAlignedDistinctSlots) {
    Arena arena(kMiB);
    SlabPool pool(arena, 64, 64, /*slots_per_block=*/8);
    std::set<void*> seen;
    for (int i = 0; i < 8; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(is_aligned_ptr(p, 64));
        EXPECT_TRUE(arena.owns(p));
        EXPECT_TRUE(seen.insert(p).second) << "duplicate slot handed out";
    }
}

TEST(SlabPool, RoundTripAndCounters) {
    Arena arena(kMiB);
    SlabPool pool(arena, 32, 8, 4);
    EXPECT_EQ(pool.in_use(), 0u);

    void* a = pool.allocate();
    void* b = pool.allocate();
    EXPECT_EQ(pool.in_use(), 2u);

    pool.deallocate(a);
    EXPECT_EQ(pool.in_use(), 1u);
    pool.deallocate(b);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.free_count(), pool.capacity());
}

TEST(SlabPool, RecycleReusesFreedSlot) {
    Arena arena(kMiB);
    SlabPool pool(arena, 48, 16, 4);
    void* a = pool.allocate();
    pool.deallocate(a);
    void* b = pool.allocate();
    EXPECT_EQ(a, b) << "freed slot should be handed back immediately (LIFO)";
}

TEST(SlabPool, GrowthHappensOnlyWhenExhausted) {
    Arena arena(kMiB);
    SlabPool pool(arena, 32, 8, /*slots_per_block=*/4);
    EXPECT_EQ(pool.block_count(), 0u);  // lazy: nothing allocated yet

    std::vector<void*> live;
    for (int i = 0; i < 4; ++i) live.push_back(pool.allocate());
    EXPECT_EQ(pool.block_count(), 1u);
    EXPECT_EQ(pool.capacity(), 4u);

    live.push_back(pool.allocate());     // forces a second block
    EXPECT_EQ(pool.block_count(), 2u);
    EXPECT_EQ(pool.capacity(), 8u);

    for (void* p : live) pool.deallocate(p);
}

TEST(SlabPool, RecyclingCausesZeroArenaGrowth) {
    Arena arena(kMiB);
    SlabPool pool(arena, 64, 64, /*slots_per_block=*/16);

    // Prime: allocate and free a batch so the free list is populated.
    std::vector<void*> live;
    for (int i = 0; i < 16; ++i) live.push_back(pool.allocate());
    for (void* p : live) pool.deallocate(p);

    const std::size_t arena_used = arena.used();
    const std::size_t blocks = pool.block_count();

    // Churn many times -- everything should come from the free list.
    for (int round = 0; round < 1000; ++round) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        pool.deallocate(p);
    }

    EXPECT_EQ(arena.used(), arena_used) << "recycling must not touch the Arena";
    EXPECT_EQ(pool.block_count(), blocks);
}

TEST(SlabPool, ExhaustionReturnsNull) {
    Arena small(4096);  // one page
    SlabPool pool(small, 256, 64, /*slots_per_block=*/8);
    int allocated = 0;
    while (pool.allocate() != nullptr) {
        ++allocated;
        ASSERT_LT(allocated, 100000) << "should exhaust the tiny arena";
    }
    EXPECT_GT(allocated, 0);
    EXPECT_EQ(pool.allocate(), nullptr);  // stays null when exhausted
}

TEST(SlabPool, DeallocateNullIsNoOp) {
    Arena arena(kMiB);
    SlabPool pool(arena, 32, 8);
    pool.deallocate(nullptr);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.free_count(), 0u);
}

// ---- ObjectPool<T> (typed wrapper) ---------------------------------------

TEST(ObjectPool, CreateConstructsAndDestroyReleases) {
    Arena arena(kMiB);
    ObjectPool<Order> pool(arena, 32);

    Order* o = pool.create();
    ASSERT_NE(o, nullptr);
    o->id = 42;
    o->price = -100;
    EXPECT_EQ(pool.in_use(), 1u);
    EXPECT_TRUE(is_aligned_ptr(o, alignof(Order)));

    pool.destroy(o);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ObjectPool, ConstructorArgsForwarded) {
    struct Widget {
        int a;
        double b;
        Widget(int x, double y) : a(x), b(y) {}
    };
    Arena arena(kMiB);
    ObjectPool<Widget> pool(arena);
    Widget* w = pool.create(7, 1.5);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->a, 7);
    EXPECT_EQ(w->b, 1.5);
    pool.destroy(w);
}

TEST(ObjectPool, DestructorRunsExactlyOnce) {
    static int live = 0;
    struct Counted {
        Counted() { ++live; }
        ~Counted() { --live; }
    };
    Arena arena(kMiB);
    ObjectPool<Counted> pool(arena, 8);

    std::vector<Counted*> v;
    for (int i = 0; i < 20; ++i) v.push_back(pool.create());
    EXPECT_EQ(live, 20);
    for (Counted* c : v) pool.destroy(c);
    EXPECT_EQ(live, 0);
}

TEST(ObjectPool, TradeExecutionPoolRoundTrips) {
    Arena arena(kMiB);
    ObjectPool<TradeExecution> pool(arena, 16);
    TradeExecution* t = pool.create();
    ASSERT_NE(t, nullptr);
    t->order_id = 1;
    t->exec_id = 2;
    pool.destroy(t);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ObjectPool, DestroyNullIsNoOp) {
    Arena arena(kMiB);
    ObjectPool<Order> pool(arena);
    pool.destroy(nullptr);
    SUCCEED();
}

}  // namespace
}  // namespace memalloc
