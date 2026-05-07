#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "citor/cancellation.h"

using citor::CancellationToken;

// CancellationToken returns false initially; after request_stop, returns true.
TEST(ParallelCancellationToken, TokenStartsNotStoppedAndTransitionsOnRequest) {
  CancellationToken tok = CancellationToken::makeOwned();
  EXPECT_FALSE(tok.stop_requested());
  EXPECT_TRUE(tok.request_stop());
  EXPECT_TRUE(tok.stop_requested());
}

// Subsequent request_stop calls return false (the first call wins the CAS).
TEST(ParallelCancellationToken,
     SecondRequestStopOnAlreadyStoppedTokenReturnsFalse) {
  CancellationToken tok = CancellationToken::makeOwned();
  EXPECT_TRUE(tok.request_stop());
  EXPECT_FALSE(tok.request_stop());
  EXPECT_TRUE(tok.stop_requested());
}

// Copies of an owned token share the underlying state: stopping the copy also
// stops the original. Default-constructed tokens are sentinel and are not
// subject to this contract.
TEST(ParallelCancellationToken,
     CopiedTokensShareUnderlyingStopFlagWithOriginal) {
  const CancellationToken a = CancellationToken::makeOwned();
  CancellationToken b = a;
  EXPECT_FALSE(a.stop_requested());
  EXPECT_FALSE(b.stop_requested());
  EXPECT_TRUE(b.request_stop());
  EXPECT_TRUE(a.stop_requested());
  EXPECT_TRUE(b.stop_requested());
}

// Stop signal becomes visible to a worker thread (release / acquire
// synchronization).
TEST(ParallelCancellationToken,
     RequestStopFromOneThreadIsVisibleToReaderOnAnotherThread) {
  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<int> observed{-1};
  std::thread worker([&]() {
    while (!tok.stop_requested()) {
      // spin until producer stops the token
    }
    observed.store(1, std::memory_order_release);
  });
  tok.request_stop();
  worker.join();
  EXPECT_EQ(observed.load(std::memory_order_acquire), 1);
}
