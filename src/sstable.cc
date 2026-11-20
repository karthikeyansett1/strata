#include "sstable.h"

#include <algorithm>
#include <iterator>

namespace strata {
namespace {

constexpr std::uint32_t kMagic = 0x31545353;
constexpr std::uint64_t kFooterBytes = 48;
constexpr std::uint64_t kMaxSectionBytes = 256 * 1024 * 1024;
constexpr std::uint64_t kMinRowBytes = 17;
constexpr std::uint64_t kCrcBytes = 4;
constexpr std::uint64_t kMaxBlockBytes = 64 * 1024 * 1024 + kCrcBytes;

Status WriteBlock(const disk::File& file, std::string_view last, std::string* block,
                  std::string* index, std::uint64_t* offset) {
  disk::Seal(block);
  disk::AddString(index, last);
  disk::Add(index, *offset);
  disk::Add(index, static_cast<std::uint64_t>(block->size()));
  if (auto status = file.Write(*block); !status.ok()) {
    return status;
  }
  *offset += block->size();
  block->clear();
  return {};
}

disk::File OpenTable(const std::filesystem::path& path) {
  return disk::File(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
}

Status OpenError() {
  return errno == ENOENT ? disk::Corruption("manifest references a missing SSTable")
                         : disk::Error("open SSTable");
}

}  // namespace

std::filesystem::path TablePath(const std::filesystem::path& directory, std::uint64_t id) {
  return directory / ("sst-" + std::to_string(id) + ".sst");
}

Status BuildTable(const std::filesystem::path& path, const disk::File& directory,
                  const Options& options, const Rows& entries) {
  auto temporary = path;
  temporary += ".tmp";
  disk::File file(
      open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (!file.valid()) {
    return disk::Error("create SSTable");
  }

  Bloom bloom(entries.size(), options.bloom_bits_per_key);
  std::string block;
  std::string index;
  std::string_view last;
  std::uint64_t offset = 0;
  for (const auto& [key, entry] : entries) {
    const auto size = kMinRowBytes + key.size() + entry.value.size();
    if (!block.empty() && block.size() + size + kCrcBytes > options.block_bytes) {
      if (auto status = WriteBlock(file, last, &block, &index, &offset); !status.ok()) {
        return status;
      }
    }
    disk::AddString(&block, key);
    disk::Add(&block, entry.sequence);
    disk::Add(&block, static_cast<std::uint8_t>(entry.type));
    disk::AddString(&block, entry.value);
    bloom.Insert(key);
    last = key;
  }
  if (auto status = WriteBlock(file, last, &block, &index, &offset); !status.ok()) {
    return status;
  }
  disk::Seal(&index);

  std::string filter(1, static_cast<char>(bloom.probes()));
  filter.append(bloom.bits());
  disk::Seal(&filter);
  if (filter.size() > kMaxSectionBytes || index.size() > kMaxSectionBytes) {
    return {StatusCode::kIOError, "SSTable metadata exceeds size limit"};
  }
  if (auto status = file.Write(filter); !status.ok()) {
    return status;
  }
  if (auto status = file.Write(index); !status.ok()) {
    return status;
  }

  std::string footer;
  disk::Add(&footer, offset);
  disk::Add(&footer, static_cast<std::uint64_t>(filter.size()));
  disk::Add(&footer, offset + filter.size());
  disk::Add(&footer, static_cast<std::uint64_t>(index.size()));
  disk::Add(&footer, static_cast<std::uint64_t>(entries.size()));
  disk::Add(&footer, kMagic);
  disk::Seal(&footer);
  if (auto status = file.Write(footer); !status.ok()) {
    return status;
  }
  return disk::Install(file, temporary, path, directory);
}

Status SSTable::Open(const std::filesystem::path& path, const TableMetadata& metadata,
                     std::uint64_t max_sequence) {
  auto file = OpenTable(path);
  if (!file.valid()) {
    return OpenError();
  }
  std::uint64_t size = 0;
  if (auto status = file.Size(&size); !status.ok()) {
    return status;
  }
  if (size < kFooterBytes) {
    return disk::Corruption("missing SSTable footer");
  }

  std::string bytes;
  const auto footer_offset = size - kFooterBytes;
  if (auto status = file.Read(footer_offset, kFooterBytes, &bytes); !status.ok()) {
    return status;
  }
  std::string_view data(bytes);
  std::uint64_t bloom_offset, bloom_size, index_offset, index_size, entries;
  std::uint32_t magic;
  if (!disk::Unseal(&data) || !disk::Take(&data, &bloom_offset) ||
      !disk::Take(&data, &bloom_size) || !disk::Take(&data, &index_offset) ||
      !disk::Take(&data, &index_size) || !disk::Take(&data, &entries) ||
      !disk::Take(&data, &magic) || magic != kMagic || index_offset > footer_offset ||
      index_size != footer_offset - index_offset || bloom_offset > index_offset ||
      bloom_size != index_offset - bloom_offset || bloom_size > kMaxSectionBytes ||
      index_size > kMaxSectionBytes || entries == 0 || entries > bloom_offset / kMinRowBytes) {
    return disk::Corruption("invalid SSTable footer");
  }

  if (auto status = file.Read(bloom_offset, bloom_size, &bytes); !status.ok()) {
    return status;
  }
  data = bytes;
  std::uint8_t probes;
  if (!disk::Unseal(&data) || !disk::Take(&data, &probes) || probes == 0 || probes > 30 ||
      data.empty()) {
    return disk::Corruption("invalid SSTable bloom filter");
  }
  bloom_.emplace(std::string(data), probes);

  if (auto status = file.Read(index_offset, index_size, &bytes); !status.ok()) {
    return status;
  }
  data = bytes;
  if (!disk::Unseal(&data)) {
    return disk::Corruption("invalid SSTable index checksum");
  }
  std::uint64_t next = 0;
  while (!data.empty()) {
    Block block{};
    if (!disk::TakeString(&data, &block.last) || !disk::Take(&data, &block.offset) ||
        !disk::Take(&data, &block.size) || block.offset != next ||
        block.size < kMinRowBytes + kCrcBytes || block.size > kMaxBlockBytes ||
        block.size > bloom_offset - next ||
        (!blocks_.empty() && blocks_.back().last >= block.last)) {
      return disk::Corruption("invalid SSTable block index");
    }
    next += block.size;
    blocks_.push_back(std::move(block));
  }
  if (next != bloom_offset || blocks_.empty() || blocks_.size() > entries ||
      blocks_.back().last != metadata.largest || blocks_.front().last < metadata.smallest) {
    return disk::Corruption("SSTable index does not match metadata");
  }
  path_ = path;
  smallest_ = metadata.smallest;
  max_sequence_ = max_sequence;
  entry_count_ = entries;
  return {};
}

std::size_t SSTable::FindBlock(std::string_view key) const {
  return std::lower_bound(
             blocks_.begin(), blocks_.end(), key,
             [](const Block& block, std::string_view term) { return block.last < term; }) -
         blocks_.begin();
}

Status SSTable::ReadBlock(const disk::File& file, std::size_t index, Rows* rows) const {
  const auto& block = blocks_[index];
  std::string bytes;
  if (auto status = file.Read(block.offset, block.size, &bytes); !status.ok()) {
    return status;
  }
  std::string_view data(bytes);
  if (!disk::Unseal(&data)) {
    return disk::Corruption("invalid SSTable data checksum");
  }

  rows->clear();
  while (!data.empty()) {
    std::string key;
    Entry entry{};
    std::uint8_t type;
    if (!disk::TakeString(&data, &key) || !disk::Take(&data, &entry.sequence) ||
        !disk::Take(&data, &type) || !disk::TakeString(&data, &entry.value) ||
        entry.sequence == 0 || entry.sequence > max_sequence_ ||
        (type != static_cast<std::uint8_t>(RecordType::kPut) &&
         type != static_cast<std::uint8_t>(RecordType::kDelete)) ||
        (type == static_cast<std::uint8_t>(RecordType::kDelete) && !entry.value.empty()) ||
        (!rows->empty() && rows->back().first >= key)) {
      return disk::Corruption("invalid SSTable row");
    }
    entry.type = static_cast<RecordType>(type);
    rows->emplace_back(std::move(key), std::move(entry));
  }
  if (rows->empty() || rows->back().first != block.last ||
      (index == 0 ? rows->front().first != smallest_
                  : rows->front().first <= blocks_[index - 1].last) ||
      rows->size() > entry_count_) {
    return disk::Corruption("SSTable rows do not match index");
  }
  return {};
}

Status SSTable::Get(std::string_view key, std::optional<Entry>* out) const {
  out->reset();
  const auto index = FindBlock(key);
  if (index == blocks_.size() || key < smallest_ || !bloom_->MayContain(key)) {
    return {};
  }
  auto file = OpenTable(path_);
  if (!file.valid()) {
    return OpenError();
  }

  Rows rows;
  if (auto status = ReadBlock(file, index, &rows); !status.ok()) {
    return status;
  }
  const auto found = std::lower_bound(rows.begin(), rows.end(), key,
                                      [](const auto& row, auto term) { return row.first < term; });
  if (found != rows.end() && found->first == key) {
    out->emplace(std::move(found->second));
  }
  return {};
}

Status SSTable::ReadAll(Rows* out) const {
  auto file = OpenTable(path_);
  if (!file.valid()) {
    return OpenError();
  }
  Rows block;
  for (std::size_t index = 0; index < blocks_.size(); ++index) {
    if (auto status = ReadBlock(file, index, &block); !status.ok()) {
      return status;
    }
    out->insert(out->end(), std::make_move_iterator(block.begin()),
                std::make_move_iterator(block.end()));
  }
  if (out->size() != entry_count_) {
    return disk::Corruption("SSTable entry count mismatch");
  }
  return {};
}

Status SSTable::ReadRange(std::string_view start, std::string_view end, Rows* out) const {
  auto index = FindBlock(start);
  if (index == blocks_.size() || end <= smallest_) {
    return {};
  }
  auto file = OpenTable(path_);
  if (!file.valid()) {
    return OpenError();
  }
  Rows block;
  for (; index < blocks_.size(); ++index) {
    if (auto status = ReadBlock(file, index, &block); !status.ok()) {
      return status;
    }
    for (auto& row : block) {
      if (row.first >= end) {
        return {};
      }
      if (row.first >= start) {
        out->push_back(std::move(row));
      }
    }
    if (blocks_[index].last >= end) {
      break;
    }
  }
  return {};
}

}  // namespace strata
