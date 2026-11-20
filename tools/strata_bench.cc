#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "strata/db.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kPreloadedKeys = 10'000;

bool Parse(std::string_view text, std::size_t* value) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() && *value > 0;
}

std::string Key(std::uint64_t number) {
  std::string key(16, '0');
  const auto encoded = std::to_chars(key.data(), key.data() + key.size(), number, 16);
  std::rotate(key.data(), encoded.ptr, key.data() + key.size());
  return key;
}

double Percentile(const std::vector<std::uint64_t>& sorted, double fraction) {
  const auto index = static_cast<std::size_t>(std::ceil(sorted.size() * fraction)) - 1;
  return sorted[index] / 1000.0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5 && argc != 6) {
    std::cerr << "Usage: strata_bench fillseq|fillrandom|readrandom|mixed THREADS "
                 "always|batch FRESH_DB_PATH [OPS=1000000]\n";
    return 2;
  }
  const std::string_view workload = argv[1];
  const std::string_view sync = argv[3];
  const bool read_only = workload == "readrandom";
  const bool mixed = workload == "mixed";
  std::size_t threads = 0;
  std::size_t operations = 1'000'000;
  if ((!read_only && !mixed && workload != "fillseq" && workload != "fillrandom") ||
      (sync != "always" && sync != "batch") || !Parse(argv[2], &threads) || threads > 256 ||
      (argc == 6 && !Parse(argv[5], &operations)) || operations > 25'000'000) {
    return 2;
  }
  const std::string payload(100, 'v');
  const auto population = std::min(operations, kPreloadedKeys);
  if (read_only || mixed) {
    std::unique_ptr<strata::DB> seed;
    if (!strata::DB::Open({}, argv[4], &seed).ok()) {
      return 1;
    }
    for (std::size_t i = 0; i < population; ++i) {
      if (!seed->Put(Key(i), payload).ok()) {
        return 1;
      }
    }
    if (!seed->Flush().ok()) {
      return 1;
    }
  }

  strata::Options options;
  options.sync_mode = sync == "batch" ? strata::SyncMode::kBatch : strata::SyncMode::kAlways;
  std::unique_ptr<strata::DB> db;
  if (!strata::DB::Open(options, argv[4], &db).ok()) {
    return 1;
  }

  std::atomic<bool> failed = false;
  Clock::time_point begin;
  Clock::time_point end;
  const auto begin_timer = [&]() noexcept { begin = Clock::now(); };
  const auto end_timer = [&]() noexcept { end = Clock::now(); };
  std::barrier start(static_cast<std::ptrdiff_t>(threads + 1), begin_timer);
  std::barrier finish(static_cast<std::ptrdiff_t>(threads + 1), end_timer);
  std::vector<std::uint64_t> latency(operations);
  std::vector<std::jthread> workers;
  for (std::size_t thread = 0; thread < threads; ++thread) {
    workers.emplace_back([&, thread] {
      std::mt19937_64 random(4831 + thread);
      std::string result;
      start.arrive_and_wait();
      for (std::size_t i = thread; i < operations; i += threads) {
        const bool read = read_only || (mixed && (i / threads + thread) % 2 == 0);
        const auto number = read_only || mixed      ? random() % population
                            : workload == "fillseq" ? i
                                                    : random();
        const auto key = Key(number);
        const auto before = Clock::now();
        const auto status = read ? db->Get(key, &result) : db->Put(key, payload);
        latency[i] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - before).count();
        if (!status.ok() || (read && result != payload)) {
          failed = true;
          break;
        }
      }
      finish.arrive_and_wait();
    });
  }
  start.arrive_and_wait();
  finish.arrive_and_wait();
  workers.clear();
  if (failed) {
    std::cerr << "benchmark operation failed\n";
    return 1;
  }
  const auto seconds = std::chrono::duration<double>(end - begin).count();
  std::sort(latency.begin(), latency.end());
  std::cout << workload << ',' << threads << ',' << sync << ',' << operations << ',' << std::fixed
            << std::setprecision(3) << operations / seconds << ',' << Percentile(latency, 0.5)
            << ',' << Percentile(latency, 0.99) << ',' << Percentile(latency, 0.999) << '\n';
}
