#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace strata {

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t threads);
  ~ThreadPool();

  bool Submit(std::function<void()> task);
  void Shutdown();

 private:
  void Run();

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
  std::once_flag shutdown_;
};

}  // namespace strata
