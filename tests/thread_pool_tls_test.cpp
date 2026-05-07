#include <gtest/gtest.h>

#include "citor/thread_pool.h"

using citor::ThreadPool;

// `workerIndex()` is 0 outside any pool; `insidePoolWorker()` is false. Without
// a primitive that flips the TLS state these are the only observable values.
TEST(ThreadPoolLifecycleTls,
     WorkerIndexReadsZeroOnThreadsThatHaveNeverEnteredAPool) {
  const ThreadPool pool(2);
  EXPECT_GE(pool.participants(), 1U);
  EXPECT_EQ(ThreadPool::workerIndex(), 0U);
  EXPECT_FALSE(ThreadPool::insidePoolWorker());
}
