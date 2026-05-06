#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/cpos/fork_join.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CcdLocalForkJoinHints;
using citor::ThreadPool;

// CCD-local affinity hint reaches the forkJoin entry without crashing or
// stalling. The probe order biases stealing toward same-CCD victims; the test
// verifies end-to-end correctness on the affinity-aware path.
TEST(ForkJoinAffinity, CcdLocalAffinityHintRunsTasksAndJoinsOnHostCcd) {
  ThreadPool pool(4);
  std::atomic<int> sum{0};
  pool.forkJoin<CcdLocalForkJoinHints>(
      [&]() { sum.fetch_add(1, std::memory_order_relaxed); },
      [&]() { sum.fetch_add(2, std::memory_order_relaxed); },
      [&]() { sum.fetch_add(4, std::memory_order_relaxed); },
      [&]() { sum.fetch_add(8, std::memory_order_relaxed); });
  EXPECT_EQ(sum.load(), 15);
}
