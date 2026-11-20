#include "strata/vector_store.h"

#include <bit>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

#include "disk.h"
#include "hnsw_index.h"

namespace strata {
namespace {
constexpr std::string_view kPrefix = "v/";
constexpr std::string_view kLimit = "v0";

std::string Encode(std::span<const float> values) {
  std::string bytes;
  bytes.reserve(4 + 4 * values.size());
  disk::Add(&bytes, static_cast<std::uint32_t>(values.size()));
  for (float value : values) {
    disk::Add(&bytes, std::bit_cast<std::uint32_t>(value));
  }
  return bytes;
}

bool Decode(std::string_view bytes, std::vector<float>* values) {
  std::uint32_t dimension;
  if (!disk::Take(&bytes, &dimension) || dimension == 0 || dimension > 4096 ||
      bytes.size() != 4 * dimension) {
    return false;
  }
  values->clear();
  values->reserve(dimension);
  while (!bytes.empty()) {
    std::uint32_t bits = 0;
    disk::Take(&bytes, &bits);
    values->push_back(std::bit_cast<float>(bits));
  }
  return vector::ValidVector(*values);
}

}  // namespace

struct VectorStore::State {
  explicit State(VectorOptions configured) : options(configured), index(configured) {}

  Status Load() {
    std::vector<std::pair<std::string, std::string>> rows;
    if (auto status = db->Scan(kPrefix, kLimit, &rows); !status.ok()) {
      return status;
    }
    vector::HnswIndex rebuilt(options);
    std::vector<std::string> labels;
    std::unordered_set<std::string> known;
    std::vector<float> values;
    std::size_t dim = 0;
    labels.reserve(rows.size());
    known.reserve(rows.size());
    for (const auto& [key, bytes] : rows) {
      const std::string_view id = std::string_view(key).substr(kPrefix.size());
      if (id.empty() || id.size() > 4096 || !Decode(bytes, &values) ||
          (dim != 0 && dim != values.size())) {
        return disk::Corruption("invalid persisted vector");
      }
      dim = values.size();
      rebuilt.Insert(values);
      labels.emplace_back(id);
      known.insert(labels.back());
    }
    index = std::move(rebuilt);
    ids = std::move(known);
    labels_ = std::move(labels);
    dimension = dim;
    return {};
  }

  VectorOptions options;
  vector::HnswIndex index;
  std::unique_ptr<DB> db;
  std::vector<std::string> labels_;
  std::unordered_set<std::string> ids;
  std::size_t dimension = 0;
  bool usable = true;
  mutable std::shared_mutex mutex;
};

VectorStore::VectorStore(ConstructionKey) {}
VectorStore::~VectorStore() = default;

Status VectorStore::Open(const Options& storage, const VectorOptions& options,
                         const std::string& db_path, std::unique_ptr<VectorStore>* out) noexcept
    try {
  if (!out || db_path.empty() || db_path.find('\0') != std::string::npos ||
      !vector::ValidOptions(options)) {
    return {StatusCode::kInvalidArgument, "invalid vector store options or path"};
  }
  auto store = std::make_unique<VectorStore>(ConstructionKey{});
  store->state_ = std::make_unique<State>(options);
  const auto path = std::filesystem::path(db_path) / "__strata_vectors";
  if (auto status = DB::Open(storage, path.string(), &store->state_->db); !status.ok()) {
    return status;
  }
  if (auto status = store->state_->Load(); !status.ok()) {
    return status;
  }
  *out = std::move(store);
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status VectorStore::Put(std::string_view id, std::span<const float> values) noexcept try {
  if (id.empty() || id.size() > 4096 || !vector::ValidVector(values)) {
    return {StatusCode::kInvalidArgument, "invalid vector id, dimension, or component"};
  }
  std::unique_lock lock(state_->mutex);
  if (!state_->usable) {
    return {StatusCode::kIOError, "reopen vector store after a failed update"};
  }
  if (state_->dimension != 0 && state_->dimension != values.size()) {
    return {StatusCode::kInvalidArgument, "vector dimension does not match the store"};
  }
  std::string label(id);
  const bool updating = state_->ids.contains(label);
  const auto key = std::string(kPrefix) + label;
  const auto payload = Encode(values);

  state_->usable = false;
  if (auto status = state_->db->Put(key, payload); !status.ok()) {
    return status;
  }
  if (updating) {
    if (auto status = state_->Load(); !status.ok()) {
      return status;
    }
  } else {
    state_->index.Insert(values);
    state_->ids.insert(label);
    state_->labels_.push_back(std::move(label));
    state_->dimension = values.size();
  }
  state_->usable = true;
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

Status VectorStore::Search(std::span<const float> query, std::size_t k, std::vector<VectorHit>* out,
                           std::size_t ef) const noexcept try {
  if (!out || k == 0 || k > 65536 || ef > 65536 || !vector::ValidVector(query)) {
    return {StatusCode::kInvalidArgument, "invalid vector or search size"};
  }
  std::shared_lock lock(state_->mutex);
  if (!state_->usable) {
    return {StatusCode::kIOError, "reopen vector store after a failed update"};
  }
  if (state_->dimension != 0 && state_->dimension != query.size()) {
    return {StatusCode::kInvalidArgument, "query dimension does not match the store"};
  }
  std::vector<VectorHit> hits;
  for (const auto& [distance, row] : state_->index.Search(query, k, ef)) {
    hits.push_back(
        {state_->labels_[row], vector::ReportDistance(distance, state_->options.metric)});
  }
  *out = std::move(hits);
  return {};
} catch (...) {
  return Status(StatusCode::kIOError);
}

}  // namespace strata
