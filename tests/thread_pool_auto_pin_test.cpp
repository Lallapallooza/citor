#include <gtest/gtest.h>

#include <cstddef>

#ifdef __linux__
#include <sched.h>
#endif

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

struct LifecycleAutoPinHints : HintsDefaults {
  // chunk == 0 is the gate that fires auto-pin on the producer.
  static constexpr bool cancellationChecks = false;
};

// Auto-pin must restore the producer thread's original affinity even when a
// single producer thread used multiple pools. The TLS state must not be
// overwritten by a second pool's autoPinProducerOnce in a way that drops the
// first pool's saved affinity.
TEST(ThreadPoolLifecycleAutoPin,
     AutoPinRestoresOriginalCallerAffinityAfterPoolSwitch) {
#ifdef __linux__
  cpu_set_t original{};
  CPU_ZERO(&original);
  ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(original), &original),
            0);
  if (CPU_COUNT(&original) < 2) {
    GTEST_SKIP() << "needs >= 2 allowed CPUs to observe pinning behavior";
  }
  const auto originalCount = static_cast<std::size_t>(CPU_COUNT(&original));

  {
    ThreadPool p1(2);
    ThreadPool p2(2);

    // chunk == 0 triggers ensureProducerPinnedForChunkZero on each pool's hot
    // path.
    auto noop = [](std::size_t /*lo*/, std::size_t /*hi*/) noexcept {};
    p1.parallelFor<LifecycleAutoPinHints>(0, 128, noop);
    p2.parallelFor<LifecycleAutoPinHints>(0, 128, noop);
  }

  cpu_set_t after{};
  CPU_ZERO(&after);
  ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(after), &after), 0);
  EXPECT_EQ(static_cast<std::size_t>(CPU_COUNT(&after)), originalCount)
      << "producer affinity not restored after using two pools sequentially";
#else
  GTEST_SKIP() << "auto-pin behavior is Linux-only";
#endif
}
