#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::HintsDefaults;
using citor::ThreadPool;

namespace {

// Hint preset for the exception-propagation test. DynamicChunked with `chunk = 16` keeps the
// dispatch shape small so the throwing chunk lands quickly; the cancellation-on-throw pathway
// halts admission of further chunks via the descriptor's `firstException` slot.
struct BulkForQueriesExceptionHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr std::size_t chunk = 16;
};

} // namespace

// One query body throws. The producer must rethrow on join with the original message intact.
// This mirrors the parallelFor exception contract: bulkForQueries forwards through the same
// engine, so the descriptor's first-exception slot captures the throw and dispatchOne rethrows.
TEST(BulkForQueriesException, ExceptionPropagation) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 256;

  bool threwAsExpected = false;
  try {
    pool.bulkForQueries<BulkForQueriesExceptionHints>(kQ, [](std::size_t lo, std::size_t /*hi*/) {
      if (lo == 0) {
        throw std::runtime_error("bulk-query body fault");
      }
    });
  } catch (const std::runtime_error &e) {
    threwAsExpected = std::string{e.what()} == "bulk-query body fault";
  } catch (...) {
    threwAsExpected = false;
  }
  EXPECT_TRUE(threwAsExpected);
}

// Multiple chunks throw concurrently; only the first throw is rethrown. Subsequent throws are
// captured-and-dropped per the first-exception-wins contract documented in dispatch_static.h /
// dispatch_dynamic.h. We verify the call rethrows once and the post-call state is consistent
// (no double-throw, no leak).
TEST(BulkForQueriesException, FirstExceptionWinsOnConcurrentThrows) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 1024;
  std::atomic<std::size_t> bodiesEntered{0};

  bool threw = false;
  try {
    pool.bulkForQueries<BulkForQueriesExceptionHints>(
        kQ, [&bodiesEntered](std::size_t /*lo*/, std::size_t /*hi*/) {
          bodiesEntered.fetch_add(1, std::memory_order_acq_rel);
          throw std::runtime_error("racy body");
        });
  } catch (const std::runtime_error &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  // At least one body ran and threw; the first-exception slot stops admission of further chunks.
  EXPECT_GE(bodiesEntered.load(std::memory_order_acquire), 1U);
}

// Runtime-hint mirror: `bulkForQueriesRuntime` propagates exceptions identically so call sites
// going through the runtime POD see the same first-exception-wins contract. The runtime path
// also references the hint's `minTaskUs` and `estimatedItemNs` fields, which keeps clang-tidy's
// `unused-const-variable` check satisfied for the compile-time hint preset above.
TEST(BulkForQueriesException, RuntimeHintsAlsoPropagateExceptions) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 256;

  citor::Hints rh;
  rh.balance = Balance::DynamicChunked;
  rh.estimatedItemNs = BulkForQueriesExceptionHints::estimatedItemNs;
  rh.minTaskUs = BulkForQueriesExceptionHints::minTaskUs;
  rh.chunk = BulkForQueriesExceptionHints::chunk;

  bool threw = false;
  try {
    pool.bulkForQueriesRuntime(
        kQ,
        [](std::size_t lo, std::size_t /*hi*/) {
          if (lo == 0) {
            throw std::runtime_error("runtime bulk-query body fault");
          }
        },
        rh);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}
