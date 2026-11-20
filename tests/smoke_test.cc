#include <gtest/gtest.h>

namespace strata {
const char* BuildName() noexcept;
}

TEST(StrataSmoke, LinksAndRuns) { EXPECT_STREQ(strata::BuildName(), "Strata"); }
