#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>

#include "citor/cpos/parallel_reduce.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Exception propagation: a worker map throws -> the producer rethrows on join.
TEST(ParallelReduceExceptions,
     RethrowsBodyExceptionAtJoinAndPreservesWhatPayload) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;

  bool threwAsExpected = false;
  try {
    (void)pool.parallelReduce<HintsDefaults>(
        0, kN, 0.0,
        [](std::size_t lo, std::size_t /*hi*/) -> double {
          if (lo == 0) {
            throw std::runtime_error("test reduce exception");
          }
          return 0.0;
        },
        [](double a, double b) { return a + b; });
  } catch (const std::runtime_error &e) {
    threwAsExpected = std::string{e.what()} == "test reduce exception";
  } catch (...) {
    threwAsExpected = false;
  }
  EXPECT_TRUE(threwAsExpected);
}
