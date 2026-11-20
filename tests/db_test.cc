#include "strata/db.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "temp_directory.h"

namespace strata {
namespace {

TEST(DB, SupportsBinaryKeysEmptyValuesAndRevival) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path + "/db", &db).ok());
  const std::string key("a\0b", 3);
  const std::string binary_value("x\0y", 3);
  std::string value = "unchanged";

  EXPECT_EQ(db->Get(key, &value).code(), StatusCode::kNotFound);
  EXPECT_EQ(value, "unchanged");
  EXPECT_EQ(db->Get(key, nullptr).code(), StatusCode::kInvalidArgument);
  ASSERT_TRUE(db->Put(key, binary_value).ok());
  ASSERT_TRUE(db->Get(key, &value).ok());
  EXPECT_EQ(value, binary_value);
  ASSERT_TRUE(db->Delete(key).ok());
  EXPECT_EQ(db->Get(key, &value).code(), StatusCode::kNotFound);
  ASSERT_TRUE(db->Put(key, "").ok());
  ASSERT_TRUE(db->Put("", "empty key").ok());
  db.reset();

  ASSERT_TRUE(DB::Open({}, directory.path + "/db", &db).ok());
  ASSERT_TRUE(db->Get(key, &value).ok());
  EXPECT_TRUE(value.empty());
  ASSERT_TRUE(db->Get("", &value).ok());
  EXPECT_EQ(value, "empty key");
}

TEST(DB, LocksWalUntilTheHandleCloses) {
  test::TempDirectory directory;
  std::unique_ptr<DB> first;
  std::unique_ptr<DB> second;
  ASSERT_TRUE(DB::Open({}, directory.path, &first).ok());
  EXPECT_EQ(DB::Open({}, directory.path, &second).code(), StatusCode::kBusy);
  EXPECT_FALSE(second);
  first.reset();
  EXPECT_TRUE(DB::Open({}, directory.path, &second).ok());
}

TEST(DB, ConcurrentWritersAndReadersSeeCompleteValues) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  std::vector<std::jthread> threads;

  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&, thread] {
      const auto key = std::to_string(thread);
      for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(db->Put(key, std::to_string(i)).ok());
        std::string value;
        ASSERT_TRUE(db->Get(key, &value).ok());
        EXPECT_EQ(value, std::to_string(i));

        const auto neighboring_key = std::to_string((thread + 1) % 4);
        const auto status = db->Get(neighboring_key, &value);
        if (status.ok()) {
          EXPECT_EQ(value.find_first_not_of("0123456789"), std::string::npos);
          EXPECT_FALSE(value.empty());
        } else {
          EXPECT_EQ(status.code(), StatusCode::kNotFound);
        }
      }
    });
  }

  threads.clear();
  db.reset();
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  for (int thread = 0; thread < 4; ++thread) {
    std::string value;
    ASSERT_TRUE(db->Get(std::to_string(thread), &value).ok());
    EXPECT_EQ(value, "39");
  }
}

}  // namespace
}  // namespace strata
