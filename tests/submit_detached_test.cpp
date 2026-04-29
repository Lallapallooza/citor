#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>

#include "citor/cancellation.h"
#include "citor/cpos/submit_detached.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::HintsDefaults;
using citor::submitDetached;
using citor::ThreadPool;

namespace {

constexpr std::size_t kManyTasks = 100;

} // namespace

// 100 detached tasks each increment a shared counter. The pool destructor must wait for every body
// to retire so the post-scope read sees the full count.
TEST(SubmitDetached, SubmitDetachedSimple) {
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

// One task throws. Destruction must complete cleanly; lastDetachedException returns a non-null
// exception_ptr that rethrows the original message.
TEST(SubmitDetached, SubmitDetachedExceptionCaptured) {
  std::exception_ptr captured;
  {
    ThreadPool pool(2);
    pool.submitDetached<HintsDefaults>([]() { throw std::runtime_error("detached body threw"); });
    // Wait for the body to complete by destroying the pool below; capture the slot afterwards.
    // Spin briefly here so the body has a chance to run before we sample (the dtor will also
    // drain, but we want to assert the slot read is safe before destruction).
    for (int i = 0; i < 200; ++i) {
      if (pool.lastDetachedException() != nullptr) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    captured = pool.lastDetachedException();
  }
  ASSERT_NE(captured, nullptr);
  try {
    std::rethrow_exception(captured);
    FAIL() << "rethrow_exception did not throw";
  } catch (const std::runtime_error &e) {
    EXPECT_STREQ(e.what(), "detached body threw");
  } catch (...) {
    FAIL() << "captured exception was not std::runtime_error";
  }
}

// A pre-cancelled token short-circuits the body before any user code runs.
TEST(SubmitDetached, SubmitDetachedCancellation) {
  std::atomic<bool> ran{false};
  {
    ThreadPool pool(2);
    CancellationToken tok = CancellationToken::makeOwned();
    (void)tok.request_stop();
    pool.submitDetached<HintsDefaults>([&ran]() { ran.store(true, std::memory_order_release); },
                                       tok);
  }
  EXPECT_FALSE(ran.load(std::memory_order_acquire));
}

// Multiple producer threads concurrently submit detached tasks to one pool. The total counter
// after destruction equals the total submitted.
TEST(SubmitDetached, SubmitDetachedConcurrentSubmit) {
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
          pool.submitDetached<HintsDefaults>(
              [&counter]() { counter.fetch_add(1, std::memory_order_acq_rel); });
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

// Launch a slow detached task, immediately destroy the pool. The destructor must block until the
// body completes; the post-scope flag must be true.
TEST(SubmitDetached, SubmitDetachedDestructionWaitsForInflight) {
  std::atomic<bool> bodyStarted{false};
  std::atomic<bool> bodyFinished{false};
  {
    ThreadPool pool(2);
    pool.submitDetached<HintsDefaults>([&bodyStarted, &bodyFinished]() {
      bodyStarted.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      bodyFinished.store(true, std::memory_order_release);
    });
    // Spin briefly until the body has begun so we can observe the dtor truly waiting on it.
    for (int i = 0; i < 500; ++i) {
      if (bodyStarted.load(std::memory_order_acquire)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(bodyStarted.load(std::memory_order_acquire));
    EXPECT_FALSE(bodyFinished.load(std::memory_order_acquire));
    // Pool destructor runs at scope exit; it must block until bodyFinished flips.
  }
  EXPECT_TRUE(bodyFinished.load(std::memory_order_acquire));
}

// Exercise the CPO surface (tag_invoke dispatch) so the same engine is reachable from both the
// member-template call and the customization-point call. The CPO is a callable object whose call
// operator is templated on `HintsT`; invoking it requires the explicit member-template syntax.
TEST(SubmitDetached, CpoDispatchRoutesToPool) {
  std::atomic<int> seen{0};
  {
    ThreadPool pool(2);
    submitDetached.operator()<HintsDefaults>(
        pool, [&seen]() { seen.store(7, std::memory_order_release); });
  }
  EXPECT_EQ(seen.load(std::memory_order_acquire), 7);
}
