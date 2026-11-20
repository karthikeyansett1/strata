#include "strata/db.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

#include "memtable.h"
#include "wal.h"

namespace strata {

struct DB::State {
  Wal wal;
  Memtable memtable;
  std::mutex writer;
  std::uint64_t sequence = 0;
  bool write_failed = false;
};

DB::DB(ConstructionKey) : state_(std::make_unique<State>()) {}
DB::~DB() = default;

Status DB::Open(const Options& opts, const std::string& path, std::unique_ptr<DB>* out) noexcept
    try {
  if (!out || path.empty() || path.find('\0') != std::string::npos) {
    return {StatusCode::kInvalidArgument, "Open requires a path and an output pointer"};
  }
  if (opts.sync_mode != SyncMode::kAlways && opts.sync_mode != SyncMode::kBatch) {
    return {StatusCode::kInvalidArgument, "unsupported sync mode"};
  }

  auto db = std::make_unique<DB>(ConstructionKey{});
  auto& state = *db->state_;
  if (auto status = state.wal.Open(path); !status.ok()) {
    return status;
  }
  const auto replay = [&state](std::string_view key, std::string_view value, RecordType type,
                               std::uint64_t sequence) {
    state.memtable.Apply(std::string(key), {sequence, type, std::string(value)});
  };
  if (auto status = state.wal.Recover(replay, &state.sequence); !status.ok()) {
    return status;
  }

  *out = std::move(db);
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status DB::Put(std::string_view key, std::string_view value) noexcept {
  return Write(key, value, false);
}

Status DB::Delete(std::string_view key) noexcept { return Write(key, {}, true); }

Status DB::Get(std::string_view key, std::string* value) noexcept try {
  if (!value) {
    return {StatusCode::kInvalidArgument, "Get requires an output pointer"};
  }
  auto entry = state_->memtable.Get(key);
  if (!entry || entry->type == RecordType::kDelete) {
    return {StatusCode::kNotFound, "key not found"};
  }
  *value = std::move(entry->value);
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status DB::Write(std::string_view key, std::string_view value, bool deleted) noexcept try {
  auto& state = *state_;
  std::lock_guard lock(state.writer);
  if (state.write_failed) {
    return {StatusCode::kIOError, "reopen the database after a failed write"};
  }
  if (state.sequence == std::numeric_limits<std::uint64_t>::max()) {
    return {StatusCode::kIOError, "WAL sequence space exhausted"};
  }

  const auto sequence = state.sequence + 1;
  const auto type = deleted ? RecordType::kDelete : RecordType::kPut;

  // Until the durable record is published, failures leave the handle read-only.
  state.write_failed = true;
  auto status = state.wal.Append(key, value, type, sequence);
  if (!status.ok()) {
    if (status.code() == StatusCode::kInvalidArgument) {
      state.write_failed = false;
    }
    return status;
  }
  state.memtable.Apply(std::string(key), {sequence, type, std::string(value)});
  state.sequence = sequence;
  state.write_failed = false;
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

}  // namespace strata
