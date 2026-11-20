#include <pthread.h>
#include <signal.h>

#include <charconv>
#include <iostream>
#include <string_view>
#include <thread>

#include "server/server.h"

namespace {
template <class UInt>
bool Parse(std::string_view text, UInt* value) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = 7379;
  std::size_t threads = 4;
  std::string data = "data";
  strata::Options options;
  for (int i = 1; i < argc; i += 2) {
    const std::string_view flag = argv[i];
    const std::string_view value = i + 1 < argc ? argv[i + 1] : "";
    if (flag == "--port" && Parse(value, &port)) {
      continue;
    }
    if (flag == "--threads" && Parse(value, &threads) && threads > 0 && threads <= 256) {
      continue;
    }
    if (flag == "--data-dir" && !value.empty()) {
      data = value;
      continue;
    }
    if (flag == "--sync-mode" && (value == "always" || value == "batch")) {
      options.sync_mode = value == "batch" ? strata::SyncMode::kBatch : strata::SyncMode::kAlways;
      continue;
    }
    std::cerr << "Usage: strata_server [--port NUMBER] [--data-dir PATH] "
                 "[--sync-mode always|batch] [--threads NUMBER]\n";
    return flag == "--help" ? 0 : 2;
  }

  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    return 1;
  }

  std::unique_ptr<strata::DB> db;
  if (const auto status = strata::DB::Open(options, data, &db); !status.ok()) {
    std::cerr << "open database failed: " << status.message() << '\n';
    return 1;
  }
  std::stop_source stop;
  std::jthread signal_waiter([&](std::stop_token cancel) {
    while (!cancel.stop_requested()) {
      const timespec timeout{0, 100'000'000};
      if (sigtimedwait(&signals, nullptr, &timeout) >= 0) {
        stop.request_stop();
        return;
      }
    }
  });
  const auto status = strata::server::Serve(
      *db, port, threads, stop.get_token(),
      [](std::uint16_t bound) { std::cout << "LISTENING " << bound << std::endl; });
  signal_waiter.request_stop();
  if (!status.ok()) {
    std::cerr << status.message() << '\n';
    return 1;
  }
}
