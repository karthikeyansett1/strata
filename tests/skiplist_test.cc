#include "skiplist.h"

#include <gtest/gtest.h>

#include <map>
#include <random>

namespace strata {
namespace {

TEST(SkipList, RandomizedAgainstOrderedMap) {
  SkipList<int, int> entries;
  std::map<int, int> reference;
  std::mt19937 random(2389);

  for (int i = 0; i < 100'000; ++i) {
    const int key = random() % 20'000;
    if ((random() & 1) == 0) {
      entries.Upsert(key, i, [](int, int) { return true; });
      reference.insert_or_assign(key, i);
    } else {
      const auto expected = reference.find(key);
      const auto found = entries.Get(key);
      ASSERT_EQ(found.has_value(), expected != reference.end()) << key;
      if (found) {
        EXPECT_EQ(*found, expected->second) << key;
      }
    }
  }

  const auto ordered = entries.OrderedEntries();
  ASSERT_EQ(ordered.size(), reference.size());
  auto expected = reference.begin();
  for (const auto& [key, value] : ordered) {
    EXPECT_EQ(key, expected->first);
    EXPECT_EQ(value, expected->second);
    ++expected;
  }
}

}  // namespace
}  // namespace strata
