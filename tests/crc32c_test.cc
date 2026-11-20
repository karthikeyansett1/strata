#include "crc32c.h"

#include <gtest/gtest.h>

#include <string>

namespace strata {
namespace {

TEST(Crc32c, KnownVectors) {
  EXPECT_EQ(Crc32c(""), 0u);
  EXPECT_EQ(Crc32c("123456789"), 0xe3069283u);
  EXPECT_EQ(Crc32c(std::string(32, '\0')), 0x8a9136aau);
  EXPECT_EQ(Crc32c(std::string(32, '\xff')), 0x62a8ab43u);
}

}  // namespace
}  // namespace strata
