#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "manifest.h"
#include "strata/db.h"
#include "temp_directory.h"
#include "version.h"
#include "wal.h"

namespace strata {
namespace {

TEST(Compaction, KeepsNewestRowsAndDropsTombstonesAfterMergingAllTables) {
  test::TempDirectory directory;
  Options options;
  options.l0_compaction_trigger = 100;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path, &db).ok());
  ASSERT_TRUE(db->Put("removed", "old").ok());
  const std::string binary_key("\xff\xff", 2);
  ASSERT_TRUE(db->Put(binary_key, "old").ok());
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_TRUE(db->Put(binary_key, "new").ok());
  ASSERT_TRUE(db->Delete("removed").ok());
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_EQ(db->GetStats().sstable_count, 2u);
  ASSERT_TRUE(db->CompactAll().ok());
  EXPECT_EQ(db->GetStats().compactions, 1u);
  EXPECT_EQ(db->GetStats().sstable_count, 1u);
  EXPECT_FALSE(std::filesystem::exists(TablePath(directory.path, 1)));
  EXPECT_FALSE(std::filesystem::exists(TablePath(directory.path, 2)));
  db.reset();

  Manifest manifest;
  ASSERT_TRUE(manifest.Load(directory.path).ok());
  ASSERT_EQ(manifest.tables.size(), 1u);
  EXPECT_EQ(manifest.tables[0].level, 1u);
  SSTable table;
  ASSERT_TRUE(table
                  .Open(TablePath(directory.path, manifest.tables[0].id), manifest.tables[0],
                        manifest.sequence)
                  .ok());
  Rows rows;
  ASSERT_TRUE(table.ReadAll(&rows).ok());
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].first, binary_key);
  EXPECT_EQ(rows[0].second.value, "new");

  ASSERT_TRUE(DB::Open(options, directory.path, &db).ok());
  std::string value;
  EXPECT_EQ(db->Get("removed", &value).code(), StatusCode::kNotFound);
  ASSERT_TRUE(db->Get(binary_key, &value).ok());
  EXPECT_EQ(value, "new");
  ASSERT_TRUE(db->Delete(binary_key).ok());
  ASSERT_TRUE(db->CompactAll().ok());
  EXPECT_EQ(db->GetStats().sstable_count, 0u);
  db.reset();
  ASSERT_TRUE(DB::Open(options, directory.path, &db).ok());
  EXPECT_EQ(db->Get(binary_key, &value).code(), StatusCode::kNotFound);
}

TEST(Compaction, ObsoleteTableIsPinnedByEveryVersionThatReferencesIt) {
  test::TempDirectory directory;
  auto table = std::make_shared<Table>();
  table->path = TablePath(directory.path, 1);
  std::ofstream(table->path).close();
  auto first = std::make_shared<Version>();
  first->tables.push_back(table);
  std::shared_ptr<const Version> second = std::make_shared<Version>(*first);
  table->obsolete.store(true);
  table.reset();
  first.reset();
  EXPECT_TRUE(std::filesystem::exists(TablePath(directory.path, 1)));
  second.reset();
  EXPECT_FALSE(std::filesystem::exists(TablePath(directory.path, 1)));
}

TEST(Compaction, ReplaysBothWalsAfterAnInterruptedBackgroundFlush) {
  test::TempDirectory directory;
  {
    Wal old;
    ASSERT_TRUE(old.Open(directory.path).ok());
    ASSERT_TRUE(old.Append("same", "old", RecordType::kPut, 1).ok());
    ASSERT_TRUE(old.Append("old-only", "kept", RecordType::kPut, 2).ok());
    Wal active;
    ASSERT_TRUE(active.Open(directory.path, 3).ok());
    ASSERT_TRUE(active.Append("same", "", RecordType::kDelete, 3).ok());
    ASSERT_TRUE(active.Append("active-only", "live", RecordType::kPut, 4).ok());
    Manifest rotated;
    rotated.next_wal_start = 3;
    const disk::File fd(open(directory.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    ASSERT_TRUE(fd.valid());
    ASSERT_TRUE(rotated.Save(directory.path, fd).ok());
  }

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  std::string value;
  EXPECT_EQ(db->Get("same", &value).code(), StatusCode::kNotFound);
  ASSERT_TRUE(db->Get("old-only", &value).ok());
  EXPECT_EQ(value, "kept");
  ASSERT_TRUE(db->Get("active-only", &value).ok());
  EXPECT_EQ(value, "live");
  ASSERT_TRUE(db->Flush().ok());
  EXPECT_EQ(db->GetStats().sequence, 4u);
  EXPECT_FALSE(std::filesystem::exists(directory.path + "/wal-1.log"));
  EXPECT_FALSE(std::filesystem::exists(directory.path + "/wal-3.log"));
}

TEST(Compaction, OpensAndUpgradesAPhaseTwoManifest) {
  test::TempDirectory directory;
  const disk::File fd(open(directory.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  ASSERT_TRUE(fd.valid());
  ASSERT_TRUE(BuildTable(TablePath(directory.path, 1), fd, {},
                         Rows{{"old", {1, RecordType::kPut, "value"}}})
                  .ok());
  {
    Wal wal;
    ASSERT_TRUE(wal.Open(directory.path, 2).ok());
    ASSERT_TRUE(wal.Append("new", "wal", RecordType::kPut, 2).ok());
  }
  std::string manifest;
  disk::Add(&manifest, std::uint32_t{0x314d5453});
  disk::Add(&manifest, std::uint64_t{1});  // Flushed sequence.
  disk::Add(&manifest, std::uint64_t{2});  // Next table.
  disk::Add(&manifest, std::uint64_t{2});  // Active WAL.
  disk::Add(&manifest, std::uint64_t{1});  // One table.
  disk::Add(&manifest, std::uint64_t{1});
  disk::Add(&manifest, std::uint32_t{0});
  disk::AddString(&manifest, "old");
  disk::AddString(&manifest, "old");
  disk::Seal(&manifest);
  std::ofstream(directory.path + "/MANIFEST", std::ios::binary) << manifest;

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get("old", &value).ok());
  EXPECT_EQ(value, "value");
  ASSERT_TRUE(db->Get("new", &value).ok());
  EXPECT_EQ(value, "wal");
  ASSERT_TRUE(db->CompactAll().ok());
  db.reset();

  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  ASSERT_TRUE(db->Get("old", &value).ok());
  EXPECT_EQ(value, "value");
  ASSERT_TRUE(db->Get("new", &value).ok());
  EXPECT_EQ(value, "wal");
}

}  // namespace
}  // namespace strata
