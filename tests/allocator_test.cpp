#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "memalloc/align.hpp"
#include "memalloc/allocator.hpp"

namespace memalloc {
namespace {

struct Order {
    std::uint64_t id;
    std::int64_t price;
    std::uint32_t qty;
};

// ---- Routing -------------------------------------------------------------

TEST(Allocator, SmallRequestRoutesToSlab) {
    Allocator a;
    ASSERT_TRUE(a.routes_to_slab(64));
    void* p = a.allocate(64);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(a.slab_live(), 1u);
    EXPECT_EQ(a.tlsf_live(), 0u);
    a.deallocate(p);
    EXPECT_EQ(a.slab_live(), 0u);
}

TEST(Allocator, LargeRequestRoutesToTlsf) {
    Allocator a;
    const std::size_t big = a.slab_threshold() + 1;
    ASSERT_FALSE(a.routes_to_slab(big));
    void* p = a.allocate(big);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(a.tlsf_live(), 1u);
    EXPECT_EQ(a.slab_live(), 0u);
    a.deallocate(p);
    EXPECT_EQ(a.tlsf_live(), 0u);
}

TEST(Allocator, BoundaryRoutesByThreshold) {
    Allocator a;
    const std::size_t t = a.slab_threshold();
    void* edge = a.allocate(t);       // exactly the largest class -> slab
    void* over = a.allocate(t + 1);   // one past -> TLSF
    ASSERT_NE(edge, nullptr);
    ASSERT_NE(over, nullptr);
    EXPECT_EQ(a.slab_live(), 1u);
    EXPECT_EQ(a.tlsf_live(), 1u);
    a.deallocate(edge);
    a.deallocate(over);
    EXPECT_EQ(a.slab_live(), 0u);
    EXPECT_EQ(a.tlsf_live(), 0u);
}

// ---- Deallocate routes to the correct engine -----------------------------

TEST(Allocator, DeallocateRoutesByAddress) {
    Allocator a;
    // Allocate from both engines, then free in interleaved order. If routing
    // were wrong, the wrong engine's free() would corrupt and trip ASan.
    std::vector<void*> slab;
    std::vector<void*> tlsf;
    for (int i = 0; i < 50; ++i) {
        slab.push_back(a.allocate(32));
        tlsf.push_back(a.allocate(2000));
    }
    EXPECT_EQ(a.slab_live(), 50u);
    EXPECT_EQ(a.tlsf_live(), 50u);

    for (std::size_t i = 0; i < slab.size(); ++i) {
        a.deallocate(tlsf[i]);
        a.deallocate(slab[i]);
    }
    EXPECT_EQ(a.slab_live(), 0u);
    EXPECT_EQ(a.tlsf_live(), 0u);
}

// ---- Memory is usable, distinct, aligned ---------------------------------

TEST(Allocator, MixedWorkloadDistinctAndWritable) {
    Allocator a;
    struct Span {
        char* lo;
        std::size_t n;
    };
    std::vector<Span> spans;
    for (int i = 0; i < 500; ++i) {
        // Sizes spanning both engines.
        const std::size_t n = (i % 7 == 0) ? (512 + i * 7) : (8 + (i % 200));
        void* p = a.allocate(n);
        ASSERT_NE(p, nullptr) << "i=" << i;
        EXPECT_TRUE(is_aligned_ptr(p, kAlignSize));
        std::memset(p, i & 0xFF, n);
        spans.push_back({static_cast<char*>(p), n});
    }
    for (std::size_t i = 0; i < spans.size(); ++i) {
        for (std::size_t j = i + 1; j < spans.size(); ++j) {
            char* a_hi = spans[i].lo + spans[i].n;
            char* b_hi = spans[j].lo + spans[j].n;
            ASSERT_TRUE(a_hi <= spans[j].lo || b_hi <= spans[i].lo)
                << "overlap " << i << "," << j;
        }
    }
    for (Span s : spans) a.deallocate(s.lo);
    EXPECT_EQ(a.slab_live(), 0u);
    EXPECT_EQ(a.tlsf_live(), 0u);
}

// ---- Typed create/destroy across both engines ----------------------------

TEST(Allocator, CreateDestroySmallObject) {
    Allocator a;
    Order* o = a.create<Order>();
    ASSERT_NE(o, nullptr);
    o->id = 7;
    o->price = -3;
    o->qty = 100;
    EXPECT_EQ(a.slab_live(), 1u);  // sizeof(Order)=24 -> slab
    a.destroy(o);
    EXPECT_EQ(a.slab_live(), 0u);
}

TEST(Allocator, CreateDestroyLargeObject) {
    struct Big {
        char payload[1024];
        int tag;
    };
    Allocator a;
    Big* b = a.create<Big>(Big{});
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a.tlsf_live(), 1u);  // > slab threshold -> TLSF
    b->tag = 42;
    EXPECT_EQ(b->tag, 42);
    a.destroy(b);
    EXPECT_EQ(a.tlsf_live(), 0u);
}

TEST(Allocator, DestructorRunsOnDestroy) {
    static int live = 0;
    struct Counted {
        Counted() { ++live; }
        ~Counted() { --live; }
    };
    Allocator a;
    std::vector<Counted*> v;
    for (int i = 0; i < 30; ++i) v.push_back(a.create<Counted>());
    EXPECT_EQ(live, 30);
    for (Counted* c : v) a.destroy(c);
    EXPECT_EQ(live, 0);
}

// ---- Reuse + churn -------------------------------------------------------

TEST(Allocator, ReuseAcrossEngines) {
    Allocator a;
    void* s1 = a.allocate(48);
    a.deallocate(s1);
    void* s2 = a.allocate(48);
    EXPECT_EQ(s1, s2) << "slab slot should recycle";
    a.deallocate(s2);

    void* t1 = a.allocate(4000);
    a.deallocate(t1);
    void* t2 = a.allocate(4000);
    EXPECT_EQ(t1, t2) << "TLSF block should recycle";
    a.deallocate(t2);
}

TEST(Allocator, ZeroAndNullSafe) {
    Allocator a;
    EXPECT_EQ(a.allocate(0), nullptr);
    a.deallocate(nullptr);  // no-op
    EXPECT_EQ(a.slab_live(), 0u);
    EXPECT_EQ(a.tlsf_live(), 0u);
}

TEST(Allocator, ChurnLeavesNothingLive) {
    Allocator a;
    std::vector<void*> live;
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 400; ++i) {
            live.push_back(a.allocate(16 + (i * 37) % 4096));
            ASSERT_NE(live.back(), nullptr);
        }
        // Free every other pointer, then the rest.
        for (std::size_t i = 0; i < live.size(); i += 2) a.deallocate(live[i]);
        for (std::size_t i = 1; i < live.size(); i += 2) a.deallocate(live[i]);
        live.clear();
    }
    EXPECT_EQ(a.slab_live(), 0u);
    EXPECT_EQ(a.tlsf_live(), 0u);
}

}  // namespace
}  // namespace memalloc
