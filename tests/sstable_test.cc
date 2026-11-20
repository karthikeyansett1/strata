#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>

#include "bloom.h"
#include "strata/db.h"
#include "temp_directory.h"

namespace strata {
namespace {

TEST(Bloom, FalsePositiveRateIsWithinTwiceTheoretical) {
  constexpr int kKeys = 10'000;
  constexpr int kQueries = 100'000;
  Bloom bloom(kKeys, 10);
  for (int i = 0; i < kKeys; ++i) {
    bloom.Insert("in-" + std::to_string(i));
  }
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(bloom.MayContain("in-" + std::to_string(i)));
  }
  int positives = 0;
  for (int i = 0; i < kQueries; ++i) {
    positives += bloom.MayContain("out-" + std::to_string(i));
  }
  const double rate = static_cast<double>(positives) / kQueries;
  const auto probes = bloom.probes();
  const double theoretical = std::pow(1 - std::exp(-probes / 10.0), probes);
  std::cout << "Bloom false-positive rate: " << rate << " (" << positives << '/' << kQueries
            << "), theoretical: " << theoretical << '\n';
  EXPECT_LE(rate, 2 * theoretical);
}

TEST(SSTables, ShadowsAcrossTablesAndRecoversTheActiveWal) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  ASSERT_TRUE(db->Put("key", "old").ok());
  ASSERT_TRUE(db->Put("kept", "kept").ok());
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_FALSE(std::filesystem::exists(directory.path + "/wal-1.log"));
  ASSERT_TRUE(db->Put("key", "new").ok());
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_TRUE(db->Delete("key").ok());
  std::string value;
  EXPECT_EQ(db->Get("key", &value).code(), StatusCode::kNotFound);
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_TRUE(db->Put("wal-only", "live").ok());
  db.reset();

  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  EXPECT_EQ(db->Get("key", &value).code(), StatusCode::kNotFound);
  ASSERT_TRUE(db->Get("kept", &value).ok());
  EXPECT_EQ(value, "kept");
  ASSERT_TRUE(db->Get("wal-only", &value).ok());
  EXPECT_EQ(value, "live");
  ASSERT_TRUE(db->Put("key", "revived").ok());
  ASSERT_TRUE(db->Get("key", &value).ok());
  EXPECT_EQ(value, "revived");
  std::vector<std::pair<std::string, std::string>> rows;
  ASSERT_TRUE(db->Scan("key", "z", &rows).ok());
  EXPECT_EQ(rows, (decltype(rows){{"key", "revived"}, {"wal-only", "live"}}));
}

TEST(SSTables, RandomizedScansMatchOrderedMapThroughAutomaticRotation) {
  test::TempDirectory directory;
  Options options;
  options.block_bytes = 85;
  options.memtable_bytes = 400;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path, &db).ok());
  std::map<std::string, std::string> expected;
  std::mt19937 random(42);
  const auto key = [](int i) { return std::to_string(100 + i); };

  for (int i = 0; i < 450; ++i) {
    const auto name = key(random() % 50);
    if (random() % 3 == 0) {
      ASSERT_TRUE(db->Delete(name).ok());
      expected.erase(name);
    } else {
      auto value = "value-" + std::to_string(i);
      ASSERT_TRUE(db->Put(name, value).ok());
      expected[name] = std::move(value);
    }
    if (i % 50 == 0) {
      db.reset();
      ASSERT_TRUE(DB::Open(options, directory.path, &db).ok());
    }
    if (i % 7 == 0) {
      const auto a = random() % 51;
      const auto b = random() % 51;
      const auto start = key(std::min(a, b));
      const auto end = key(std::max(a, b));
      std::vector<std::pair<std::string, std::string>> rows;
      ASSERT_TRUE(db->Scan(start, end, &rows).ok());
      EXPECT_EQ(rows, (decltype(rows)(expected.lower_bound(start), expected.lower_bound(end))));
    }
  }
  EXPECT_TRUE(std::filesystem::exists(directory.path + "/sst-2.sst"));
}

void FlipByte(const std::filesystem::path& path, std::uintmax_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file);
  file.seekg(offset);
  char byte;
  ASSERT_TRUE(file.get(byte));
  file.seekp(offset);
  ASSERT_TRUE(file.put(byte ^ 1));
  file.close();
  ASSERT_TRUE(file);
}

TEST(SSTables, ReportsBlockFooterAndManifestCorruption) {
  enum class Damage { kBlock, kFooter, kManifest };
  for (auto damage : {Damage::kBlock, Damage::kFooter, Damage::kManifest}) {
    test::TempDirectory directory;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
    ASSERT_TRUE(db->Put("key", "value").ok());
    ASSERT_TRUE(db->Flush().ok());
    db.reset();
    const auto table = directory.path + "/sst-1.sst";
    if (damage == Damage::kBlock) {
      ASSERT_NO_FATAL_FAILURE(FlipByte(table, 0));
      ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
      std::string value;
      EXPECT_EQ(db->Get("key", &value).code(), StatusCode::kCorruption);
      std::vector<std::pair<std::string, std::string>> rows;
      EXPECT_EQ(db->Scan("", "z", &rows).code(), StatusCode::kCorruption);
    } else {
      if (damage == Damage::kFooter) {
        ASSERT_NO_FATAL_FAILURE(FlipByte(table, std::filesystem::file_size(table) - 1));
      } else {
        ASSERT_NO_FATAL_FAILURE(FlipByte(directory.path + "/MANIFEST", 0));
      }
      EXPECT_EQ(DB::Open({}, directory.path, &db).code(), StatusCode::kCorruption);
    }
  }
}

TEST(SSTables, RotatingWalKeepsReadersAndTheDirectoryLockConsistent) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  ASSERT_TRUE(db->Put("key", "value").ok());

  std::atomic<int> reads = 0;
  std::jthread reader([&](std::stop_token stop) {
    do {
      std::vector<std::pair<std::string, std::string>> rows;
      EXPECT_TRUE(db->Scan("", "z", &rows).ok());
      EXPECT_EQ(rows, (decltype(rows){{"key", "value"}}));
      ++reads;
      std::this_thread::yield();
    } while (!stop.stop_requested());
  });
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(db->Put("key", "value").ok());
    ASSERT_TRUE(db->Flush().ok());
    std::unique_ptr<DB> other;
    EXPECT_EQ(DB::Open({}, directory.path, &other).code(), StatusCode::kBusy);
  }
  reader.request_stop();
  reader.join();
  EXPECT_GT(reads, 0);
}

TEST(SSTables, IgnoresOrphansBeforeManifestAndRejectsMissingCommittedWal) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  ASSERT_TRUE(db->Put("key", "acknowledged").ok());
  db.reset();

  std::ofstream(directory.path + "/sst-1.sst") << "orphan";
  std::ofstream(directory.path + "/MANIFEST.tmp") << "incomplete";
  std::ofstream(directory.path + "/wal-2.log").close();
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get("key", &value).ok());
  EXPECT_EQ(value, "acknowledged");
  ASSERT_TRUE(db->Flush().ok());
  db.reset();

  // The committed manifest must win even if old-WAL cleanup never completed.
  std::ofstream(directory.path + "/wal-1.log") << "obsolete";
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  ASSERT_TRUE(db->Get("key", &value).ok());
  EXPECT_EQ(value, "acknowledged");
  db.reset();
  ASSERT_TRUE(std::filesystem::remove(directory.path + "/wal-2.log"));
  EXPECT_EQ(DB::Open({}, directory.path, &db).code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace strata
