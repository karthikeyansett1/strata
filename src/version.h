#pragma once

#include <atomic>
#include <memory>

#include "sstable.h"

namespace strata {

struct Table {
  SSTable reader;
  std::filesystem::path path;
  std::atomic<bool> obsolete = false;

  ~Table() {
    if (obsolete.load()) {
      unlink(path.c_str());
    }
  }
};

struct Version {
  std::shared_ptr<Memtable> active = std::make_shared<Memtable>();
  std::shared_ptr<const Memtable> immutable;
  std::vector<std::shared_ptr<Table>> tables;
};

}  // namespace strata
