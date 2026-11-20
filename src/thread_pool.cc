#include "thread_pool.h"

#include <stdexcept>
#include <utility>

namespace strata {

ThreadPool::ThreadPool(std::size_t threads) {
  if (threads == 0) {
    throw std::invalid_argument("thread pool requires at least one worker");
  }
  workers_.reserve(threads);
  try {
    for (std::size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this] { Run(); });
    }
  } catch (...) {
    Shutdown();
    throw;
  }
}

ThreadPool::~ThreadPool() { Shutdown(); }

bool ThreadPool::Submit(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return false;
    }
    tasks_.push_back(std::move(task));
  }
  ready_.notify_one();
  return true;
}

void ThreadPool::Shutdown() {
  std::call_once(shutdown_, [this] {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) {
      worker.join();
    }
  });
}

void ThreadPool::Run() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    try {
      task();
    } catch (...) {
      // A failed task does not stop the queue from draining.
    }
  }
}

}  // namespace strata
