#include "strata/db.h"

#include <sys/file.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <queue>
#include <shared_mutex>

#include "sstable.h"
#include "thread_pool.h"
#include "version.h"
#include "wal.h"

namespace strata {
namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

template <class Emit>
void MergeRows(const std::vector<Rows>& sources, Emit emit) {
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
      emit(key, entry);
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

namespace {

struct Request {
  std::string_view key;
  std::string_view value;
  RecordType type;
  bool done = false;
  StatusCode result = StatusCode::kOk;
};

}  // namespace

struct DB::State {
  disk::File directory_fd;
  std::filesystem::path directory;
  std::unique_ptr<Wal> wal = std::make_unique<Wal>();
  std::shared_ptr<Version> version = std::make_shared<Version>();
  Manifest manifest;
  Options options;
  std::mutex writer;
  std::shared_mutex view;
  std::condition_variable changed;
  std::deque<Request*> requests;
  std::uint64_t sequence = 0;
  std::uint64_t wal_syncs = 0;
  std::uint64_t compactions = 0;
  std::uint64_t compact_requested = 0;
  std::uint64_t compact_completed = 0;
  std::size_t memtable_bytes = 0;
  StatusCode failure = StatusCode::kOk;
  bool maintenance_running = false;
  bool opened = false;
  std::unique_ptr<ThreadPool> pool;

  static std::size_t FrameBytes(std::string_view key, std::string_view value) {
    return key.size() + value.size() + 25;
  }

  void Account(std::string_view key, std::string_view value) {
    memtable_bytes +=
        std::min(FrameBytes(key, value), std::numeric_limits<std::size_t>::max() - memtable_bytes);
  }

  void Publish(std::shared_ptr<Version> next) {
    std::unique_lock lock(view);
    version = std::move(next);
  }

  void Fail(StatusCode code) {
    if (failure == StatusCode::kOk) {
      failure = code;
    }
    changed.notify_all();
  }

  bool ShouldCompact() const {
    return compact_requested != compact_completed ||
           static_cast<std::size_t>(
               std::count_if(manifest.tables.begin(), manifest.tables.end(), [](const auto& table) {
                 return table.level == 0;
               })) >= options.l0_compaction_trigger;
  }

  void Schedule() {
    if (maintenance_running || failure != StatusCode::kOk ||
        (!version->immutable && !ShouldCompact())) {
      return;
    }
    try {
      if (!pool) {
        pool = std::make_unique<ThreadPool>(options.background_threads);
      }
      maintenance_running = pool->Submit([this] { Maintain(); });
    } catch (...) {
      Fail(StatusCode::kIOError);
    }
    if (!maintenance_running) {
      Fail(StatusCode::kIOError);
    }
  }

  Status Rotate() {
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
      return {StatusCode::kIOError, "WAL sequence space exhausted"};
    }
    auto next = manifest;
    next.next_wal_start = sequence + 1;
    auto new_wal = std::make_unique<Wal>();
    if (auto status = new_wal->Open(directory.string(), next.next_wal_start); !status.ok()) {
      return status;
    }
    if (auto status = new_wal->RequireEmpty(); !status.ok()) {
      return status;
    }
    auto next_version = std::make_shared<Version>(*version);
    next_version->immutable = version->active;
    next_version->active = std::make_shared<Memtable>();

    // If manifest installation fails, either WAL layout may be durable.
    if (auto status = next.Save(directory, directory_fd); !status.ok()) {
      return status;
    }
    manifest = std::move(next);
    wal.swap(new_wal);
    memtable_bytes = 0;
    Publish(std::move(next_version));
    Schedule();
    return Status(failure);
  }

  Status CreateTable(const Rows& rows, std::uint64_t id, std::uint32_t level,
                     std::uint64_t max_sequence, TableMetadata* metadata,
                     std::shared_ptr<Table>* table) const {
    *metadata = {id, level, rows.front().first, rows.back().first};
    auto result = std::make_shared<Table>();
    result->path = TablePath(directory, id);
    if (auto status = BuildTable(result->path, directory_fd, options, rows); !status.ok()) {
      return status;
    }
    if (auto status = result->reader.Open(result->path, *metadata, max_sequence); !status.ok()) {
      return status;
    }
    *table = std::move(result);
    return {};
  }

  Status FlushImmutable(std::unique_lock<std::mutex>& lock) {
    const auto immutable = version->immutable;
    const auto id = manifest.next_file;
    const auto through = manifest.next_wal_start - 1;
    const auto obsolete_wal = directory / Wal::FileName(manifest.wal_start);
    if (id == std::numeric_limits<std::uint64_t>::max()) {
      return {StatusCode::kIOError, "table number space exhausted"};
    }
    lock.unlock();
    TableMetadata metadata{};
    std::shared_ptr<Table> table;
    const auto built = CreateTable(immutable->OrderedEntries(), id, 0, through, &metadata, &table);
    lock.lock();
    if (!built.ok()) {
      return built;
    }

    auto next_version = std::make_shared<Version>(*version);
    next_version->immutable.reset();
    next_version->tables.push_back(std::move(table));
    auto next = manifest;
    next.sequence = through;
    next.wal_start = next.next_wal_start;
    next.next_wal_start = 0;
    next.next_file++;
    next.tables.push_back(std::move(metadata));
    if (auto status = next.Save(directory, directory_fd); !status.ok()) {
      return status;
    }
    manifest = std::move(next);
    Publish(std::move(next_version));
    unlink(obsolete_wal.c_str());
    changed.notify_all();
    return {};
  }

  Status Compact(std::unique_lock<std::mutex>& lock) {
    const auto ticket = compact_requested;
    if (manifest.tables.empty() ||
        (manifest.tables.size() == 1 && manifest.tables.front().level == 1)) {
      compact_completed = ticket;
      changed.notify_all();
      return {};
    }

    const auto old = version;
    const auto id = manifest.next_file;
    const auto through = manifest.sequence;
    if (id == std::numeric_limits<std::uint64_t>::max()) {
      return {StatusCode::kIOError, "table number space exhausted"};
    }
    lock.unlock();
    std::vector<Rows> sources(old->tables.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
      if (auto status = old->tables[i]->reader.ReadAll(&sources[i]); !status.ok()) {
        return status;
      }
    }
    Rows rows;
    MergeRows(sources,
              [&rows](const auto& key, const auto& entry) { rows.emplace_back(key, entry); });
    TableMetadata metadata{};
    std::shared_ptr<Table> table;
    if (!rows.empty()) {
      if (auto status = CreateTable(rows, id, 1, through, &metadata, &table); !status.ok()) {
        return status;
      }
    }
    lock.lock();

    auto next_version = std::make_shared<Version>(*version);
    next_version->tables.clear();
    auto next = manifest;
    next.tables.clear();
    if (table) {
      next_version->tables.push_back(std::move(table));
      next.tables.push_back(std::move(metadata));
      next.next_file++;
    }
    if (auto status = next.Save(directory, directory_fd); !status.ok()) {
      return status;
    }
    manifest = std::move(next);
    Publish(std::move(next_version));
    for (const auto& obsolete : old->tables) {
      obsolete->obsolete.store(true);
    }
    ++compactions;
    compact_completed = ticket;
    changed.notify_all();
    return {};
  }

  void Maintain() {
    std::unique_lock lock(writer);
    try {
      while (failure == StatusCode::kOk && (version->immutable || ShouldCompact())) {
        const auto status = version->immutable ? FlushImmutable(lock) : Compact(lock);
        if (!lock.owns_lock()) {
          lock.lock();
        }
        if (!status.ok()) {
          Fail(status.code());
        }
      }
    } catch (...) {
      if (!lock.owns_lock()) {
        lock.lock();
      }
      Fail(StatusCode::kIOError);
    }
    maintenance_running = false;
    changed.notify_all();
  }

  StatusCode Commit(std::unique_lock<std::mutex>& lock, std::array<Request*, 32>* group,
                    std::size_t* count) {
    if (failure != StatusCode::kOk) {
      return failure;
    }
    if (memtable_bytes >= options.memtable_bytes) {
      changed.wait(lock, [this] { return !version->immutable || failure != StatusCode::kOk; });
      if (failure != StatusCode::kOk) {
        return failure;
      }
      if (memtable_bytes >= options.memtable_bytes) {
        if (auto status = Rotate(); !status.ok()) {
          return status.code();
        }
      }
    }

    const auto limit = options.sync_mode == SyncMode::kBatch ? group->size() : 1;
    auto remaining = options.memtable_bytes - memtable_bytes;
    auto next_sequence = sequence;
    for (auto* request : requests) {
      const auto bytes = FrameBytes(request->key, request->value);
      if (*count != 0 && (*count == limit || bytes > remaining)) {
        break;
      }
      if (next_sequence == std::numeric_limits<std::uint64_t>::max()) {
        break;
      }
      (*group)[(*count)++] = request;
      remaining -= std::min(remaining, bytes);
      if (auto status =
              wal->Append(request->key, request->value, request->type, ++next_sequence, false);
          !status.ok()) {
        return status.code();
      }
    }
    if (*count == 0) {
      return StatusCode::kIOError;
    }
    if (auto status = wal->SyncWrites(); !status.ok()) {
      return status.code();
    }
    ++wal_syncs;

    std::unique_lock publication(view);
    for (std::size_t i = 0; i < *count; ++i) {
      const auto* request = (*group)[i];
      version->active->Apply(std::string(request->key),
                             {++sequence, request->type, std::string(request->value)});
      Account(request->key, request->value);
    }
    return StatusCode::kOk;
  }

  void Lead(std::unique_lock<std::mutex>& lock) {
    std::array<Request*, 32> group{};
    std::size_t count = 0;
    StatusCode result;
    try {
      // A bounded delay gives followers time to enqueue without holding the writer mutex.
      if (options.sync_mode == SyncMode::kBatch && requests.size() < group.size()) {
        changed.wait_for(lock, std::chrono::microseconds(150), [&] {
          return requests.size() >= group.size() || failure != StatusCode::kOk;
        });
      }
      result = Commit(lock, &group, &count);
    } catch (...) {
      result = StatusCode::kIOError;
    }
    if (result != StatusCode::kOk) {
      Fail(result);
      count = requests.size();
    }
    for (std::size_t i = 0; i < count; ++i) {
      auto* request = requests.front();
      requests.pop_front();
      request->result = result;
      request->done = true;
    }
    changed.notify_all();
  }
};

DB::DB(ConstructionKey) : state_(std::make_unique<State>()) {}

DB::~DB() {
  if (state_->opened) {
    (void)Flush();
    if (state_->pool) {
      state_->pool->Shutdown();
    }
  }
}

Status DB::Open(const Options& opts, const std::string& path, std::unique_ptr<DB>* out) noexcept
    try {
  if (!out || path.empty() || path.find('\0') != std::string::npos) {
    return {StatusCode::kInvalidArgument, "Open requires a path and an output pointer"};
  }
  if ((opts.sync_mode != SyncMode::kAlways && opts.sync_mode != SyncMode::kBatch) ||
      opts.memtable_bytes == 0 || opts.block_bytes == 0 || opts.block_bytes > 64 * 1024 * 1024 ||
      opts.bloom_bits_per_key == 0 || opts.bloom_bits_per_key > 64 ||
      opts.background_threads == 0 || opts.l0_compaction_trigger == 0) {
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
    auto table = std::make_shared<Table>();
    table->path = TablePath(state.directory, metadata.id);
    if (auto status = table->reader.Open(table->path, metadata, state.manifest.sequence);
        !status.ok()) {
      return status;
    }
    state.version->tables.push_back(std::move(table));
  }

  const auto recover = [&](Wal& wal, Memtable& memtable, std::uint64_t* last, bool active) {
    return wal.Recover(
        [&](std::string_view key, std::string_view value, RecordType type, std::uint64_t sequence) {
          memtable.Apply(std::string(key), {sequence, type, std::string(value)});
          if (active) {
            state.Account(key, value);
          }
        },
        last, active);
  };
  auto active_start = state.manifest.wal_start;
  if (state.manifest.next_wal_start != 0) {
    Wal immutable_wal;
    if (auto status = immutable_wal.Open(state.directory.string(), active_start, false);
        !status.ok()) {
      return status;
    }
    auto frozen = std::make_shared<Memtable>();
    std::uint64_t last = 0;
    if (auto status = recover(immutable_wal, *frozen, &last, false); !status.ok()) {
      return status;
    }
    active_start = state.manifest.next_wal_start;
    if (last != active_start - 1) {
      return disk::Corruption("immutable WAL does not reach the active WAL boundary");
    }
    state.version->immutable = std::move(frozen);
  }
  if (auto status = state.wal->Open(state.directory.string(), active_start, legacy); !status.ok()) {
    return status;
  }
  if (auto status = recover(*state.wal, *state.version->active, &state.sequence, true);
      !status.ok()) {
    return status;
  }
  {
    std::lock_guard lock(state.writer);
    state.Schedule();
    if (state.failure != StatusCode::kOk) {
      return Status(state.failure);
    }
    state.opened = true;
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
  auto& state = *state_;
  std::unique_lock lock(state.writer);
  const auto target = state.sequence;
  while (state.failure == StatusCode::kOk && state.manifest.sequence < target) {
    if (!state.version->immutable) {
      try {
        if (auto status = state.Rotate(); !status.ok()) {
          state.Fail(status.code());
          return status;
        }
      } catch (...) {
        state.Fail(StatusCode::kIOError);
        return Status(StatusCode::kIOError);
      }
    }
    state.changed.wait(lock, [&] {
      return state.failure != StatusCode::kOk || !state.version->immutable ||
             state.manifest.sequence >= target;
    });
  }
  return Status(state.failure);
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status DB::CompactAll() noexcept try {
  if (auto status = Flush(); !status.ok()) {
    return status;
  }
  auto& state = *state_;
  std::unique_lock lock(state.writer);
  const auto ticket = ++state.compact_requested;
  state.Schedule();
  state.changed.wait(
      lock, [&] { return state.failure != StatusCode::kOk || state.compact_completed >= ticket; });
  return Status(state.failure);
} catch (...) {
  return Status(StatusCode::kIOError);
}

Stats DB::GetStats() const noexcept try {
  std::lock_guard lock(state_->writer);
  return {state_->sequence, state_->wal_syncs, state_->compactions, state_->version->tables.size()};
} catch (...) {
  return {};
}

Status DB::Get(std::string_view key, std::string* value) noexcept try {
  if (!value) {
    return {StatusCode::kInvalidArgument, "Get requires an output pointer"};
  }
  std::shared_ptr<const Version> version;
  std::optional<Entry> entry;
  {
    std::shared_lock lock(state_->view);
    version = state_->version;
    entry = version->active->Get(key);
  }
  if (!entry && version->immutable) {
    entry = version->immutable->Get(key);
  }
  for (auto table = version->tables.rbegin(); !entry && table != version->tables.rend(); ++table) {
    if (auto status = (*table)->reader.Get(key, &entry); !status.ok()) {
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
    std::shared_ptr<const Version> version;
    Rows active;
    {
      std::shared_lock lock(state_->view);
      version = state_->version;
      active = version->active->OrderedEntries();
    }
    std::vector<Rows> sources;
    const auto append_range = [&](Rows rows) {
      auto& source = sources.emplace_back();
      for (auto& row : rows) {
        if (row.first >= start && row.first < end) {
          source.push_back(std::move(row));
        }
      }
    };
    append_range(std::move(active));
    if (version->immutable) {
      append_range(version->immutable->OrderedEntries());
    }
    for (const auto& table : version->tables) {
      sources.emplace_back();
      if (auto status = table->reader.ReadRange(start, end, &sources.back()); !status.ok()) {
        return status;
      }
    }
    MergeRows(sources, [&result](const auto& key, const auto& entry) {
      result.emplace_back(key, entry.value);
    });
  }
  *out = std::move(result);
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status DB::Write(std::string_view key, std::string_view value, bool deleted) noexcept try {
  if (!Wal::ValidSize(key, value)) {
    return {StatusCode::kInvalidArgument, "key and value exceed WAL record size limit"};
  }
  auto& state = *state_;
  Request request{key, value, deleted ? RecordType::kDelete : RecordType::kPut};
  std::unique_lock lock(state.writer);
  if (state.failure != StatusCode::kOk) {
    return Status(state.failure);
  }
  state.requests.push_back(&request);
  state.changed.notify_all();
  state.changed.wait(lock, [&] {
    return request.done || (!state.requests.empty() && state.requests.front() == &request);
  });
  if (!request.done) {
    state.Lead(lock);
  }
  return Status(request.result);
} catch (...) {
  return Status(StatusCode::kIOError);
}

}  // namespace strata
