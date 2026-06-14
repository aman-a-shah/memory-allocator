#include <gtest/gtest.h>

#include <cstdint>
#include <deque>

#include "memalloc/align.hpp"
#include "memalloc/tlsf.hpp"

namespace memalloc {
namespace {

// Allocates a BlockHeader with a stable address (deque never relocates).
BlockHeader* make_block(std::deque<BlockHeader>& store, std::uint32_t size) {
    BlockHeader b{};
    b.size = size;
    store.push_back(b);
    return &store.back();
}

// ---- Size -> bin mapping -------------------------------------------------

TEST(TlsfMapping, SmallRegionIsLinear) {
    // Below kSmallBlockSize everything is first level 0, sub-bin = size / 8.
    EXPECT_EQ(tlsf_mapping_insert(8).fl, 0);
    EXPECT_EQ(tlsf_mapping_insert(8).sl, 1);
    EXPECT_EQ(tlsf_mapping_insert(16).sl, 2);
    EXPECT_EQ(tlsf_mapping_insert(255).fl, 0);
    EXPECT_EQ(tlsf_mapping_insert(255).sl, 31);
}

TEST(TlsfMapping, FirstLevelBoundaries) {
    // 256 = 2^8 -> first power-of-two class, sub-bin 0.
    auto b256 = tlsf_mapping_insert(256);
    EXPECT_EQ(b256.fl, 1);
    EXPECT_EQ(b256.sl, 0);

    // Just below 512 is the top sub-bin of the same first level.
    auto b511 = tlsf_mapping_insert(511);
    EXPECT_EQ(b511.fl, 1);
    EXPECT_EQ(b511.sl, 31);

    // 512 = 2^9 advances the first level.
    auto b512 = tlsf_mapping_insert(512);
    EXPECT_EQ(b512.fl, 2);
    EXPECT_EQ(b512.sl, 0);
}

TEST(TlsfMapping, SubBinWidthMatchesFirstLevel) {
    // At fl=1 (range [256,512)) each sub-bin spans 8 bytes.
    EXPECT_EQ(tlsf_mapping_insert(256).sl, 0);
    EXPECT_EQ(tlsf_mapping_insert(263).sl, 0);
    EXPECT_EQ(tlsf_mapping_insert(264).sl, 1);
}

TEST(TlsfMapping, AllIndicesWithinBounds) {
    for (std::uint32_t s = kMinBlockSize; s < (1u << 20); s += kAlignSize) {
        BinIndex bi = tlsf_mapping_insert(s);
        ASSERT_GE(bi.fl, 0);
        ASSERT_LT(bi.fl, kFLIndexCount);
        ASSERT_GE(bi.sl, 0);
        ASSERT_LT(bi.sl, kSLIndexCount);
    }
}

TEST(TlsfMapping, SearchRoundsUpNeverDown) {
    // The search bin must be >= the insert bin for the same size.
    for (std::uint32_t s = kSmallBlockSize; s < (1u << 18); s += 17) {
        BinIndex ins = tlsf_mapping_insert(s);
        BinIndex sea = tlsf_mapping_search(s);
        const int ins_flat = ins.fl * kSLIndexCount + ins.sl;
        const int sea_flat = sea.fl * kSLIndexCount + sea.sl;
        EXPECT_GE(sea_flat, ins_flat) << "size=" << s;
    }
}

// ---- Bitmap bookkeeping --------------------------------------------------

TEST(TlsfMatrix, InsertSetsBitsRemoveClears) {
    FreeListMatrix m;
    std::deque<BlockHeader> store;
    BlockHeader* b = make_block(store, 300);
    BinIndex bi = tlsf_mapping_insert(300);

    EXPECT_FALSE(m.bin_occupied(bi.fl, bi.sl));
    m.insert(b);
    EXPECT_TRUE(m.bin_occupied(bi.fl, bi.sl));
    EXPECT_NE(m.fl_bitmap() & (1u << bi.fl), 0u);
    EXPECT_EQ(m.bin_head(bi.fl, bi.sl), b);

    m.remove(b);
    EXPECT_FALSE(m.bin_occupied(bi.fl, bi.sl));
    EXPECT_EQ(m.fl_bitmap() & (1u << bi.fl), 0u);
    EXPECT_EQ(m.bin_head(bi.fl, bi.sl), nullptr);
}

TEST(TlsfMatrix, TwoBlocksSameBinKeepBitSetUntilEmpty) {
    FreeListMatrix m;
    std::deque<BlockHeader> store;
    // Same size -> same bin.
    BlockHeader* a = make_block(store, 300);
    BlockHeader* b = make_block(store, 300);
    BinIndex bi = tlsf_mapping_insert(300);

    m.insert(a);
    m.insert(b);  // b becomes head
    EXPECT_EQ(m.bin_head(bi.fl, bi.sl), b);
    EXPECT_TRUE(m.bin_occupied(bi.fl, bi.sl));

    m.remove(b);  // remove head; a remains
    EXPECT_EQ(m.bin_head(bi.fl, bi.sl), a);
    EXPECT_TRUE(m.bin_occupied(bi.fl, bi.sl));

    m.remove(a);  // now empty
    EXPECT_FALSE(m.bin_occupied(bi.fl, bi.sl));
}

TEST(TlsfMatrix, RemoveMiddleBlockKeepsListIntact) {
    FreeListMatrix m;
    std::deque<BlockHeader> store;
    BlockHeader* a = make_block(store, 300);
    BlockHeader* b = make_block(store, 300);
    BlockHeader* c = make_block(store, 300);
    BinIndex bi = tlsf_mapping_insert(300);

    m.insert(a);
    m.insert(b);
    m.insert(c);  // list: c -> b -> a
    m.remove(b);  // remove the middle node

    EXPECT_EQ(m.bin_head(bi.fl, bi.sl), c);
    EXPECT_EQ(c->next_free, a);
    EXPECT_EQ(a->prev_free, c);
    EXPECT_TRUE(m.bin_occupied(bi.fl, bi.sl));
}

// ---- Good-fit lookup -----------------------------------------------------

TEST(TlsfSearch, EmptyMatrixReturnsNull) {
    FreeListMatrix m;
    EXPECT_EQ(m.find_suitable(64), nullptr);
}

TEST(TlsfSearch, ReturnsSmallestFittingBin) {
    FreeListMatrix m;
    std::deque<BlockHeader> store;
    BlockHeader* small = make_block(store, 64);
    BlockHeader* mid = make_block(store, 128);
    BlockHeader* big = make_block(store, 4096);
    m.insert(small);
    m.insert(mid);
    m.insert(big);

    EXPECT_EQ(m.find_suitable(64), small);   // exact
    EXPECT_EQ(m.find_suitable(100), mid);    // 64 too small -> 128
    EXPECT_EQ(m.find_suitable(4096), big);   // jumps first levels
}

TEST(TlsfSearch, ReturnsNullWhenNothingLargeEnough) {
    FreeListMatrix m;
    std::deque<BlockHeader> store;
    m.insert(make_block(store, 4096));
    EXPECT_EQ(m.find_suitable(5000), nullptr);
}

TEST(TlsfSearch, JumpsToNextFirstLevelWhenBinExhausted) {
    FreeListMatrix m;
    std::deque<BlockHeader> store;
    // Only a large block exists; a small-ish request must climb first levels.
    BlockHeader* big = make_block(store, 1u << 16);
    m.insert(big);
    EXPECT_EQ(m.find_suitable(300), big);
}

TEST(TlsfSearch, ResultAlwaysSatisfiesRequest) {
    // Safety invariant: any returned block is >= the (aligned) request.
    FreeListMatrix m;
    std::deque<BlockHeader> store;
    const std::uint32_t sizes[] = {32, 80, 200, 256, 500, 1024, 9001, 65536};
    for (std::uint32_t s : sizes) m.insert(make_block(store, s));

    for (std::uint32_t r = 8; r <= 70000; r += 7) {
        BlockHeader* b = m.find_suitable(r);
        if (b != nullptr) {
            const std::uint32_t want =
                static_cast<std::uint32_t>(align_up(r, kAlignSize));
            EXPECT_GE(b->size, want) << "request=" << r;
        }
    }
}

TEST(TlsfSearch, ReportsChosenBin) {
    FreeListMatrix m;
    std::deque<BlockHeader> store;
    BlockHeader* b = make_block(store, 300);
    m.insert(b);

    BinIndex chosen{};
    BlockHeader* got = m.find_suitable(280, &chosen);
    ASSERT_EQ(got, b);
    EXPECT_TRUE(m.bin_occupied(chosen.fl, chosen.sl));
}

}  // namespace
}  // namespace memalloc
