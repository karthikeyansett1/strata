#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "strata/options.h"
#include "strata/status.h"

namespace strata {

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
  [[nodiscard]] Status Scan(std::string_view start, std::string_view end,
                            std::vector<std::pair<std::string, std::string>>* out) noexcept;
  ~DB();

 private:
  struct State;
  Status Write(std::string_view key, std::string_view value, bool deleted) noexcept;

  std::unique_ptr<State> state_;
};

}  // namespace strata
