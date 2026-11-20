#include "memtable.h"

#include <gtest/gtest.h>

namespace strata {
namespace {

TEST(Memtable, KeepsNewestSequenceIncludingTombstones) {
  Memtable table;
  EXPECT_FALSE(table.Get("b"));
  table.Apply("b", {3, RecordType::kPut, "current"});
  table.Apply("b", {2, RecordType::kPut, "stale"});
  table.Apply("b", {3, RecordType::kPut, "duplicate"});
  auto entry = table.Get("b");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->value, "current");

  table.Apply("b", {5, RecordType::kDelete, {}});
  table.Apply("b", {4, RecordType::kPut, "stale"});
  entry = table.Get("b");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->sequence, 5u);
  EXPECT_EQ(entry->type, RecordType::kDelete);

  table.Apply("b", {6, RecordType::kPut, "revived"});
  table.Apply("a", {7, RecordType::kPut, ""});
  const auto ordered = table.OrderedEntries();
  ASSERT_EQ(ordered.size(), 2u);
  EXPECT_EQ(ordered[0].first, "a");
  EXPECT_EQ(ordered[0].second.type, RecordType::kPut);
  EXPECT_EQ(ordered[1].first, "b");
  EXPECT_EQ(ordered[1].second.value, "revived");
}

}  // namespace
}  // namespace strata
