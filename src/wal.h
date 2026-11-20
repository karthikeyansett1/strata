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

  static std::string FileName(std::uint64_t start);
  Status Open(const std::string& path, std::uint64_t start = 1, bool create = true);
  Status RequireEmpty() const;
  Status Recover(const Replay& replay, std::uint64_t* last_sequence, bool allow_torn_tail = true);
  Status Append(std::string_view key, std::string_view value, RecordType type,
                std::uint64_t sequence, bool sync = true);
  Status SyncWrites() const;
  static bool ValidSize(std::string_view key, std::string_view value);

 private:
  int fd_ = -1;
  std::uint64_t start_ = 1;
};

}  // namespace strata
