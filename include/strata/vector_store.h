#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "strata/db.h"

namespace strata {

enum class Metric { kL2, kCosine };

struct VectorOptions {
  std::size_t m = 16;
  std::size_t ef_construction = 100;
  std::size_t ef_search = 64;
  Metric metric = Metric::kCosine;
};

struct VectorHit {
  std::string id;
  float distance;
};

class VectorStore {
 private:
  class ConstructionKey {
    friend class VectorStore;
    ConstructionKey() = default;
  };

 public:
  explicit VectorStore(ConstructionKey);
  ~VectorStore();

  [[nodiscard]] static Status Open(const Options& storage, const VectorOptions& options,
                                   const std::string& db_path,
                                   std::unique_ptr<VectorStore>* out) noexcept;
  [[nodiscard]] Status Put(std::string_view id, std::span<const float> values) noexcept;
  [[nodiscard]] Status Search(std::span<const float> query, std::size_t k,
                              std::vector<VectorHit>* out, std::size_t ef = 0) const noexcept;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace strata
