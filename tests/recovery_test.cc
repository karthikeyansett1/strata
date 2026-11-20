#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <string>

#include "strata/db.h"
#include "temp_directory.h"
#include "wal.h"

namespace strata {
namespace {

constexpr int kKeys = 10'000;

std::string Key(int i) { return "key-" + std::to_string(i); }
std::string Value(int i) { return "value-" + std::to_string(i); }

void ExpectChildSuccess(pid_t child) {
  int child_status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &child_status, 0);
  } while (waited < 0 && errno == EINTR);
  ASSERT_EQ(waited, child);
  ASSERT_TRUE(WIFEXITED(child_status));
  ASSERT_EQ(WEXITSTATUS(child_status), 0);
}

TEST(Recovery, RestoresTenThousandKeysAndDeletesAfterExitWithoutDestructors) {
  test::TempDirectory directory;
  const auto db_path = directory.path + "/db";
  const pid_t writer = fork();
  ASSERT_GE(writer, 0);
  if (writer == 0) {
    std::unique_ptr<DB> db;
    if (!DB::Open({}, db_path, &db).ok()) {
      _exit(10);
    }
    for (int i = 0; i < kKeys; ++i) {
      if (!db->Put(Key(i), Value(i)).ok()) {
        _exit(11);
      }
    }
    for (int i = 0; i < kKeys; i += 7) {
      if (!db->Delete(Key(i)).ok()) {
        _exit(12);
      }
    }
    _exit(0);
  }

  ASSERT_NO_FATAL_FAILURE(ExpectChildSuccess(writer));

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, db_path, &db).ok());
  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    const auto status = db->Get(Key(i), &value);
    if (i % 7 == 0) {
      EXPECT_EQ(status.code(), StatusCode::kNotFound) << i;
    } else {
      ASSERT_TRUE(status.ok()) << i;
      EXPECT_EQ(value, Value(i)) << i;
    }
  }
  ASSERT_TRUE(db->Put(Key(0), "after recovery").ok());
  db.reset();

  ASSERT_TRUE(DB::Open({}, db_path, &db).ok());
  ASSERT_TRUE(db->Get(Key(0), &value).ok());
  EXPECT_EQ(value, "after recovery");
}

TEST(Recovery, RepairsPartialAppendAfterWriteError) {
  test::TempDirectory directory;
  const auto wal_path = std::filesystem::path(directory.path) / Wal::kFileName;
  const pid_t writer = fork();
  ASSERT_GE(writer, 0);
  if (writer == 0) {
    std::unique_ptr<DB> db;
    Options options;
    options.sync_mode = SyncMode::kBatch;
    if (!DB::Open(options, directory.path, &db).ok() || !db->Put("key", "previous").ok()) {
      _exit(20);
    }

    const auto size = std::filesystem::file_size(wal_path);
    struct rlimit limit {};
    if (getrlimit(RLIMIT_FSIZE, &limit) != 0 || std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) {
      _exit(21);
    }
    limit.rlim_cur = size + 12;
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      _exit(22);
    }
    if (db->Put("key", "failed").code() != StatusCode::kIOError ||
        std::filesystem::file_size(wal_path) != limit.rlim_cur) {
      _exit(23);
    }
    std::string value;
    if (!db->Get("key", &value).ok() || value != "previous" ||
        db->Put("other", "blocked").code() != StatusCode::kIOError) {
      _exit(24);
    }
    _exit(0);
  }

  ASSERT_NO_FATAL_FAILURE(ExpectChildSuccess(writer));
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get("key", &value).ok());
  EXPECT_EQ(value, "previous");
  EXPECT_EQ(db->Get("other", &value).code(), StatusCode::kNotFound);
  ASSERT_TRUE(db->Put("key", "next").ok());
  db.reset();

  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  ASSERT_TRUE(db->Get("key", &value).ok());
  EXPECT_EQ(value, "next");
}

}  // namespace
}  // namespace strata
