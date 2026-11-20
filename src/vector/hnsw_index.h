#pragma once

#include <random>

#include "flat_index.h"

namespace strata::vector {

bool ValidOptions(const VectorOptions& options);

class HnswIndex {
 public:
  explicit HnswIndex(VectorOptions options) : options_(options) {}

  void Insert(std::span<const float> values);
  std::vector<Neighbor> Search(std::span<const float> values, std::size_t k,
                               std::size_t ef = 0) const;

 private:
  struct Node {
    std::vector<float> values;
    std::vector<std::vector<std::uint32_t>> edges;
  };

  unsigned RandomLevel();
  Neighbor Greedy(std::span<const float> query, Neighbor entry, std::size_t layer) const;
  std::vector<Neighbor> SearchLayer(std::span<const float> query, Neighbor entry, std::size_t layer,
                                    std::size_t ef) const;
  std::vector<std::uint32_t> Select(const std::vector<Neighbor>& candidates,
                                    std::size_t limit) const;
  void Connect(std::uint32_t source, std::uint32_t target, std::size_t layer, std::size_t limit);
  float Measure(std::span<const float> query, std::uint32_t id) const;

  VectorOptions options_;
  std::mt19937 rng_{4831};
  std::vector<Node> nodes_;
  std::uint32_t entry_ = 0;
  std::size_t max_layer_ = 0;
};

}  // namespace strata::vector
