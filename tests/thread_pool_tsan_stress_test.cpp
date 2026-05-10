#include <gtest/gtest.h>

#include <cstddef>

#include "citor/thread_pool.h"

// Optional TSan stress: 10000 randomized construct/destroy cycles. Compiles in
// only under a ThreadSanitizer build because under non-TSan builds the loop is
// overkill for ctest. Under TSan it confirms the destructor's release/acquire
// chain is race-free.
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#define CITOR_TSAN_BUILD 1
#endif
#endif
#ifdef __SANITIZE_THREAD__
#define CITOR_TSAN_BUILD 1
#endif

#ifdef CITOR_TSAN_BUILD
TEST(ThreadPoolLifecycleTsanStress,
     RandomizedConstructAndDestroyLoopIsRaceFreeUnderTsan) {
  for (int i = 0; i < 10000; ++i) {
    const std::size_t p = static_cast<std::size_t>((i % 8) + 1);
    const citor::ThreadPool pool(p);
    EXPECT_GE(pool.participants(), 1U);
  }
}
#endif
