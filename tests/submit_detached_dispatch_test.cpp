#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <thread>

#include "citor/cpos/submit_detached.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::submitDetached;
using citor::ThreadPool;

namespace {

constexpr std::size_t kManyTasks = 100;

} // namespace

// 100 detached tasks each increment a shared counter. The pool destructor must
// wait for every body to retire so the post-scope read sees the full count.
TEST(SubmitDetachedDispatch,
     ManyDetachedTasksAllRunToCompletionBeforePoolDestructorReturns) {
  std::atomic<std::size_t> counter{0};
  {
    ThreadPool pool(2);
    for (std::size_t i = 0; i < kManyTasks; ++i) {
      pool.submitDetached<HintsDefaults>(
          [&counter]() { counter.fetch_add(1, std::memory_order_acq_rel); });
    }
    // Pool destructor drains the in-flight counter to zero before returning.
  }
  EXPECT_EQ(counter.load(std::memory_order_acquire), kManyTasks);
}

// Multiple producer threads concurrently submit detached tasks to one pool. The
// total counter after destruction equals the total submitted.
TEST(SubmitDetachedDispatch,
     ConcurrentProducersSubmittingDetachedTasksAllSucceedAndEveryBodyRuns) {
  constexpr std::size_t kProducers = 4;
  constexpr std::size_t kPerProducer = 50;
  std::atomic<std::size_t> counter{0};
  {
    ThreadPool pool(4);
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
      producers.emplace_back([&pool, &counter]() {
        for (std::size_t i = 0; i < kPerProducer; ++i) {
          pool.submitDetached<HintsDefaults>([&counter]() {
            counter.fetch_add(1, std::memory_order_acq_rel);
          });
        }
      });
    }
    for (auto &t : producers) {
      t.join();
    }
    // Pool destructor drains the in-flight counter.
  }
  EXPECT_EQ(counter.load(std::memory_order_acquire), kProducers * kPerProducer);
}

// Exercise the CPO surface (tag_invoke dispatch) so the same engine is
// reachable from both the member-template call and the customization-point
// call. The CPO is a callable object whose call operator is templated on
// `HintsT`; invoking it requires the explicit member-template syntax.
TEST(SubmitDetachedDispatch, CpoCallRoutesToSameDetachedQueueAsMemberCall) {
  std::atomic<int> seen{0};
  {
    ThreadPool pool(2);
    submitDetached.operator()<HintsDefaults>(
        pool, [&seen]() { seen.store(7, std::memory_order_release); });
  }
  EXPECT_EQ(seen.load(std::memory_order_acquire), 7);
}
