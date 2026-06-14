#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "memalloc/align.hpp"
#include "memalloc/arena.hpp"

namespace memalloc {
namespace {

constexpr std::size_t kMiB = 1024 * 1024;

TEST(Arena, CapacityRoundsUpToPage) {
    Arena arena(1);                       // 1 byte requested
    EXPECT_GE(arena.capacity(), 1u);
    EXPECT_EQ(arena.capacity() % 4096, 0u);  // page-aligned (page >= 4096)
    EXPECT_EQ(arena.used(), 0u);
    EXPECT_EQ(arena.remaining(), arena.capacity());
}

TEST(Arena, AllocateReturnsInRangeAndOwned) {
    Arena arena(kMiB);
    void* p = arena.allocate(128);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(arena.owns(p));
    EXPECT_GE(p, arena.base());
    EXPECT_LT(static_cast<char*>(p),
              static_cast<char*>(arena.base()) + arena.capacity());
}

TEST(Arena, RespectsDefaultAlignment) {
    Arena arena(kMiB);
    // Force a non-aligned cursor, then allocate with default alignment.
    arena.allocate(1, 1);
    void* p = arena.allocate(64);  // default alignof(max_align_t)
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned_ptr(p, alignof(std::max_align_t)));
}

TEST(Arena, RespectsCacheLineAlignment) {
    Arena arena(kMiB);
    arena.allocate(1, 1);
    void* p = arena.allocate(256, kCacheLineSize);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned_ptr(p, kCacheLineSize));
}

TEST(Arena, SubBlocksDoNotOverlap) {
    Arena arena(kMiB);
    std::vector<std::pair<char*, std::size_t>> blocks;
    for (int i = 0; i < 64; ++i) {
        const std::size_t n = 64 + static_cast<std::size_t>(i) * 8;
        void* p = arena.allocate(n);
        ASSERT_NE(p, nullptr);
        blocks.emplace_back(static_cast<char*>(p), n);
    }
    // Every pair of [start, end) ranges must be disjoint.
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        for (std::size_t j = i + 1; j < blocks.size(); ++j) {
            char* a_lo = blocks[i].first;
            char* a_hi = a_lo + blocks[i].second;
            char* b_lo = blocks[j].first;
            char* b_hi = b_lo + blocks[j].second;
            EXPECT_TRUE(a_hi <= b_lo || b_hi <= a_lo)
                << "blocks " << i << " and " << j << " overlap";
        }
    }
}

TEST(Arena, WritableAcrossWholeAllocation) {
    Arena arena(kMiB);
    const std::size_t n = 4096;
    auto* p = static_cast<unsigned char*>(arena.allocate(n));
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, n);  // must not fault (ASan-checked)
    EXPECT_EQ(p[0], 0xAB);
    EXPECT_EQ(p[n - 1], 0xAB);
}

TEST(Arena, ExhaustionReturnsNullGracefully) {
    Arena arena(kMiB);
    void* big = arena.allocate(arena.capacity() + 1);
    EXPECT_EQ(big, nullptr);              // too large from the start
    EXPECT_EQ(arena.used(), 0u);         // failed alloc must not move cursor

    // Drain the arena, then confirm the next request fails cleanly.
    void* whole = arena.allocate(arena.capacity());
    ASSERT_NE(whole, nullptr);
    EXPECT_EQ(arena.remaining(), 0u);
    EXPECT_EQ(arena.allocate(1), nullptr);
}

TEST(Arena, RejectsZeroBytesAndBadAlignment) {
    Arena arena(kMiB);
    EXPECT_EQ(arena.allocate(0), nullptr);
    EXPECT_EQ(arena.allocate(16, 3), nullptr);   // alignment not power of two
    EXPECT_EQ(arena.allocate(16, 0), nullptr);
    EXPECT_EQ(arena.used(), 0u);
}

TEST(Arena, ResetRewindsCursorAndReusesMemory) {
    Arena arena(kMiB);
    void* first = arena.allocate(256);
    ASSERT_NE(first, nullptr);
    EXPECT_GT(arena.used(), 0u);

    arena.reset();
    EXPECT_EQ(arena.used(), 0u);

    void* again = arena.allocate(256);
    EXPECT_EQ(again, first);  // same address handed back after reset
}

TEST(Arena, OwnsRejectsOutsidePointers) {
    Arena arena(kMiB);
    int stack_var = 0;
    EXPECT_FALSE(arena.owns(&stack_var));
    EXPECT_FALSE(arena.owns(nullptr));
}

TEST(Arena, MoveTransfersOwnership) {
    Arena a(kMiB);
    void* p = a.allocate(128);
    void* a_base = a.base();
    ASSERT_NE(p, nullptr);

    Arena b(std::move(a));
    EXPECT_EQ(b.base(), a_base);
    EXPECT_TRUE(b.owns(p));
    EXPECT_EQ(a.base(), nullptr);     // moved-from is emptied
    EXPECT_EQ(a.capacity(), 0u);
}

}  // namespace
}  // namespace memalloc
