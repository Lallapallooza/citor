// Regression tests for the auto-pin scope's restoration of the producer
// thread's affinity when the pool is destroyed on a different thread.

#include <gtest/gtest.h>

#include "citor/thread_pool.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include "test_support_subprocess.h"
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

// The pool that pinned a producer thread must not restore that thread's
// affinity after the thread has exited: its `pthread_t` is no longer valid.
// The producer runs on a caller-supplied stack that is unmapped after the
// join, so glibc's thread descriptor (which lives on that stack) is gone and
// any use of the stale handle faults. Runs in a child process so a crash
// reads as a test failure instead of taking down the gtest binary.
TEST(RegressionAutoPin,
     PoolDestroyedAfterProducerThreadExitsSkipsAffinityRestore) {
#ifndef __linux__
  GTEST_SKIP();
#else
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed),
            0);
  if (CPU_COUNT(&allowed) < 2) {
    GTEST_SKIP() << "needs >= 2 allowed CPUs for the producer to be pinned";
  }

  const auto outcome = citor_test_support::runInChildWithTimeout(
      [] {
        constexpr std::size_t kStackSize = std::size_t{1} << 20U;
        void *stack = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) {
          _exit(2);
        }

        std::unique_ptr<ThreadPool> pool;
        auto producer = [](void *arg) -> void * {
          auto &slot = *static_cast<std::unique_ptr<ThreadPool> *>(arg);
          slot = std::make_unique<ThreadPool>(2);
          slot->parallelFor<HintsDefaults>(
              0, 128, [](std::size_t /*lo*/, std::size_t /*hi*/) {});
          return nullptr;
        };

        pthread_attr_t attr;
        pthread_t thread;
        if (pthread_attr_init(&attr) != 0 ||
            pthread_attr_setstack(&attr, stack, kStackSize) != 0 ||
            pthread_create(&thread, &attr, producer, &pool) != 0) {
          _exit(3);
        }
        pthread_attr_destroy(&attr);
        if (pthread_join(thread, nullptr) != 0 ||
            munmap(stack, kStackSize) != 0) {
          _exit(4);
        }
        pool.reset();
      },
      std::chrono::seconds(30));
  EXPECT_EQ(outcome, citor_test_support::ChildOutcome::Exited0)
      << "pool destruction touched the exited producer's thread handle";
#endif
}
