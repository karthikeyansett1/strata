#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace strata {

class Bloom {
 public:
  Bloom(std::size_t keys, std::size_t bits_per_key)
      : bits_(std::max<std::size_t>(8, (keys * bits_per_key + 7) / 8), '\0'),
        probes_(std::clamp<std::size_t>(bits_per_key * 69 / 100, 1, 30)) {}

  Bloom(std::string bits, std::uint8_t probes) : bits_(std::move(bits)), probes_(probes) {}

  void Insert(std::string_view key) {
    auto [hash, step] = Hash(key);
    for (unsigned i = 0; i < probes_; ++i, hash += step) {
      const auto bit = hash % (bits_.size() * 8);
      bits_[bit / 8] |= static_cast<char>(1u << (bit % 8));
    }
  }

  bool MayContain(std::string_view key) const {
    auto [hash, step] = Hash(key);
    for (unsigned i = 0; i < probes_; ++i, hash += step) {
      const auto bit = hash % (bits_.size() * 8);
      if (!(static_cast<unsigned char>(bits_[bit / 8]) & (1u << (bit % 8)))) {
        return false;
      }
    }
    return true;
  }

  const std::string& bits() const { return bits_; }
  std::uint8_t probes() const { return probes_; }

 private:
  static std::uint64_t Mix(std::uint64_t h) {
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
  }

  static std::pair<std::uint64_t, std::uint64_t> Hash(std::string_view key) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char byte : key) {
      h = (h ^ byte) * 0x100000001b3ULL;
    }
    return {Mix(h), Mix(h ^ 0x9e3779b97f4a7c15ULL) | 1};
  }

  std::string bits_;
  std::uint8_t probes_;
};

}  // namespace strata
