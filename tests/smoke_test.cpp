#include <gtest/gtest.h>

#include "memalloc/memalloc.hpp"

// Phase 0 smoke test: proves the test harness links against the library and
// runs. Real allocator tests arrive with the Arena layer in Phase 1.
TEST(Smoke, VersionMatchesProject) {
    const auto v = memalloc::version();
    EXPECT_EQ(v.major, 0);
    EXPECT_EQ(v.minor, 1);
    EXPECT_EQ(v.patch, 0);
    EXPECT_STREQ(memalloc::version_string(), "0.1.0");
}
