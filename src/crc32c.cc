#include "crc32c.h"

#include <array>

namespace strata {
namespace {

constexpr auto kTable = [] {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value >> 1) ^ ((value & 1) ? 0x82f63b78u : 0);
    }
    table[i] = value;
  }
  return table;
}();

}  // namespace

std::uint32_t Crc32c(std::string_view bytes) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (unsigned char byte : bytes) {
    crc = kTable[(crc ^ byte) & 0xff] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

}  // namespace strata
