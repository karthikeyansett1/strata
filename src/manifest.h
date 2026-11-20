#pragma once

#include <vector>

#include "disk.h"

namespace strata {

struct TableMetadata {
  std::uint64_t id;
  std::uint32_t level;
  std::string smallest;
  std::string largest;
};

struct Manifest {
  std::uint64_t sequence = 0;
  std::uint64_t next_file = 1;
  std::uint64_t wal_start = 1;
  std::vector<TableMetadata> tables;

  Status Load(const std::filesystem::path& directory);
  Status Save(const std::filesystem::path& directory, const disk::File& directory_fd) const;
};

}  // namespace strata
