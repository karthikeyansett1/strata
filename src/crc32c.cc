#include "crc32c.h"

#include <array>
#include <cstring>

#if defined(__x86_64__)
#include <nmmintrin.h>
#elif defined(__aarch64__) && defined(__linux__) && defined(__AARCH64EL__)
#include <arm_acle.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

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

std::uint32_t Software(std::string_view bytes) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (unsigned char byte : bytes) {
    crc = kTable[(crc ^ byte) & 0xff] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

#if defined(__x86_64__)
__attribute__((target("sse4.2")))
std::uint32_t Hardware(std::string_view bytes) noexcept {
  std::uint64_t crc = 0xffffffffu;
  while (bytes.size() >= sizeof(std::uint64_t)) {
    std::uint64_t word;
    std::memcpy(&word, bytes.data(), sizeof(word));
    crc = _mm_crc32_u64(crc, word);
    bytes.remove_prefix(sizeof(word));
  }
  for (unsigned char byte : bytes) {
    crc = _mm_crc32_u8(static_cast<std::uint32_t>(crc), byte);
  }
  return static_cast<std::uint32_t>(crc) ^ 0xffffffffu;
}
#elif defined(__aarch64__) && defined(__linux__) && defined(__AARCH64EL__)
__attribute__((target("+crc")))
std::uint32_t Hardware(std::string_view bytes) noexcept {
  std::uint32_t crc = 0xffffffffu;
  while (bytes.size() >= sizeof(std::uint64_t)) {
    std::uint64_t word;
    std::memcpy(&word, bytes.data(), sizeof(word));
    crc = __crc32cd(crc, word);
    bytes.remove_prefix(sizeof(word));
  }
  for (unsigned char byte : bytes) {
    crc = __crc32cb(crc, byte);
  }
  return crc ^ 0xffffffffu;
}
#endif

auto SelectImplementation() noexcept {
#if defined(__x86_64__)
  if (__builtin_cpu_supports("sse4.2")) {
    return &Hardware;
  }
#elif defined(__aarch64__) && defined(__linux__) && defined(__AARCH64EL__)
  if (getauxval(AT_HWCAP) & HWCAP_CRC32) {
    return &Hardware;
  }
#endif
  return &Software;
}

}  // namespace

std::uint32_t Crc32c(std::string_view bytes) noexcept {
  static const auto hash = SelectImplementation();
  return hash(bytes);
}

}  // namespace strata
