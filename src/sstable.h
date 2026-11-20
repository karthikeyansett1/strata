#pragma once

#include <optional>
#include <vector>

#include "bloom.h"
#include "manifest.h"
#include "memtable.h"
#include "strata/options.h"

namespace strata {

using Rows = std::vector<std::pair<std::string, Entry>>;

std::filesystem::path TablePath(const std::filesystem::path& directory, std::uint64_t id);
Status BuildTable(const std::filesystem::path& path, const disk::File& directory,
                  const Options& options, const Rows& entries);

class SSTable {
 public:
  Status Open(const std::filesystem::path& path, const TableMetadata& metadata,
              std::uint64_t max_sequence);
  Status Get(std::string_view key, std::optional<Entry>* out) const;
  Status ReadAll(Rows* out) const;
  Status ReadRange(std::string_view start, std::string_view end, Rows* out) const;

 private:
  struct Block {
    std::string last;
    std::uint64_t offset;
    std::uint64_t size;
  };

  std::size_t FindBlock(std::string_view key) const;
  Status ReadBlock(const disk::File& file, std::size_t index, Rows* rows) const;

  std::filesystem::path path_;
  std::string smallest_;
  std::vector<Block> blocks_;
  std::optional<Bloom> bloom_;
  std::uint64_t max_sequence_ = 0;
  std::uint64_t entry_count_ = 0;
};

}  // namespace strata
