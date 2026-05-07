#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include "citor/thread_pool.h"

using citor::ThreadPool;

// Idle pool CPU usage must be under 1% of one core. Sample 5 s to keep ctest
// snappy; the acceptance contract is the percentage, not the wall time.
TEST(ThreadPoolLifecycleIdle,
     IdlePoolConsumesUnderOnePercentCpuOverHalfSecondWindow) {
#ifdef __linux__
  const ThreadPool pool(16);
  EXPECT_GE(pool.participants(), 1U);

  rusage before{};
  rusage after{};
  ASSERT_EQ(getrusage(RUSAGE_SELF, &before), 0);

  const auto wallStart = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(5));
  const auto wallEnd = std::chrono::steady_clock::now();

  ASSERT_EQ(getrusage(RUSAGE_SELF, &after), 0);

  const auto wallSecs =
      std::chrono::duration<double>(wallEnd - wallStart).count();
  const double userSecs =
      (static_cast<double>(after.ru_utime.tv_sec) +
       static_cast<double>(after.ru_utime.tv_usec) / 1.0e6) -
      (static_cast<double>(before.ru_utime.tv_sec) +
       static_cast<double>(before.ru_utime.tv_usec) / 1.0e6);
  const double sysSecs = (static_cast<double>(after.ru_stime.tv_sec) +
                          static_cast<double>(after.ru_stime.tv_usec) / 1.0e6) -
                         (static_cast<double>(before.ru_stime.tv_sec) +
                          static_cast<double>(before.ru_stime.tv_usec) / 1.0e6);
  const double cpuSecs = userSecs + sysSecs;
  const double burnFraction = wallSecs > 0.0 ? (cpuSecs / wallSecs) : 0.0;

  EXPECT_LT(burnFraction, 0.01)
      << "idle pool burned " << burnFraction << " core-seconds per wall second";
#else
  GTEST_SKIP() << "idle burn measurement uses getrusage; non-Linux platform";
#endif
}
