#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "strata/db.h"
#include "strata/vector_store.h"

namespace strata::server {

constexpr std::size_t kMaxLine = 64 * 1024;
constexpr std::size_t kMaxResponse = 16 * 1024 * 1024;

class Input {
 public:
  bool Append(std::string_view data);
  std::optional<std::string> Next();

 private:
  std::string buffer_;
};

std::string Execute(DB& db, std::string_view line, VectorStore* vectors = nullptr);

}  // namespace strata::server
