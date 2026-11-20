#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "strata/options.h"
#include "strata/status.h"

namespace strata {

struct Stats {
  std::uint64_t sequence = 0;
  std::uint64_t wal_syncs = 0;
  std::uint64_t compactions = 0;
  std::size_t sstable_count = 0;
};

class DB {
 private:
  class ConstructionKey {
    friend class DB;
    ConstructionKey() = default;
  };

 public:
  explicit DB(ConstructionKey);
  [[nodiscard]] static Status Open(const Options& opts, const std::string& path,
                                   std::unique_ptr<DB>* out) noexcept;
  [[nodiscard]] Status Put(std::string_view key, std::string_view value) noexcept;
  [[nodiscard]] Status Get(std::string_view key, std::string* value) noexcept;
  [[nodiscard]] Status Delete(std::string_view key) noexcept;
  [[nodiscard]] Status Flush() noexcept;
  [[nodiscard]] Status CompactAll() noexcept;
  [[nodiscard]] Stats GetStats() const noexcept;
  [[nodiscard]] Status Scan(std::string_view start, std::string_view end,
                            std::vector<std::pair<std::string, std::string>>* out) noexcept;
  ~DB();

 private:
  struct State;
  Status Write(std::string_view key, std::string_view value, bool deleted) noexcept;

  std::unique_ptr<State> state_;
};

}  // namespace strata
