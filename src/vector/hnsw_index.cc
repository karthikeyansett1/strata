#include "hnsw_index.h"

#include <limits>
#include <queue>
#include <stdexcept>

namespace strata::vector {
namespace {

struct Visited {
  std::vector<std::uint32_t> marks;
  std::uint32_t stamp = 0;

  void Start(std::size_t size) {
    if (marks.size() < size) {
      marks.resize(size);
    }
    if (++stamp == 0) {
      std::fill(marks.begin(), marks.end(), 0);
      stamp = 1;
    }
  }

  bool Add(std::uint32_t id) {
    if (marks[id] == stamp) {
      return false;
    }
    marks[id] = stamp;
    return true;
  }
};

}  // namespace

bool ValidOptions(const VectorOptions& options) {
  return options.m >= 2 && options.m <= 64 && options.ef_construction >= options.m &&
         options.ef_construction <= 4096 && options.ef_search >= 1 && options.ef_search <= 65536 &&
         (options.metric == Metric::kL2 || options.metric == Metric::kCosine);
}

unsigned HnswIndex::RandomLevel() {
  unsigned level = 0;
  while (level < 32 && rng_() % options_.m == 0) {
    ++level;
  }
  return level;
}

float HnswIndex::Measure(std::span<const float> query, std::uint32_t id) const {
  return Distance(query, nodes_[id].values, options_.metric);
}

Neighbor HnswIndex::Greedy(std::span<const float> query, Neighbor entry, std::size_t layer) const {
  while (true) {
    const auto previous = entry;
    for (auto id : nodes_[previous.second].edges[layer]) {
      entry = std::min(entry, Neighbor{Measure(query, id), id});
    }
    if (entry == previous) {
      return entry;
    }
  }
}

std::vector<Neighbor> HnswIndex::SearchLayer(std::span<const float> query, Neighbor entry,
                                             std::size_t layer, std::size_t ef) const {
  thread_local Visited visited;
  visited.Start(nodes_.size());
  std::priority_queue<Neighbor> best;
  std::priority_queue<Neighbor, std::vector<Neighbor>, std::greater<>> frontier;
  best.push(entry);
  frontier.push(entry);
  visited.Add(entry.second);
  while (!frontier.empty()) {
    const auto current = frontier.top();
    frontier.pop();
    if (best.size() >= ef && current > best.top()) {
      break;
    }
    for (auto id : nodes_[current.second].edges[layer]) {
      if (!visited.Add(id)) {
        continue;
      }
      const Neighbor candidate{Measure(query, id), id};
      if (best.size() < ef || candidate < best.top()) {
        frontier.push(candidate);
        best.push(candidate);
        if (best.size() > ef) {
          best.pop();
        }
      }
    }
  }
  std::vector<Neighbor> result(best.size());
  for (auto it = result.rbegin(); it != result.rend(); ++it) {
    *it = best.top();
    best.pop();
  }
  return result;
}

std::vector<std::uint32_t> HnswIndex::Select(const std::vector<Neighbor>& candidates,
                                             std::size_t limit) const {
  std::vector<std::uint32_t> result;
  result.reserve(std::min(limit, candidates.size()));
  for (const auto& [distance, id] : candidates) {
    const auto occluded = std::any_of(result.begin(), result.end(), [&](auto selected) {
      return Measure(nodes_[id].values, selected) < distance;
    });
    if (!occluded) {
      result.push_back(id);
      if (result.size() == limit) {
        break;
      }
    }
  }
  return result;
}

void HnswIndex::Connect(std::uint32_t source, std::uint32_t target, std::size_t layer,
                        std::size_t limit) {
  auto& edges = nodes_[source].edges[layer];
  edges.push_back(target);
  if (edges.size() > limit) {
    std::vector<Neighbor> candidates;
    candidates.reserve(edges.size());
    for (auto id : edges) {
      candidates.emplace_back(Measure(nodes_[source].values, id), id);
    }
    std::sort(candidates.begin(), candidates.end());
    edges = Select(candidates, limit);
  }
}

void HnswIndex::Insert(std::span<const float> values) {
  if (nodes_.size() >= std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("HNSW node limit reached");
  }
  const auto level = RandomLevel();
  const auto id = static_cast<std::uint32_t>(nodes_.size());
  nodes_.push_back(
      {Prepare(values, options_.metric), std::vector<std::vector<std::uint32_t>>(level + 1)});
  if (id == 0) {
    max_layer_ = level;
    return;
  }

  const auto& query = nodes_.back().values;
  Neighbor entry{Measure(query, entry_), entry_};
  for (auto layer = max_layer_; layer > level; --layer) {
    entry = Greedy(query, entry, layer);
  }
  for (auto layer = std::min<std::size_t>(level, max_layer_) + 1; layer-- > 0;) {
    const auto candidates = SearchLayer(query, entry, layer, options_.ef_construction);
    const auto limit = layer == 0 ? options_.m * 2 : options_.m;
    auto& neighbors = nodes_[id].edges[layer];
    neighbors = Select(candidates, options_.m);
    for (auto neighbor : neighbors) {
      Connect(neighbor, id, layer, limit);
    }
    entry = candidates.front();
  }
  if (level > max_layer_) {
    entry_ = id;
    max_layer_ = level;
  }
}

std::vector<Neighbor> HnswIndex::Search(std::span<const float> values, std::size_t k,
                                        std::size_t ef) const {
  if (nodes_.empty() || k == 0) {
    return {};
  }
  auto query = Prepare(values, options_.metric);
  Neighbor entry{Measure(query, entry_), entry_};
  for (auto layer = max_layer_; layer > 0; --layer) {
    entry = Greedy(query, entry, layer);
  }
  auto result = SearchLayer(query, entry, 0, std::max(k, ef == 0 ? options_.ef_search : ef));
  const auto keep = std::min(k, nodes_.size());
  if (result.size() < keep) {
    // Extreme duplication or pruning can leave a small reachable component.
    result.clear();
    result.reserve(nodes_.size());
    for (std::size_t id = 0; id < nodes_.size(); ++id) {
      result.emplace_back(Measure(query, id), id);
    }
    std::partial_sort(result.begin(), result.begin() + keep, result.end());
  }
  result.resize(keep);
  return result;
}

}  // namespace strata::vector
