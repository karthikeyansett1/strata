#pragma once

#include <cstddef>

namespace strata {

enum class SyncMode { kAlways, kBatch };

struct Options {
  std::size_t memtable_bytes = 4 * 1024 * 1024;
  SyncMode sync_mode = SyncMode::kAlways;
  std::size_t background_threads = 2;
  std::size_t bloom_bits_per_key = 10;
  std::size_t block_bytes = 4 * 1024;
  std::size_t l0_compaction_trigger = 4;
};

}  // namespace strata
