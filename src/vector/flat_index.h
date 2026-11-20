#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

#include "distance.h"

namespace strata::vector {

using Neighbor = std::pair<float, std::uint32_t>;  // Rank by distance, then insertion index.

class FlatIndex {
 public:
  explicit FlatIndex(Metric metric) : metric_(metric) {}

  void Insert(std::span<const float> values) { rows_.push_back(Prepare(values, metric_)); }

  std::vector<Neighbor> Search(std::span<const float> query, std::size_t k) const {
    if (k == 0 || rows_.empty()) {
      return {};
    }
    const auto prepared = Prepare(query, metric_);
    std::vector<Neighbor> result;
    result.reserve(rows_.size());
    for (std::size_t i = 0; i < rows_.size(); ++i) {
      result.emplace_back(Distance(prepared, rows_[i], metric_), i);
    }
    const auto keep = std::min(k, result.size());
    std::partial_sort(result.begin(), result.begin() + keep, result.end());
    result.resize(keep);
    return result;
  }

 private:
  Metric metric_;
  std::vector<std::vector<float>> rows_;
};

}  // namespace strata::vector
