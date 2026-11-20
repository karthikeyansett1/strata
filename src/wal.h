#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "memtable.h"
#include "strata/status.h"

namespace strata {

class Wal {
 public:
  static constexpr char kFileName[] = "wal-1.log";

  Wal() = default;
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  using Replay = std::function<void(std::string_view key, std::string_view value, RecordType type,
                                    std::uint64_t sequence)>;

  Status Open(const std::string& path);
  Status Recover(const Replay& replay, std::uint64_t* last_sequence);
  Status Append(std::string_view key, std::string_view value, RecordType type,
                std::uint64_t sequence);

 private:
  int fd_ = -1;
};

}  // namespace strata
