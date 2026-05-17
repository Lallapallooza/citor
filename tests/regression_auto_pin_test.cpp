// Regression test for the auto-pin scope's restoration of the producer
// thread's affinity when the pool is destroyed on a different thread.

#include <gtest/gtest.h>

#include "citor/thread_pool.h"

#include <cstddef>
#include <memory>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

using namespace citor;

// If the pool is destroyed by a different thread than the one that
// producer-pinned, the producer thread's affinity stays pinned after pool
// lifetime. Verify by capturing the producer's CPU mask before pool ctor,
// dispatching to trigger the auto-pin, destroying the pool on a different
// thread, and asserting the producer thread's affinity matches its
// original mask.
TEST(
    RegressionAutoPin,
    AutoPinScopeRestoresOriginalAffinityEvenWhenScopeIsDestroyedOnAnotherThread) {
#ifndef __linux__
  GTEST_SKIP();
#else
  // Capture initial affinity.
  cpu_set_t before;
  CPU_ZERO(&before);
  ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(before), &before), 0);

  // Construct + dispatch on this thread to trigger auto-pin.
  auto pool = std::make_unique<ThreadPool>(2);
  pool->parallelFor<HintsDefaults>(
      0, 4, [](std::size_t /*lo*/, std::size_t /*hi*/) {});

  // Destroy on a different thread.
  std::thread destroyer([&] { pool.reset(); });
  destroyer.join();

  // Producer's affinity should be back to `before`.
  cpu_set_t after;
  CPU_ZERO(&after);
  ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(after), &after), 0);

  bool match = true;
  for (std::size_t i = 0; i < static_cast<std::size_t>(CPU_SETSIZE); ++i) {
    if (CPU_ISSET(i, &before) != CPU_ISSET(i, &after)) {
      match = false;
      break;
    }
  }
  EXPECT_TRUE(match) << "producer affinity not restored after cross-thread "
                        "pool destruction";
#endif
}
