#include "thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>

namespace strata {
namespace {

TEST(ThreadPool, DrainsAcceptedTasksAndRejectsNewTasksAfterShutdown) {
  ThreadPool pool(2);
  std::atomic<int> completed = 0;
  std::promise<void> release;
  auto ready = release.get_future().share();
  for (int i = 0; i < 32; ++i) {
    EXPECT_TRUE(pool.Submit([ready, &completed] {
      ready.wait();
      ++completed;
    }));
  }
  EXPECT_TRUE(pool.Submit([] { throw std::runtime_error("task failure"); }));
  EXPECT_TRUE(pool.Submit([&completed] { ++completed; }));
  std::jthread shutdown([&] { pool.Shutdown(); });
  release.set_value();
  shutdown.join();
  EXPECT_EQ(completed, 33);
  EXPECT_FALSE(pool.Submit([] {}));
}

}  // namespace
}  // namespace strata
