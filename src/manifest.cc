#include "manifest.h"

#include <limits>

namespace strata {
namespace {
constexpr std::uint32_t kMagic = 0x314d5453;
constexpr std::uint64_t kMaxManifestBytes = 256 * 1024 * 1024;
}  // namespace

Status Manifest::Load(const std::filesystem::path& directory) {
  disk::File file(
      open((directory / "MANIFEST").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!file.valid()) {
    return errno == ENOENT ? Status(StatusCode::kNotFound) : disk::Error("open manifest");
  }
  std::uint64_t size = 0;
  if (auto status = file.Size(&size); !status.ok()) {
    return status;
  }
  if (size > kMaxManifestBytes) {
    return disk::Corruption("manifest exceeds size limit");
  }
  std::string bytes;
  if (auto status = file.Read(0, size, &bytes); !status.ok()) {
    return status;
  }

  std::string_view data(bytes);
  Manifest loaded;
  std::uint32_t magic;
  std::uint64_t count;
  if (!disk::Unseal(&data) || !disk::Take(&data, &magic) || magic != kMagic ||
      !disk::Take(&data, &loaded.sequence) || !disk::Take(&data, &loaded.next_file) ||
      !disk::Take(&data, &loaded.wal_start) || !disk::Take(&data, &count) || count == 0 ||
      count > data.size() / 20 || loaded.sequence == std::numeric_limits<std::uint64_t>::max() ||
      loaded.wal_start != loaded.sequence + 1) {
    return disk::Corruption("invalid manifest header");
  }

  std::uint64_t previous = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    TableMetadata table{};
    if (!disk::Take(&data, &table.id) || !disk::Take(&data, &table.level) ||
        !disk::TakeString(&data, &table.smallest) || !disk::TakeString(&data, &table.largest) ||
        table.id <= previous || table.id >= loaded.next_file || table.level != 0 ||
        table.smallest > table.largest) {
      return disk::Corruption("invalid manifest table");
    }
    previous = table.id;
    loaded.tables.push_back(std::move(table));
  }
  if (!data.empty()) {
    return disk::Corruption("unexpected bytes in manifest");
  }
  *this = std::move(loaded);
  return {};
}

Status Manifest::Save(const std::filesystem::path& directory,
                      const disk::File& directory_fd) const {
  std::string bytes;
  disk::Add(&bytes, kMagic);
  disk::Add(&bytes, sequence);
  disk::Add(&bytes, next_file);
  disk::Add(&bytes, wal_start);
  disk::Add(&bytes, static_cast<std::uint64_t>(tables.size()));
  for (const auto& table : tables) {
    disk::Add(&bytes, table.id);
    disk::Add(&bytes, table.level);
    disk::AddString(&bytes, table.smallest);
    disk::AddString(&bytes, table.largest);
  }
  disk::Seal(&bytes);
  if (bytes.size() > kMaxManifestBytes) {
    return {StatusCode::kIOError, "manifest exceeds size limit"};
  }
  const auto temporary = directory / "MANIFEST.tmp";
  disk::File file(
      open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (!file.valid()) {
    return disk::Error("create manifest");
  }
  if (auto status = file.Write(bytes); !status.ok()) {
    return status;
  }
  return disk::Install(file, temporary, directory / "MANIFEST", directory_fd);
}

}  // namespace strata
