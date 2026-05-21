#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "citor/coro.h"
#include "citor/thread_pool.h"

namespace {

using citor::ThreadPool;

TEST(CoroWrapper, AsyncReturnsValue) {
  ThreadPool pool(4);
  auto task = [&]() -> citor::coro::Task<int> {
    const int v = co_await citor::coro::async(pool, [] { return 42; });
    co_return v;
  };
  EXPECT_EQ(citor::coro::syncWait(task()), 42);
}

TEST(CoroWrapper, ParallelForFillsBuffer) {
  ThreadPool pool(4);
  std::vector<std::int64_t> buf(1024, 0);
  auto task = [&]() -> citor::coro::Task<void> {
    co_await citor::coro::parallelFor(pool, std::size_t{0}, buf.size(),
                                      [&](std::size_t lo, std::size_t hi) {
                                        for (std::size_t i = lo; i < hi; ++i) {
                                          buf[i] = static_cast<std::int64_t>(i);
                                        }
                                      });
  };
  citor::coro::syncWait(task());
  for (std::size_t i = 0; i < buf.size(); ++i) {
    ASSERT_EQ(buf[i], static_cast<std::int64_t>(i));
  }
}

TEST(CoroWrapper, ParallelReduceReturnsSum) {
  ThreadPool pool(4);
  constexpr std::size_t n = 10'000;
  auto task = [&]() -> citor::coro::Task<std::int64_t> {
    const std::int64_t sum = co_await citor::coro::parallelReduce(
        pool, std::size_t{0}, n, std::int64_t{0},
        [](std::size_t lo, std::size_t hi) {
          std::int64_t s = 0;
          for (std::size_t i = lo; i < hi; ++i) {
            s += static_cast<std::int64_t>(i);
          }
          return s;
        },
        [](std::int64_t a, std::int64_t b) { return a + b; });
    co_return sum;
  };
  const std::int64_t expected =
      static_cast<std::int64_t>(n) * static_cast<std::int64_t>(n - 1) / 2;
  EXPECT_EQ(citor::coro::syncWait(task()), expected);
}

TEST(CoroWrapper, ForkJoinRunsBothBranches) {
  ThreadPool pool(4);
  std::atomic<int> a{0};
  std::atomic<int> b{0};
  auto task = [&]() -> citor::coro::Task<void> {
    co_await citor::coro::forkJoin(
        pool, [&] { a.store(1, std::memory_order_relaxed); },
        [&] { b.store(2, std::memory_order_relaxed); });
  };
  citor::coro::syncWait(task());
  EXPECT_EQ(a.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(b.load(std::memory_order_relaxed), 2);
}

TEST(CoroWrapper, ExceptionPropagates) {
  ThreadPool pool(2);
  auto task = [&]() -> citor::coro::Task<int> {
    const int v = co_await citor::coro::async(
        pool, []() -> int { throw std::runtime_error("boom"); });
    co_return v;
  };
  EXPECT_THROW(citor::coro::syncWait(task()), std::runtime_error);
}

// Stress test: hammer syncWait to expose any race between the producer
// destroying the coroutine handle and the worker thread unwinding the
// symmetric-transfer chain back through h.resume(). Under TSan this would
// surface as a data race on the coroutine frame's metadata. Under release
// this would surface as a SIGSEGV/UAF eventually. Run many tight iterations
// so the race window has many chances to open.
TEST(CoroWrapper, SyncWaitStressHammer) {
  ThreadPool pool(4);
  constexpr int kIterations = 5000;
  for (int i = 0; i < kIterations; ++i) {
    auto task = [&]() -> citor::coro::Task<int> {
      const int v = co_await citor::coro::async(pool, [i] { return i + 1; });
      co_return v;
    };
    const int got = citor::coro::syncWait(task());
    ASSERT_EQ(got, i + 1);
  }
}

} // namespace
