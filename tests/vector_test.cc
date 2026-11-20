#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <limits>
#include <random>
#include <set>

#include "strata/vector_store.h"
#include "temp_directory.h"
#include "vector/distance.h"
#include "vector/hnsw_index.h"

namespace strata {
namespace {

TEST(VectorDistance, RuntimeKernelsMatchScalarOnUnalignedTails) {
  std::mt19937 rng(4831);
  std::uniform_real_distribution<float> generate(-1, 1);
  std::vector<float> a(400);
  std::vector<float> b(400);
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i] = generate(rng);
    b[i] = generate(rng);
  }
  for (std::size_t length : {1, 4, 7, 8, 9, 31, 384, 387}) {
    const auto left = std::span<const float>(a).subspan(1, length);
    const auto right = std::span<const float>(b).subspan(2, length);
    EXPECT_NEAR(vector::L2Squared(left, right), vector::ScalarL2Squared(left, right), 2e-4);
    EXPECT_NEAR(vector::Dot(left, right), vector::ScalarDot(left, right), 1e-4);
    const auto unit_a = vector::Prepare(left, Metric::kCosine);
    const auto unit_b = vector::Prepare(right, Metric::kCosine);
    const auto scalar = std::clamp(1.0f - vector::ScalarDot(unit_a, unit_b), 0.0f, 2.0f);
    EXPECT_NEAR(vector::Distance(unit_a, unit_b, Metric::kCosine), scalar, 1e-5);
  }
  EXPECT_FLOAT_EQ(vector::L2Squared(std::vector<float>{3, 4}, std::vector<float>{0, 0}), 25);
  EXPECT_FLOAT_EQ(
      vector::Distance(std::vector<float>{0, 0}, std::vector<float>{1, 0}, Metric::kCosine), 1);
}

TEST(Hnsw, FindsNearestNeighborsAgainstFlatGroundTruth) {
  const VectorOptions options{12, 80, 64, Metric::kL2};
  vector::HnswIndex graph(options);
  vector::FlatIndex flat(options.metric);
  std::mt19937 rng(128);
  std::uniform_real_distribution<float> generate(-1, 1);
  for (int i = 0; i < 600; ++i) {
    std::vector<float> values(19);
    for (auto& value : values) {
      value = generate(rng);
    }
    flat.Insert(values);
    graph.Insert(values);
  }
  int matched = 0;
  for (int i = 0; i < 24; ++i) {
    std::vector<float> query(19);
    for (auto& value : query) {
      value = generate(rng);
    }
    const auto expected = flat.Search(query, 8);
    const auto actual = graph.Search(query, 8);
    ASSERT_EQ(actual.size(), expected.size());
    EXPECT_TRUE(std::is_sorted(actual.begin(), actual.end()));
    for (const auto& hit : actual) {
      matched += std::find(expected.begin(), expected.end(), hit) != expected.end();
    }
  }
  EXPECT_GE(matched, 173);
}

TEST(Hnsw, ReturnsKDistinctCandidatesForIdenticalVectors) {
  const VectorOptions options{2, 10, 1, Metric::kCosine};
  vector::HnswIndex graph(options);
  const std::vector<float> repeated{1, 0};
  for (int i = 0; i < 200; ++i) {
    graph.Insert(repeated);
  }
  const auto hits = graph.Search(repeated, 100, 1);
  ASSERT_EQ(hits.size(), 100u);
  std::set<std::uint32_t> ids;
  for (const auto& [distance, id] : hits) {
    EXPECT_FLOAT_EQ(distance, 0);
    ids.insert(id);
  }
  EXPECT_EQ(ids.size(), hits.size());
}

TEST(VectorStore, PersistsAndRebuildsWithoutCollidingWithBinaryKvKeys) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  std::unique_ptr<VectorStore> vectors;
  const VectorOptions l2{12, 80, 24, Metric::kL2};
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  ASSERT_TRUE(VectorStore::Open({}, l2, directory.path, &vectors).ok());
  const std::string binary_id("n\0\xff", 3);
  const auto key = "v/" + binary_id;
  ASSERT_TRUE(db->Put(key, "raw KV").ok());
  ASSERT_TRUE(vectors->Put("alpha", std::vector<float>{4, 5}).ok());
  ASSERT_TRUE(vectors->Put(binary_id, std::vector<float>{3, 4}).ok());
  ASSERT_TRUE(vectors->Put("alpha", std::vector<float>{1, 2}).ok());
  EXPECT_EQ(vectors->Put("wrong", std::vector<float>{1}).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(
      vectors->Put("bad", std::vector<float>{0, std::numeric_limits<float>::infinity()}).code(),
      StatusCode::kInvalidArgument);
  vectors.reset();
  ASSERT_TRUE(VectorStore::Open({}, l2, directory.path, &vectors).ok());

  std::string value;
  ASSERT_TRUE(db->Get(key, &value).ok());
  EXPECT_EQ(value, "raw KV");
  std::vector<std::pair<std::string, std::string>> rows;
  ASSERT_TRUE(db->Scan("v/", "v0", &rows).ok());
  EXPECT_EQ(rows, (decltype(rows){{key, "raw KV"}}));
  std::vector<VectorHit> hits;
  ASSERT_TRUE(vectors->Search(std::vector<float>{0, 0}, 10, &hits).ok());
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].id, "alpha");
  EXPECT_FLOAT_EQ(hits[0].distance, std::sqrt(5.0f));
  EXPECT_EQ(hits[1].id, binary_id);
  EXPECT_FLOAT_EQ(hits[1].distance, 5);
  EXPECT_EQ(vectors->Search(std::vector<float>{1}, 1, &hits).code(), StatusCode::kInvalidArgument);
}

TEST(VectorStore, RebuildsFromAcknowledgedWalAfterProcessExit) {
  test::TempDirectory directory;
  const auto writer = fork();
  ASSERT_GE(writer, 0);
  if (writer == 0) {
    std::unique_ptr<VectorStore> vectors;
    if (!VectorStore::Open({}, {}, directory.path, &vectors).ok() ||
        !vectors->Put("wal", std::vector<float>{0, 1}).ok()) {
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  ASSERT_EQ(waitpid(writer, &status, 0), writer);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  std::unique_ptr<VectorStore> vectors;
  ASSERT_TRUE(VectorStore::Open({}, {}, directory.path, &vectors).ok());
  std::vector<VectorHit> hits;
  ASSERT_TRUE(vectors->Search(std::vector<float>{0, 3}, 1, &hits).ok());
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits.front().id, "wal");
  EXPECT_FLOAT_EQ(hits.front().distance, 0);
}

}  // namespace
}  // namespace strata
