#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "memalloc/align.hpp"
#include "memalloc/arena.hpp"
#include "memalloc/tlsf.hpp"

namespace memalloc {
namespace {

constexpr std::size_t kMiB = 1024 * 1024;

TEST(Tlsf, StartsValidWithOneRegion) {
    Arena arena(4 * kMiB);
    Tlsf tlsf(arena, 64 * 1024);
    EXPECT_EQ(tlsf.region_count(), 1u);
    EXPECT_EQ(tlsf.used_bytes(), 0u);
    EXPECT_GT(tlsf.free_bytes(), 0u);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, BasicAllocateReturnsUsableAlignedMemory) {
    Arena arena(4 * kMiB);
    Tlsf tlsf(arena, 64 * 1024);

    void* p = tlsf.allocate(100);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned_ptr(p, kAlignSize));
    EXPECT_TRUE(arena.owns(p));

    std::memset(p, 0xCD, 100);  // fully writable (ASan-checked)
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, AllocationsAreDistinctAndNonOverlapping) {
    Arena arena(8 * kMiB);
    Tlsf tlsf(arena, 256 * 1024);

    struct Span {
        char* lo;
        std::size_t n;
    };
    std::vector<Span> spans;
    for (int i = 0; i < 200; ++i) {
        const std::size_t n = 8 + static_cast<std::size_t>(i) * 13;
        void* p = tlsf.allocate(n);
        ASSERT_NE(p, nullptr);
        std::memset(p, i & 0xFF, n);
        spans.push_back({static_cast<char*>(p), n});
    }
    for (std::size_t i = 0; i < spans.size(); ++i) {
        for (std::size_t j = i + 1; j < spans.size(); ++j) {
            char* a_hi = spans[i].lo + spans[i].n;
            char* b_hi = spans[j].lo + spans[j].n;
            EXPECT_TRUE(a_hi <= spans[j].lo || b_hi <= spans[i].lo)
                << "overlap between " << i << " and " << j;
        }
    }
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, AllocAccountingTracksUsedBytes) {
    Arena arena(4 * kMiB);
    Tlsf tlsf(arena, 128 * 1024);

    void* a = tlsf.allocate(64);
    void* b = tlsf.allocate(256);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // Each allocation rounds up to >= request; used must cover both.
    EXPECT_GE(tlsf.used_bytes(), 64u + 256u);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, SplitLeavesRemainderAndFreeReclaimsIt) {
    Arena arena(4 * kMiB);
    Tlsf tlsf(arena, 64 * 1024);

    const std::size_t free_before = tlsf.free_bytes();
    void* p = tlsf.allocate(128);
    ASSERT_NE(p, nullptr);
    EXPECT_LT(tlsf.free_bytes(), free_before);  // splitting consumed free space
    EXPECT_TRUE(tlsf.validate());

    tlsf.free(p);
    // After freeing the only allocation, all payload coalesces back. Free
    // bytes should be >= the original (overhead from the split is reclaimed).
    EXPECT_GE(tlsf.free_bytes(), free_before);
    EXPECT_EQ(tlsf.used_bytes(), 0u);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, ForwardCoalescing) {
    Arena arena(4 * kMiB);
    Tlsf tlsf(arena, 64 * 1024);

    void* a = tlsf.allocate(512);
    void* b = tlsf.allocate(512);
    void* c = tlsf.allocate(512);  // keeps b from coalescing with the tail
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    tlsf.free(a);  // a free
    tlsf.free(b);  // b should merge forward into a's neighbor / vice versa
    EXPECT_TRUE(tlsf.validate());  // validate forbids adjacent free blocks

    tlsf.free(c);
    EXPECT_EQ(tlsf.used_bytes(), 0u);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, BackwardCoalescing) {
    Arena arena(4 * kMiB);
    Tlsf tlsf(arena, 64 * 1024);

    void* a = tlsf.allocate(512);
    void* b = tlsf.allocate(512);
    void* c = tlsf.allocate(512);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    tlsf.free(a);  // free the earlier physical neighbor first
    tlsf.free(b);  // b must coalesce *backward* into a
    EXPECT_TRUE(tlsf.validate());

    tlsf.free(c);
    EXPECT_EQ(tlsf.used_bytes(), 0u);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, BidirectionalCoalescingMergesBothNeighbors) {
    Arena arena(4 * kMiB);
    Tlsf tlsf(arena, 64 * 1024);

    void* a = tlsf.allocate(512);
    void* b = tlsf.allocate(512);
    void* c = tlsf.allocate(512);
    void* d = tlsf.allocate(512);  // guard so c doesn't touch the tail
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(d, nullptr);

    tlsf.free(a);                  // a free
    tlsf.free(c);                  // c free  (a)_b_(c) with b allocated
    tlsf.free(b);                  // freeing b merges with BOTH a and c
    EXPECT_TRUE(tlsf.validate());

    tlsf.free(d);
    EXPECT_EQ(tlsf.used_bytes(), 0u);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, FullCycleReturnsToInitialFreeBytes) {
    Arena arena(8 * kMiB);
    Tlsf tlsf(arena, 256 * 1024);
    const std::size_t free0 = tlsf.free_bytes();

    std::vector<void*> live;
    for (int i = 0; i < 300; ++i) {
        void* p = tlsf.allocate(40 + (i % 17) * 24);
        ASSERT_NE(p, nullptr);
        live.push_back(p);
    }
    ASSERT_TRUE(tlsf.validate());
    // Free in a scrambled order to exercise both coalesce directions.
    for (std::size_t i = 0; i < live.size(); i += 2) tlsf.free(live[i]);
    for (std::size_t i = 1; i < live.size(); i += 2) tlsf.free(live[i]);

    EXPECT_EQ(tlsf.used_bytes(), 0u);
    EXPECT_TRUE(tlsf.validate());
    // Everything coalesced back; no permanent fragmentation within a region.
    EXPECT_GE(tlsf.free_bytes(), free0);
}

TEST(Tlsf, ReuseAfterFree) {
    Arena arena(4 * kMiB);
    Tlsf tlsf(arena, 64 * 1024);
    void* a = tlsf.allocate(200);
    ASSERT_NE(a, nullptr);
    tlsf.free(a);
    void* b = tlsf.allocate(200);
    EXPECT_EQ(a, b) << "freed block should be reused";
    tlsf.free(b);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, GrowsByAddingRegionsWhenExhausted) {
    Arena arena(8 * kMiB);
    Tlsf tlsf(arena, 64 * 1024);  // small initial region
    EXPECT_EQ(tlsf.region_count(), 1u);

    std::vector<void*> live;
    for (int i = 0; i < 4000; ++i) {
        void* p = tlsf.allocate(256);
        ASSERT_NE(p, nullptr);
        live.push_back(p);
    }
    EXPECT_GT(tlsf.region_count(), 1u);  // had to grow
    EXPECT_TRUE(tlsf.validate());
    for (void* p : live) tlsf.free(p);
    EXPECT_EQ(tlsf.used_bytes(), 0u);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, LargeRequestTriggersBigRegion) {
    Arena arena(8 * kMiB);
    Tlsf tlsf(arena, 4 * 1024);  // tiny default chunk
    void* p = tlsf.allocate(1 * kMiB);  // far bigger than the default chunk
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xEE, 1 * kMiB);
    EXPECT_TRUE(tlsf.validate());
    tlsf.free(p);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, ExhaustionReturnsNull) {
    Arena arena(64 * 1024);   // whole arena is tiny
    Tlsf tlsf(arena, 32 * 1024);
    std::vector<void*> live;
    void* p;
    int guard = 0;
    while ((p = tlsf.allocate(1024)) != nullptr) {
        live.push_back(p);
        ASSERT_LT(guard++, 100000);
    }
    EXPECT_EQ(tlsf.allocate(1024), nullptr);  // stays null
    EXPECT_TRUE(tlsf.validate());
    for (void* q : live) tlsf.free(q);
    EXPECT_TRUE(tlsf.validate());
}

TEST(Tlsf, ZeroAndNullAreSafe) {
    Arena arena(kMiB);
    Tlsf tlsf(arena, 64 * 1024);
    EXPECT_EQ(tlsf.allocate(0), nullptr);
    tlsf.free(nullptr);  // no-op
    EXPECT_TRUE(tlsf.validate());
}

}  // namespace
}  // namespace memalloc
