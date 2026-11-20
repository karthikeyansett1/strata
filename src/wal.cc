#include "wal.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <system_error>
#include <utility>

#include "crc32c.h"

namespace strata {
namespace {

constexpr std::size_t kHeaderBytes = 8;
constexpr std::size_t kFixedBodyBytes = 17;
constexpr std::size_t kMinFrameBytes = kHeaderBytes + kFixedBodyBytes;
constexpr std::size_t kMaxBodyBytes = 64 * 1024 * 1024;

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  int Release() noexcept { return std::exchange(fd_, -1); }

 private:
  int fd_;
};

Status IoError(const char* operation, int error) {
  return {StatusCode::kIOError, std::string(operation) + ": " +
                                    std::error_code(error, std::generic_category()).message()};
}

Status Corruption(std::uint64_t offset) {
  return {StatusCode::kCorruption, "invalid WAL record at offset " + std::to_string(offset)};
}

Status Sync(int fd, const char* operation) {
  while (fsync(fd) != 0) {
    const int error = errno;
    if (error != EINTR) {
      return IoError(operation, error);
    }
  }
  return {};
}

template <class UInt>
UInt ReadLittleEndian(const char* bytes) {
  UInt value = 0;
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<UInt>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return value;
}

template <class UInt>
void WriteLittleEndian(char* bytes, UInt value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[i] = static_cast<char>(value >> (8 * i));
  }
}

Status ReadAt(int fd, std::uint64_t offset, char* bytes, std::size_t size) {
  while (size > 0) {
    const auto count = pread(fd, bytes, size, static_cast<off_t>(offset));
    if (count < 0) {
      const int error = errno;
      if (error == EINTR) {
        continue;
      }
      return IoError("read WAL", error);
    }
    if (count == 0) {
      return {StatusCode::kIOError, "WAL size changed during recovery"};
    }
    offset += count;
    bytes += count;
    size -= count;
  }
  return {};
}

struct Frame {
  RecordType type;
  std::uint64_t sequence;
  std::string_view key;
  std::string_view value;
};

bool Decode(std::string_view bytes, Frame* out) {
  const auto type = static_cast<RecordType>(bytes[8]);
  if (type != RecordType::kPut && type != RecordType::kDelete) {
    return false;
  }

  const auto key_size = ReadLittleEndian<std::uint32_t>(bytes.data() + 17);
  if (key_size > bytes.size() - kMinFrameBytes) {
    return false;
  }
  const auto value_size = ReadLittleEndian<std::uint32_t>(bytes.data() + 21 + key_size);
  if (value_size != bytes.size() - kMinFrameBytes - key_size ||
      (type == RecordType::kDelete && value_size != 0)) {
    return false;
  }

  *out = {type, ReadLittleEndian<std::uint64_t>(bytes.data() + 9), bytes.substr(21, key_size),
          bytes.substr(kMinFrameBytes + key_size)};
  return true;
}

bool ChecksumMatches(std::string_view bytes) {
  return ReadLittleEndian<std::uint32_t>(bytes.data()) == Crc32c(bytes.substr(4));
}

Status HasConsistentTruncatedBody(int fd, std::uint64_t offset, std::uint64_t end,
                                  std::uint32_t length, std::uint64_t sequence, bool* consistent) {
  *consistent = false;
  if (length < kFixedBodyBytes || length > kMaxBodyBytes || end - offset < kMinFrameBytes) {
    return {};
  }

  std::array<char, 13> metadata;
  if (auto status = ReadAt(fd, offset + kHeaderBytes, metadata.data(), metadata.size());
      !status.ok()) {
    return status;
  }
  const auto type = static_cast<RecordType>(metadata[0]);
  const auto key_size = ReadLittleEndian<std::uint32_t>(metadata.data() + 9);
  if ((type != RecordType::kPut && type != RecordType::kDelete) ||
      ReadLittleEndian<std::uint64_t>(metadata.data() + 1) != sequence ||
      key_size > length - kFixedBodyBytes || key_size > end - offset - kMinFrameBytes) {
    return {};
  }

  std::array<char, 4> value_length;
  if (auto status = ReadAt(fd, offset + 21 + key_size, value_length.data(), value_length.size());
      !status.ok()) {
    return status;
  }
  const auto value_size = ReadLittleEndian<std::uint32_t>(value_length.data());
  *consistent = value_size == length - kFixedBodyBytes - key_size &&
                (type == RecordType::kPut || value_size == 0);
  return {};
}

// A corrupted length can hide later records by pointing at or past EOF. Look for
// independently valid later frames before discarding a suspected torn suffix.
Status CheckForLaterFrame(int fd, std::uint64_t offset, std::uint64_t end,
                          std::uint64_t last_sequence) {
  constexpr std::size_t kPrefixBytes = 17;
  constexpr std::size_t kChunkBytes = 64 * 1024;
  std::array<char, kChunkBytes + kPrefixBytes - 1> buffer;
  const auto max_sequence_delta = (end - offset) / kMinFrameBytes;
  std::string candidate;

  for (auto start = offset + 1; end - start >= kMinFrameBytes;) {
    const auto count = std::min<std::uint64_t>(kChunkBytes, end - start - kMinFrameBytes + 1);
    if (auto status = ReadAt(fd, start, buffer.data(), count + kPrefixBytes - 1); !status.ok()) {
      return status;
    }

    for (std::size_t i = 0; i < count; ++i) {
      const char* prefix = buffer.data() + i;
      const auto type = static_cast<RecordType>(prefix[8]);
      if (type != RecordType::kPut && type != RecordType::kDelete) {
        continue;
      }
      const auto sequence = ReadLittleEndian<std::uint64_t>(prefix + 9);
      if (sequence <= last_sequence || sequence - last_sequence < 2 ||
          sequence - last_sequence > max_sequence_delta) {
        continue;
      }
      const auto length = ReadLittleEndian<std::uint32_t>(prefix + 4);
      if (length < kFixedBodyBytes || length > kMaxBodyBytes ||
          length > end - (start + i) - kHeaderBytes) {
        continue;
      }

      candidate.resize(kHeaderBytes + length);
      if (auto status = ReadAt(fd, start + i, candidate.data(), candidate.size()); !status.ok()) {
        return status;
      }
      Frame frame{};
      if (Decode(candidate, &frame) && ChecksumMatches(candidate)) {
        return Corruption(offset);
      }
    }
    start += count;
  }
  return {};
}

}  // namespace

Wal::~Wal() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

Status Wal::Open(const std::string& path) {
  std::error_code error;
  std::filesystem::create_directory(path, error);
  if (error) {
    return {StatusCode::kIOError, "create database directory: " + error.message()};
  }

  FileDescriptor directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (directory.get() < 0) {
    return IoError("open database directory", errno);
  }
  FileDescriptor file(openat(directory.get(), kFileName,
                             O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                             0600));
  if (file.get() < 0) {
    return IoError("open WAL", errno);
  }

  struct stat info {};
  if (fstat(file.get(), &info) != 0) {
    return IoError("stat WAL", errno);
  }
  if (!S_ISREG(info.st_mode)) {
    return {StatusCode::kInvalidArgument, "WAL must be a regular file"};
  }
  while (flock(file.get(), LOCK_EX | LOCK_NB) != 0) {
    const int lock_error = errno;
    if (lock_error == EWOULDBLOCK) {
      return {StatusCode::kBusy, "database already open"};
    }
    if (lock_error != EINTR) {
      return IoError("lock WAL", lock_error);
    }
  }

  // Persist the WAL name and the database directory itself before serving writes.
  if (auto status = Sync(directory.get(), "sync database directory"); !status.ok()) {
    return status;
  }
  FileDescriptor parent(openat(directory.get(), "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (parent.get() < 0) {
    return IoError("open parent directory", errno);
  }
  if (auto status = Sync(parent.get(), "sync parent directory"); !status.ok()) {
    return status;
  }
  fd_ = file.Release();
  return {};
}

Status Wal::Recover(const Replay& replay, std::uint64_t* last_sequence) {
  struct stat info {};
  if (fstat(fd_, &info) != 0) {
    return IoError("stat WAL", errno);
  }

  const auto end = static_cast<std::uint64_t>(info.st_size);
  std::uint64_t offset = 0;
  std::uint64_t sequence = 0;
  std::array<char, kHeaderBytes> header;
  std::string bytes;
  while (end - offset >= kHeaderBytes) {
    if (auto status = ReadAt(fd_, offset, header.data(), header.size()); !status.ok()) {
      return status;
    }
    const auto length = ReadLittleEndian<std::uint32_t>(header.data() + 4);
    if (length > end - offset - kHeaderBytes) {
      bool consistent = false;
      if (auto status =
              HasConsistentTruncatedBody(fd_, offset, end, length, sequence + 1, &consistent);
          !status.ok()) {
        return status;
      }
      // Agreeing lengths frame the suffix as a partial payload, even if the
      // payload itself contains bytes that look like another WAL record.
      if (!consistent) {
        if (auto status = CheckForLaterFrame(fd_, offset, end, sequence); !status.ok()) {
          return status;
        }
      }
      break;
    }
    if (length < kFixedBodyBytes || length > kMaxBodyBytes) {
      return Corruption(offset);
    }

    bytes.assign(header.data(), header.size());
    bytes.resize(kHeaderBytes + length);
    if (auto status = ReadAt(fd_, offset + kHeaderBytes, bytes.data() + kHeaderBytes, length);
        !status.ok()) {
      return status;
    }
    Frame frame{};
    const bool well_formed =
        Decode(bytes, &frame) && frame.sequence > sequence && frame.sequence - sequence == 1;
    if (!ChecksumMatches(bytes)) {
      if (bytes.size() != end - offset) {
        return Corruption(offset);
      }
      if (!well_formed) {
        if (auto status = CheckForLaterFrame(fd_, offset, end, sequence); !status.ok()) {
          return status;
        }
      }
      break;
    }

    if (!well_formed) {
      return Corruption(offset);
    }
    replay(frame.key, frame.value, frame.type, frame.sequence);
    sequence = frame.sequence;
    offset += bytes.size();
  }

  if (offset != end) {
    while (ftruncate(fd_, static_cast<off_t>(offset)) != 0) {
      const int error = errno;
      if (error != EINTR) {
        return IoError("truncate torn WAL tail", error);
      }
    }
    if (auto status = Sync(fd_, "sync repaired WAL"); !status.ok()) {
      return status;
    }
  }
  *last_sequence = sequence;
  return {};
}

Status Wal::Append(std::string_view key, std::string_view value, RecordType type,
                   std::uint64_t sequence) {
  constexpr auto kMaxDataBytes = kMaxBodyBytes - kFixedBodyBytes;
  if (key.size() > kMaxDataBytes || value.size() > kMaxDataBytes - key.size()) {
    return {StatusCode::kInvalidArgument, "key and value exceed WAL record size limit"};
  }

  const auto length = static_cast<std::uint32_t>(kFixedBodyBytes + key.size() + value.size());
  std::string bytes(kHeaderBytes + length, '\0');
  WriteLittleEndian(bytes.data() + 4, length);
  bytes[8] = static_cast<char>(type);
  WriteLittleEndian(bytes.data() + 9, sequence);
  WriteLittleEndian(bytes.data() + 17, static_cast<std::uint32_t>(key.size()));
  std::copy(key.begin(), key.end(), bytes.begin() + 21);
  WriteLittleEndian(bytes.data() + 21 + key.size(), static_cast<std::uint32_t>(value.size()));
  std::copy(value.begin(), value.end(), bytes.begin() + kMinFrameBytes + key.size());
  WriteLittleEndian(bytes.data(), Crc32c(std::string_view(bytes).substr(4)));

  std::string_view remaining = bytes;
  while (!remaining.empty()) {
    const auto written = write(fd_, remaining.data(), remaining.size());
    if (written < 0) {
      const int error = errno;
      if (error == EINTR) {
        continue;
      }
      return IoError("append WAL", error);
    }
    if (written == 0) {
      return {StatusCode::kIOError, "WAL write made no progress"};
    }
    remaining.remove_prefix(written);
  }
  return Sync(fd_, "sync WAL");
}

}  // namespace strata
