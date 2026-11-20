#pragma once

#include <string>
#include <utility>

namespace strata {

enum class StatusCode { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument, kBusy };

class Status {
 public:
  Status() noexcept = default;
  explicit Status(StatusCode code) noexcept : code_(code) {}
  Status(StatusCode code, std::string message) noexcept
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

}  // namespace strata
