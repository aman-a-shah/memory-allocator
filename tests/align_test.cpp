#include <gtest/gtest.h>

#include <cstddef>

#include "memalloc/align.hpp"

namespace memalloc {
namespace {

TEST(Align, PowerOfTwo) {
    EXPECT_FALSE(is_power_of_two(0));
    EXPECT_TRUE(is_power_of_two(1));
    EXPECT_TRUE(is_power_of_two(2));
    EXPECT_TRUE(is_power_of_two(64));
    EXPECT_FALSE(is_power_of_two(3));
    EXPECT_FALSE(is_power_of_two(48));
}

TEST(Align, AlignUp) {
    EXPECT_EQ(align_up(0, 8), 0u);
    EXPECT_EQ(align_up(1, 8), 8u);
    EXPECT_EQ(align_up(8, 8), 8u);
    EXPECT_EQ(align_up(9, 8), 16u);
    EXPECT_EQ(align_up(63, 64), 64u);
    EXPECT_EQ(align_up(65, 64), 128u);
}

TEST(Align, AlignDown) {
    EXPECT_EQ(align_down(0, 8), 0u);
    EXPECT_EQ(align_down(7, 8), 0u);
    EXPECT_EQ(align_down(8, 8), 8u);
    EXPECT_EQ(align_down(15, 8), 8u);
    EXPECT_EQ(align_down(127, 64), 64u);
}

TEST(Align, IsAligned) {
    EXPECT_TRUE(is_aligned(0, 8));
    EXPECT_TRUE(is_aligned(16, 8));
    EXPECT_FALSE(is_aligned(12, 8));
    EXPECT_TRUE(is_aligned(64, 64));
    EXPECT_FALSE(is_aligned(96, 64));
}

// align_up/align_down/is_power_of_two must be usable in constant expressions.
TEST(Align, ConstexprUsable) {
    static_assert(is_power_of_two(16), "");
    static_assert(align_up(5, 8) == 8, "");
    static_assert(align_down(15, 8) == 8, "");
    static_assert(!is_aligned(3, 4), "");
    SUCCEED();
}

}  // namespace
}  // namespace memalloc
