#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <charconv>
#include <map>
#include <thread>
#include <vector>

#include "strata/db.h"
#include "temp_directory.h"

namespace strata {
namespace {

TEST(Concurrency, EightWritersAndEightReadersRunThroughFlushAndCompaction) {
  constexpr int kThreads = 8;
  constexpr int kOperations = 100'000;
  constexpr int kKeys = 8;
  using Model = std::map<std::string, std::string>;
  test::TempDirectory directory;

  Options options;
  options.sync_mode = SyncMode::kBatch;
  options.memtable_bytes = 128 * 1024;
  options.l0_compaction_trigger = 3;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path, &db).ok());

  std::array<std::array<std::string, kKeys>, kThreads> keys;
  std::array<Model, kThreads> models;
  for (int worker = 0; worker < kThreads; ++worker) {
    for (int slot = 0; slot < kKeys; ++slot) {
      keys[worker][slot] = std::to_string(worker) + "/" + std::to_string(slot);
    }
  }

  std::barrier start(kThreads * 2);
  std::vector<std::jthread> threads;
  for (int worker = 0; worker < kThreads; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (int op = 0; op < kOperations; ++op) {
        const auto& key = keys[worker][op % kKeys];
        if (op % 7 == 0) {
          ASSERT_TRUE(db->Delete(key).ok()) << worker << ' ' << op;
          models[worker].erase(key);
        } else {
          const auto value = std::to_string(op);
          ASSERT_TRUE(db->Put(key, value).ok()) << worker << ' ' << op;
          models[worker][key] = value;
        }
      }
    });
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      std::array<int, kKeys> last{};
      for (int op = 0; op < kOperations; ++op) {
        const int slot = op % kKeys;
        std::string value;
        const auto status = db->Get(keys[worker][slot], &value);
        if (status.code() == StatusCode::kNotFound) {
          continue;
        }
        ASSERT_TRUE(status.ok()) << static_cast<int>(status.code());
        int generation = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), generation);
        ASSERT_EQ(parsed.ec, std::errc{});
        ASSERT_EQ(parsed.ptr, value.data() + value.size());
        ASSERT_GE(generation, last[slot]);
        ASSERT_LT(generation, kOperations);
        ASSERT_EQ(generation % kKeys, slot);
        ASSERT_NE(generation % 7, 0);
        last[slot] = generation;
      }
    });
  }
  threads.clear();

  const auto stats = db->GetStats();
  EXPECT_EQ(stats.sequence, kThreads * kOperations);
  EXPECT_GT(stats.compactions, 0u);
  EXPECT_LT(stats.wal_syncs, stats.sequence);
  ASSERT_TRUE(db->CompactAll().ok());
  db.reset();
  ASSERT_TRUE(DB::Open(options, directory.path, &db).ok());

  Model combined;
  for (const auto& model : models) {
    combined.insert(model.begin(), model.end());
  }
  std::vector<std::pair<std::string, std::string>> rows;
  ASSERT_TRUE(db->Scan("", "z", &rows).ok());
  EXPECT_EQ(rows, (decltype(rows)(combined.begin(), combined.end())));
  EXPECT_EQ(db->GetStats().sequence, kThreads * kOperations);
}

}  // namespace
}  // namespace strata
