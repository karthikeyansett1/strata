#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "skiplist.h"

namespace strata {

enum class RecordType : std::uint8_t { kPut = 1, kDelete = 2 };

struct Entry {
  std::uint64_t sequence;
  RecordType type;
  std::string value;
};

class Memtable {
 public:
  void Apply(std::string key, Entry entry) {
    entries_.Upsert(std::move(key), std::move(entry),
                    [](const Entry& current, const Entry& incoming) {
                      return incoming.sequence > current.sequence;
                    });
  }

  [[nodiscard]] std::optional<Entry> Get(std::string_view key) const { return entries_.Get(key); }

  [[nodiscard]] auto OrderedEntries() const { return entries_.OrderedEntries(); }

 private:
  SkipList<std::string, Entry> entries_;
};

}  // namespace strata
