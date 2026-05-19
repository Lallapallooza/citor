#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

namespace {

// Records the OS thread id that ran each scan body invocation. The body
// receives `(chunkId, lo, hi, prefix, /*out*/)`; Pass 1 (chunkId <
// participants) computes a partial, Pass 2 applies the prefix. We capture
// the thread id on every call.
std::size_t distinctTidsInParallelScan(std::size_t participants,
                                       std::size_t n) {
  ThreadPool pool(participants);
  std::mutex mu;
  std::unordered_set<std::thread::id> ids;
  (void)pool.parallelScan<HintsDefaults>(
      n, std::int64_t{0},
      [&](std::size_t /*chunkId*/, std::size_t lo, std::size_t hi,
          std::int64_t prefix, std::int64_t * /*out*/) {
        const std::thread::id me = std::this_thread::get_id();
        {
          const std::lock_guard<std::mutex> lk(mu);
          ids.insert(me);
        }
        return prefix + static_cast<std::int64_t>(hi - lo);
      },
      [](std::int64_t a, std::int64_t b) { return a + b; });
  return ids.size();
}

} // namespace

// Two-pass scan: every participant runs Pass 1 then Pass 2 on its own slot,
// so each thread shows up at least once. A regression collapsing scan to a
// single-pass producer-only sequence would yield ids.size() == 1.
TEST(ParallelScanDistribution, BodyRunsOnEveryParticipantSlotThread) {
  constexpr std::size_t kN = 1u << 14;
  EXPECT_EQ(distinctTidsInParallelScan(2, kN), 2u);
  EXPECT_EQ(distinctTidsInParallelScan(4, kN), 4u);
}

// Single-participant pool runs the body inline on the caller; the two-pass
// shape collapses to one call per the `participants <= 1` short-circuit in
// `parallelScan`.
TEST(ParallelScanDistribution, SingleParticipantPoolRunsBodyOnCallerOnly) {
  EXPECT_EQ(distinctTidsInParallelScan(1, 1u << 12), 1u);
}
