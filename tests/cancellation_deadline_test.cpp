#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

#include "citor/cancellation.h"

using citor::Deadline;

// Deadline default-constructs to never-expires; explicit thresholds expire on
// TSC advance.
TEST(ParallelCancellationDeadline, DefaultConstructedDeadlineNeverExpires) {
  const Deadline d;
  EXPECT_FALSE(d.expired());
  EXPECT_EQ(d.threshold(), std::numeric_limits<std::uint64_t>::max());
}

#if defined(__x86_64__) || defined(_M_X64)
TEST(ParallelCancellationDeadline, ExpiresWhenThresholdIsInThePast) {
  // Threshold a few microseconds in the past relative to "now": already
  // expired.
  const std::uint64_t now = __rdtsc();
  // Defensive: if the TSC is somehow at zero, skip rather than underflow.
  if (now > 1024U) {
    const Deadline expired{now - 1024U};
    EXPECT_TRUE(expired.expired());
  }
}

TEST(ParallelCancellationDeadline, DoesNotExpireWhenThresholdIsInTheFuture) {
  // Threshold ~1 second in the future at any plausible TSC frequency: not yet
  // expired.
  const std::uint64_t now = __rdtsc();
  const Deadline future{now + (std::uint64_t{1} << 40)};
  EXPECT_FALSE(future.expired());
}

TEST(ParallelCancellationDeadline,
     FromMillisDeadlineExpiresAfterSleepingForMatchingDuration) {
  // 1 ms deadline: not expired immediately, expired after a 5 ms sleep.
  const Deadline d = Deadline::fromMillis(1);
  EXPECT_FALSE(d.expired());
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(d.expired());
}

TEST(ParallelCancellationDeadline,
     FromMillisDeadlineWithFutureThresholdIsNotImmediatelyExpired) {
  // 10 s deadline: not expired immediately. Don't actually wait it out.
  const Deadline d = Deadline::fromMillis(10000);
  EXPECT_FALSE(d.expired());
}
#endif
