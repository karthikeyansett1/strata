#include "crc32c.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

namespace strata {
namespace {

TEST(Crc32c, KnownVectors) {
  EXPECT_EQ(Crc32c(""), 0u);
  EXPECT_EQ(Crc32c("123456789"), 0xe3069283u);
  EXPECT_EQ(Crc32c(std::string(32, '\0')), 0x8a9136aau);
  EXPECT_EQ(Crc32c(std::string(32, '\xff')), 0x62a8ab43u);
}

TEST(Crc32c, UnalignedBuffersAndTailsMatchBitwiseReference) {
  std::mt19937 random(4831);
  std::string bytes(4096, '\0');
  for (auto& byte : bytes) {
    byte = static_cast<char>(random());
  }
  for (std::size_t offset = 0; offset < 8; ++offset) {
    for (std::size_t length : {0, 1, 7, 8, 9, 63, 64, 65, 4053}) {
      const auto data = std::string_view(bytes).substr(offset, length);
      std::uint32_t expected = 0xffffffffu;
      for (unsigned char byte : data) {
        expected ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
          expected = expected & 1 ? (expected >> 1) ^ 0x82f63b78u : expected >> 1;
        }
      }
      EXPECT_EQ(Crc32c(data), expected ^ 0xffffffffu) << offset << ' ' << length;
    }
  }
}

}  // namespace
}  // namespace strata
