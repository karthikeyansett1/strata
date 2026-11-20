#include "strata/db.h"

#include <sys/file.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <queue>
#include <shared_mutex>

#include "sstable.h"
#include "wal.h"

namespace strata {
namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

void MergeRows(const std::vector<Rows>& sources, Pairs* result) {
  struct Cursor {
    const Rows* rows;
    std::size_t position = 0;

    const Rows::value_type& row() const { return (*rows)[position]; }
  };

  const auto after = [](const Cursor& left, const Cursor& right) {
    if (left.row().first != right.row().first) {
      return left.row().first > right.row().first;
    }
    return left.row().second.sequence < right.row().second.sequence;
  };
  std::priority_queue<Cursor, std::vector<Cursor>, decltype(after)> heap(after);
  for (const auto& rows : sources) {
    if (!rows.empty()) {
      heap.push({&rows});
    }
  }

  while (!heap.empty()) {
    const auto& [key, entry] = heap.top().row();
    if (entry.type == RecordType::kPut) {
      result->emplace_back(key, entry.value);
    }
    do {
      auto cursor = heap.top();
      heap.pop();
      if (++cursor.position < cursor.rows->size()) {
        heap.push(cursor);
      }
    } while (!heap.empty() && heap.top().row().first == key);
  }
}

}  // namespace

struct DB::State {
  disk::File directory_fd;
  std::filesystem::path directory;
  std::unique_ptr<Wal> wal = std::make_unique<Wal>();
  std::unique_ptr<Memtable> memtable = std::make_unique<Memtable>();
  Manifest manifest;
  std::vector<SSTable> tables;
  Options options;
  std::mutex writer;
  std::shared_mutex view;
  std::uint64_t sequence = 0;
  std::size_t memtable_bytes = 0;
  bool write_failed = false;

  void Account(std::string_view key, std::string_view value) {
    const auto size = key.size() + value.size() + 25;
    memtable_bytes += std::min(size, std::numeric_limits<std::size_t>::max() - memtable_bytes);
  }

  Status FlushLocked() {
    if (write_failed) {
      return {StatusCode::kIOError, "reopen the database after a failed write"};
    }
    if (memtable_bytes == 0) {
      return {};
    }
    if (sequence == std::numeric_limits<std::uint64_t>::max() ||
        manifest.next_file == std::numeric_limits<std::uint64_t>::max()) {
      return {StatusCode::kIOError, "storage sequence space exhausted"};
    }

    const auto rows = memtable->OrderedEntries();
    auto next = manifest;
    const TableMetadata metadata{next.next_file++, 0, rows.front().first, rows.back().first};
    const auto table_path = TablePath(directory, metadata.id);
    if (auto status = BuildTable(table_path, directory_fd, options, rows); !status.ok()) {
      return status;
    }

    SSTable table;
    if (auto status = table.Open(table_path, metadata, sequence); !status.ok()) {
      return status;
    }
    next.sequence = sequence;
    next.wal_start = sequence + 1;
    next.tables.push_back(metadata);
    auto new_wal = std::make_unique<Wal>();
    if (auto status = new_wal->Open(directory.string(), next.wal_start); !status.ok()) {
      return status;
    }
    if (auto status = new_wal->RequireEmpty(); !status.ok()) {
      return status;
    }

    auto new_memtable = std::make_unique<Memtable>();
    tables.reserve(tables.size() + 1);
    const auto obsolete_wal = directory / Wal::FileName(manifest.wal_start);
    // A failed directory sync after rename can leave either manifest authoritative.
    write_failed = true;
    if (auto status = next.Save(directory, directory_fd); !status.ok()) {
      return status;
    }
    tables.push_back(std::move(table));
    manifest = std::move(next);
    wal.swap(new_wal);
    memtable.swap(new_memtable);
    memtable_bytes = 0;
    write_failed = false;

    // A stale WAL is harmless because recovery follows the manifest.
    if (unlink(obsolete_wal.c_str()) == 0) {
      (void)directory_fd.Sync();
    }
    return {};
  }
};

DB::DB(ConstructionKey) : state_(std::make_unique<State>()) {}
DB::~DB() = default;

Status DB::Open(const Options& opts, const std::string& path, std::unique_ptr<DB>* out) noexcept
    try {
  if (!out || path.empty() || path.find('\0') != std::string::npos) {
    return {StatusCode::kInvalidArgument, "Open requires a path and an output pointer"};
  }
  if ((opts.sync_mode != SyncMode::kAlways && opts.sync_mode != SyncMode::kBatch) ||
      opts.memtable_bytes == 0 || opts.block_bytes == 0 || opts.block_bytes > 64 * 1024 * 1024 ||
      opts.bloom_bits_per_key == 0 || opts.bloom_bits_per_key > 64) {
    return {StatusCode::kInvalidArgument, "invalid storage options"};
  }

  auto db = std::make_unique<DB>(ConstructionKey{});
  auto& state = *db->state_;
  state.options = opts;
  state.directory = std::filesystem::absolute(path);
  std::error_code error;
  std::filesystem::create_directory(state.directory, error);
  if (error) {
    return {StatusCode::kIOError, "create database directory: " + error.message()};
  }
  state.directory_fd =
      disk::File(open(state.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!state.directory_fd.valid()) {
    return disk::Error("open database directory");
  }
  while (flock(state.directory_fd.get(), LOCK_EX | LOCK_NB) != 0) {
    const int lock_error = errno;
    if (lock_error == EWOULDBLOCK) {
      return {StatusCode::kBusy, "database already open"};
    }
    if (lock_error != EINTR) {
      return disk::Error("lock database directory", lock_error);
    }
  }

  const auto loaded = state.manifest.Load(state.directory);
  const bool legacy = loaded.code() == StatusCode::kNotFound;
  if (!loaded.ok() && !legacy) {
    return loaded;
  }
  if (legacy && !std::filesystem::exists(state.directory / Wal::kFileName)) {
    for (const auto& file : std::filesystem::directory_iterator(state.directory)) {
      const auto name = file.path().filename().string();
      if (name.starts_with("sst-") || name.starts_with("wal-") || name == "MANIFEST.tmp") {
        return disk::Corruption("missing manifest and initial WAL");
      }
    }
  }
  for (const auto& metadata : state.manifest.tables) {
    SSTable table;
    if (auto status =
            table.Open(TablePath(state.directory, metadata.id), metadata, state.manifest.sequence);
        !status.ok()) {
      return status;
    }
    state.tables.push_back(std::move(table));
  }
  if (auto status = state.wal->Open(state.directory.string(), state.manifest.wal_start, legacy);
      !status.ok()) {
    return status;
  }
  const auto replay = [&state](std::string_view key, std::string_view value, RecordType type,
                               std::uint64_t sequence) {
    state.memtable->Apply(std::string(key), {sequence, type, std::string(value)});
    state.Account(key, value);
  };
  if (auto status = state.wal->Recover(replay, &state.sequence); !status.ok()) {
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

Status DB::Flush() noexcept try {
  std::lock_guard writer(state_->writer);
  std::unique_lock lock(state_->view);
  return state_->FlushLocked();
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status DB::Get(std::string_view key, std::string* value) noexcept try {
  if (!value) {
    return {StatusCode::kInvalidArgument, "Get requires an output pointer"};
  }

  std::shared_lock lock(state_->view);
  auto entry = state_->memtable->Get(key);
  for (auto table = state_->tables.rbegin(); !entry && table != state_->tables.rend(); ++table) {
    if (auto status = table->Get(key, &entry); !status.ok()) {
      return status;
    }
  }
  if (!entry || entry->type == RecordType::kDelete) {
    return {StatusCode::kNotFound, "key not found"};
  }
  *value = std::move(entry->value);
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status DB::Scan(std::string_view start, std::string_view end, Pairs* out) noexcept try {
  if (!out || end < start) {
    return {StatusCode::kInvalidArgument, "Scan requires an ordered range and an output pointer"};
  }
  Pairs result;
  if (start != end) {
    std::shared_lock lock(state_->view);
    std::vector<Rows> sources;
    sources.reserve(state_->tables.size() + 1);
    sources.emplace_back();
    for (auto& row : state_->memtable->OrderedEntries()) {
      if (row.first >= start && row.first < end) {
        sources.back().push_back(std::move(row));
      }
    }
    for (const auto& table : state_->tables) {
      sources.emplace_back();
      if (auto status = table.ReadRange(start, end, &sources.back()); !status.ok()) {
        return status;
      }
    }
    MergeRows(sources, &result);
  }
  *out = std::move(result);
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status DB::Write(std::string_view key, std::string_view value, bool deleted) noexcept try {
  auto& state = *state_;
  std::lock_guard writer(state.writer);
  if (state.write_failed) {
    return {StatusCode::kIOError, "reopen the database after a failed write"};
  }
  if (state.sequence == std::numeric_limits<std::uint64_t>::max()) {
    return {StatusCode::kIOError, "WAL sequence space exhausted"};
  }
  if (state.memtable_bytes >= state.options.memtable_bytes) {
    std::unique_lock lock(state.view);
    if (auto status = state.FlushLocked(); !status.ok()) {
      return status;
    }
  }

  const auto sequence = state.sequence + 1;
  const auto type = deleted ? RecordType::kDelete : RecordType::kPut;

  state.write_failed = true;
  auto status = state.wal->Append(key, value, type, sequence);
  if (!status.ok()) {
    if (status.code() == StatusCode::kInvalidArgument) {
      state.write_failed = false;
    }
    return status;
  }
  std::unique_lock lock(state.view);
  state.memtable->Apply(std::string(key), {sequence, type, std::string(value)});
  state.Account(key, value);
  state.sequence = sequence;
  state.write_failed = false;
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

}  // namespace strata
