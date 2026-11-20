#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "crc32c.h"
#include "strata/status.h"

namespace strata::disk {

inline Status Error(std::string_view operation, int error = errno) {
  return {StatusCode::kIOError, std::string(operation) + ": " +
                                    std::error_code(error, std::generic_category()).message()};
}

inline Status Corruption(std::string_view message) {
  return {StatusCode::kCorruption, std::string(message)};
}

class File {
 public:
  explicit File(int fd = -1) noexcept : fd_(fd) {}
  File(File&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  File& operator=(File&& other) noexcept {
    std::swap(fd_, other.fd_);
    return *this;
  }
  ~File() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }

  Status Sync() const {
    while (fsync(fd_) != 0) {
      if (errno != EINTR) {
        return Error("sync file or directory");
      }
    }
    return {};
  }

  Status Size(std::uint64_t* out) const {
    struct stat info {};
    if (fstat(fd_, &info) != 0) {
      return Error("stat file");
    }
    if (!S_ISREG(info.st_mode)) {
      return Corruption("expected a regular file");
    }
    *out = info.st_size;
    return {};
  }

  Status Read(std::uint64_t offset, std::size_t size, std::string* out) const {
    out->resize(size);
    std::size_t done = 0;
    while (done < size) {
      const auto n = pread(fd_, out->data() + done, size - done, static_cast<off_t>(offset + done));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return Error("read file");
      }
      if (n == 0) {
        return Corruption("unexpected end of file");
      }
      done += n;
    }
    return {};
  }

  Status Write(std::string_view bytes) const {
    while (!bytes.empty()) {
      const auto n = write(fd_, bytes.data(), bytes.size());
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return Error("write file");
      }
      if (n == 0) {
        return {StatusCode::kIOError, "write made no progress"};
      }
      bytes.remove_prefix(n);
    }
    return {};
  }

 private:
  int fd_;
};

template <class UInt>
void Add(std::string* data, UInt value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    data->push_back(static_cast<char>(value >> (8 * i)));
  }
}

template <class UInt>
bool Take(std::string_view* data, UInt* out) {
  if (data->size() < sizeof(UInt)) {
    return false;
  }
  *out = 0;
  for (std::size_t i = 0; i < sizeof(UInt); ++i) {
    *out |= static_cast<UInt>(static_cast<unsigned char>((*data)[i])) << (8 * i);
  }
  data->remove_prefix(sizeof(UInt));
  return true;
}

inline void AddString(std::string* data, std::string_view value) {
  Add(data, static_cast<std::uint32_t>(value.size()));
  data->append(value);
}

inline bool TakeString(std::string_view* data, std::string* value) {
  std::uint32_t length;
  if (!Take(data, &length) || data->size() < length) {
    return false;
  }
  value->assign(data->substr(0, length));
  data->remove_prefix(length);
  return true;
}

inline void Seal(std::string* data) { Add(data, Crc32c(*data)); }

inline bool Unseal(std::string_view* data) {
  if (data->size() < sizeof(std::uint32_t)) {
    return false;
  }
  auto tail = data->substr(data->size() - sizeof(std::uint32_t));
  data->remove_suffix(sizeof(std::uint32_t));
  std::uint32_t crc;
  return Take(&tail, &crc) && Crc32c(*data) == crc;
}

inline Status Install(const File& file, const std::filesystem::path& temporary,
                      const std::filesystem::path& target, const File& directory) {
  if (auto status = file.Sync(); !status.ok()) {
    return status;
  }
  if (rename(temporary.c_str(), target.c_str()) != 0) {
    return Error("rename durable file");
  }
  return directory.Sync();
}

}  // namespace strata::disk
