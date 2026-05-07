#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "citor/cancellation.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::HintsDefaults;
using citor::ThreadPool;

// One task throws. Destruction must complete cleanly; lastDetachedException
// returns a non-null exception_ptr that rethrows the original message.
TEST(SubmitDetachedLifecycle,
     ExceptionThrownByDetachedBodyIsCapturedAndRethrowableViaResult) {
  std::exception_ptr captured;
  {
    ThreadPool pool(2);
    pool.submitDetached<HintsDefaults>(
        []() { throw std::runtime_error("detached body threw"); });
    // Wait for the body to complete by destroying the pool below; capture the
    // slot afterwards. Spin briefly here so the body has a chance to run before
    // we sample (the dtor will also drain, but we want to assert the slot read
    // is safe before destruction).
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
TEST(SubmitDetachedLifecycle,
     DetachedSubmissionWithPreCancelledTokenSkipsBodyEntirely) {
  std::atomic<bool> ran{false};
  {
    ThreadPool pool(2);
    CancellationToken tok = CancellationToken::makeOwned();
    (void)tok.request_stop();
    pool.submitDetached<HintsDefaults>(
        [&ran]() { ran.store(true, std::memory_order_release); }, tok);
  }
  EXPECT_FALSE(ran.load(std::memory_order_acquire));
}

// Launch a slow detached task, immediately destroy the pool. The destructor
// must block until the body completes; the post-scope flag must be true.
TEST(SubmitDetachedLifecycle,
     PoolDestructorBlocksUntilEveryDetachedBodyHasReturned) {
  std::atomic<bool> bodyStarted{false};
  std::atomic<bool> bodyFinished{false};
  {
    ThreadPool pool(2);
    pool.submitDetached<HintsDefaults>([&bodyStarted, &bodyFinished]() {
      bodyStarted.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      bodyFinished.store(true, std::memory_order_release);
    });
    // Spin briefly until the body has begun so we can observe the dtor truly
    // waiting on it.
    for (int i = 0; i < 500; ++i) {
      if (bodyStarted.load(std::memory_order_acquire)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(bodyStarted.load(std::memory_order_acquire));
    EXPECT_FALSE(bodyFinished.load(std::memory_order_acquire));
    // Pool destructor runs at scope exit; it must block until bodyFinished
    // flips.
  }
  EXPECT_TRUE(bodyFinished.load(std::memory_order_acquire));
}
