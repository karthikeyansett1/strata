#include "wal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <tuple>
#include <vector>

#include "temp_directory.h"

namespace strata {
namespace {

using ReplayedRecord = std::tuple<std::string, std::string, RecordType, std::uint64_t>;

class WalTest : public testing::Test {
 protected:
  void SetUp() override {
    {
      Wal wal;
      ASSERT_TRUE(wal.Open(directory_.path).ok());
      ASSERT_TRUE(wal.Append("first", "retained", RecordType::kPut, 1).ok());
      first_size_ = std::filesystem::file_size(path_);
      ASSERT_TRUE(wal.Append("last", std::string("binary\0payload", 14), RecordType::kPut, 2).ok());
    }
    original_ = ReadFile();
    ASSERT_GT(original_.size(), first_size_);
  }

  std::string ReadFile() const {
    std::ifstream input(path_, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void ReplaceFile(std::string_view bytes) const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size());
    output.close();
    ASSERT_TRUE(output);
  }

  Status Replay(Wal& wal, std::vector<ReplayedRecord>* records, std::uint64_t* sequence) const {
    return wal.Recover(
        [&](std::string_view key, std::string_view value, RecordType type, std::uint64_t seq) {
          records->emplace_back(std::string(key), std::string(value), type, seq);
        },
        sequence);
  }

  test::TempDirectory directory_;
  const std::filesystem::path path_ = std::filesystem::path(directory_.path) / Wal::kFileName;
  std::uintmax_t first_size_ = 0;
  std::string original_;
};

TEST_F(WalTest, EveryTruncationOfFinalFrameIsRepairedBeforeAppend) {
  for (std::size_t cut = first_size_; cut < original_.size(); ++cut) {
    SCOPED_TRACE(cut);
    ASSERT_NO_FATAL_FAILURE(ReplaceFile(std::string_view(original_).substr(0, cut)));
    {
      Wal wal;
      ASSERT_TRUE(wal.Open(directory_.path).ok());
      std::vector<ReplayedRecord> records;
      std::uint64_t sequence = 0;
      ASSERT_TRUE(Replay(wal, &records, &sequence).ok());
      ASSERT_EQ(sequence, 1u);
      ASSERT_EQ(records, (std::vector<ReplayedRecord>{{"first", "retained", RecordType::kPut, 1}}));
      ASSERT_EQ(std::filesystem::file_size(path_), first_size_);
      ASSERT_TRUE(wal.Append("first", "", RecordType::kDelete, sequence + 1).ok());
    }
    {
      Wal wal;
      ASSERT_TRUE(wal.Open(directory_.path).ok());
      std::vector<ReplayedRecord> records;
      std::uint64_t sequence = 0;
      ASSERT_TRUE(Replay(wal, &records, &sequence).ok());
      ASSERT_EQ(sequence, 2u);
      ASSERT_EQ(records.size(), 2u);
      EXPECT_EQ(records[1], (ReplayedRecord{"first", "", RecordType::kDelete, 2}));
    }
  }
}

TEST_F(WalTest, RejectsInteriorChecksumOrLengthDamageWithoutTruncating) {
  for (auto damaged : {std::size_t{0}, std::size_t{4}, std::size_t{7}, first_size_ - 1}) {
    auto bytes = original_;
    bytes[damaged] ^= static_cast<char>(0x80);
    ASSERT_NO_FATAL_FAILURE(ReplaceFile(bytes));

    Wal wal;
    ASSERT_TRUE(wal.Open(directory_.path).ok());
    std::vector<ReplayedRecord> records;
    std::uint64_t sequence = 0;
    EXPECT_EQ(Replay(wal, &records, &sequence).code(), StatusCode::kCorruption) << damaged;
    EXPECT_EQ(ReadFile(), bytes);
  }
}

TEST_F(WalTest, ValidLaterFrameSurvivesAnExactlyFittingCorruptedLength) {
  auto bytes = original_;
  bytes[4] = static_cast<char>(bytes.size() - 8);
  ASSERT_NO_FATAL_FAILURE(ReplaceFile(bytes));
  Wal wal;
  ASSERT_TRUE(wal.Open(directory_.path).ok());
  std::vector<ReplayedRecord> records;
  std::uint64_t sequence = 0;
  EXPECT_EQ(Replay(wal, &records, &sequence).code(), StatusCode::kCorruption);
  EXPECT_EQ(ReadFile(), bytes);
}

TEST_F(WalTest, TruncatedValueCanContainBytesThatLookLikeACompleteFrame) {
  const auto embedded =
      std::string_view(original_).substr(first_size_);  // A checksum-valid sequence-2 frame.
  ASSERT_NO_FATAL_FAILURE(ReplaceFile(""));
  {
    Wal wal;
    ASSERT_TRUE(wal.Open(directory_.path).ok());
    ASSERT_TRUE(
        wal.Append("", std::string(embedded) + std::string(100, 'x'), RecordType::kPut, 1).ok());
  }
  std::filesystem::resize_file(path_, 25 + embedded.size());

  Wal wal;
  ASSERT_TRUE(wal.Open(directory_.path).ok());
  std::vector<ReplayedRecord> records;
  std::uint64_t sequence = 0;
  ASSERT_TRUE(Replay(wal, &records, &sequence).ok());
  EXPECT_EQ(sequence, 0u);
  EXPECT_TRUE(records.empty());
  EXPECT_EQ(std::filesystem::file_size(path_), 0u);
}

TEST_F(WalTest, DiscardsBadFinalChecksum) {
  auto bytes = original_;
  bytes[first_size_] ^= 1;
  ASSERT_NO_FATAL_FAILURE(ReplaceFile(bytes));
  Wal wal;
  ASSERT_TRUE(wal.Open(directory_.path).ok());
  std::vector<ReplayedRecord> records;
  std::uint64_t sequence = 0;
  EXPECT_TRUE(Replay(wal, &records, &sequence).ok());
  EXPECT_EQ(sequence, 1u);
  EXPECT_EQ(ReadFile(), original_.substr(0, first_size_));
}

}  // namespace
}  // namespace strata
