#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace strata::test {

class TempDirectory {
 public:
  TempDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "strata-test-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
      throw std::runtime_error("mkdtemp failed");
    }
    path = std::move(pattern);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  std::string path;
};

}  // namespace strata::test
