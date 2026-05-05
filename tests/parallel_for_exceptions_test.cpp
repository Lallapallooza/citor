#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Exception propagation: a worker throws -> the producer rethrows on join.
TEST(ParallelForExceptions,
     RethrowsBodyExceptionAtJoinAndPreservesWhatPayload) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 256;

  bool threwAsExpected = false;
  try {
    pool.parallelFor<HintsDefaults>(
        0, kN, [](std::size_t lo, std::size_t /*hi*/) {
          if (lo == 0) {
            throw std::runtime_error("test exception");
          }
        });
  } catch (const std::runtime_error &e) {
    threwAsExpected = std::string{e.what()} == "test exception";
  } catch (...) {
    threwAsExpected = false;
  }
  EXPECT_TRUE(threwAsExpected);
}
