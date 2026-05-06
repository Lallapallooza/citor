#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <stdexcept>

#include "citor/cpos/parallel_scan.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Pass-1 throw inside a worker leaves it stamped at pass2Stamp; the producer's
// seq-reduce then runs unconditionally and may invoke `prefix` on partials that
// were never written. The producer must not call `prefix` after a pass-1
// failure -- the call should propagate the captured exception without any
// further user-callable being invoked.
TEST(ParallelScanExceptionSafety,
     Pass1ExceptionSuppressesEveryPass2PrefixInvocation) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "test requires multi-participant pool";
  }

  std::atomic<int> prefixCalls{0};
  auto body = [](std::size_t slot, std::size_t /*lo*/, std::size_t /*hi*/,
                 std::int64_t /*init*/,
                 std::int64_t * /*out*/) -> std::int64_t {
    if (slot == 1U) {
      throw std::runtime_error("pass1 failed");
    }
    return 1;
  };
  auto prefix = [&prefixCalls](std::int64_t a, std::int64_t b) {
    prefixCalls.fetch_add(1, std::memory_order_relaxed);
    return a + b;
  };

  auto fut = std::async(std::launch::async, [&] {
    EXPECT_THROW((void)pool.parallelScan<HintsDefaults>(1024U, std::int64_t{0},
                                                        body, prefix),
                 std::runtime_error);
  });
  ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready)
      << "parallelScan deadlocked when prefix would otherwise have run on "
         "partial state";
  fut.get();
  EXPECT_EQ(prefixCalls.load(), 0);
}

// User `prefix` throwing during the producer's sequential reduce must not
// deadlock workers that are still spinning on `prefixesPublished`. The producer
// must publish the flag before unwinding so peers can proceed (or short-circuit
// on the captured exception).
TEST(ParallelScanExceptionSafety,
     Pass2PrefixExceptionPropagatesWithoutDeadlocking) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "test requires multi-participant pool";
  }

  auto body = [](std::size_t /*slot*/, std::size_t /*lo*/, std::size_t /*hi*/,
                 std::int64_t /*init*/,
                 std::int64_t * /*out*/) -> std::int64_t { return 1; };
  auto prefix = [](std::int64_t /*a*/, std::int64_t /*b*/) -> std::int64_t {
    throw std::runtime_error("prefix failed");
  };

  auto fut = std::async(std::launch::async, [&] {
    EXPECT_THROW((void)pool.parallelScan<HintsDefaults>(1024U, std::int64_t{0},
                                                        body, prefix),
                 std::runtime_error);
  });
  ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready)
      << "parallelScan deadlocked when user prefix threw during producer "
         "reduce";
  fut.get();
}
