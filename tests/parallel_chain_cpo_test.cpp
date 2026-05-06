#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cancellation.h"
#include "citor/chain.h"
#include "citor/cpos/parallel_chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::ChainHints;
using citor::ChainHintsDefaults;
using citor::globalStage;
using citor::staticStage;
using citor::ThreadPool;

// Member-call vs CPO-call equivalence and the runtime-hints sibling. Both
// surfaces must drive the same engine and produce the same observable per-slot
// output for equivalent inputs.

// Member-call and CPO-call must produce equivalent observable output.
TEST(ParallelChainCpoEquivalence,
     MemberCallAndCpoCallProduceIdenticalPerSlotSums) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;
  const std::size_t participants = pool.participants();

  std::vector<std::atomic<std::int64_t>> memberSums(participants);
  std::vector<std::atomic<std::int64_t>> cpoSums(participants);
  for (std::size_t i = 0; i < participants; ++i) {
    memberSums[i].store(0, std::memory_order_relaxed);
    cpoSums[i].store(0, std::memory_order_relaxed);
  }

  auto stage0Body = [](std::vector<std::atomic<std::int64_t>> &sums) {
    return [&sums](std::size_t /*stageIdx*/, std::uint32_t slot, std::size_t lo,
                   std::size_t hi) {
      sums[slot].fetch_add(static_cast<std::int64_t>(hi - lo),
                           std::memory_order_acq_rel);
    };
  };
  auto stage1Body = [](std::vector<std::atomic<std::int64_t>> &sums) {
    return [&sums](std::size_t /*stageIdx*/, std::uint32_t slot,
                   std::size_t /*lo*/, std::size_t /*hi*/) {
      sums[slot].fetch_add(static_cast<std::int64_t>(slot) + 1,
                           std::memory_order_acq_rel);
    };
  };

  // Member call.
  pool.parallelChain<ChainHintsDefaults>(
      kN, globalStage("m0", stage0Body(memberSums)),
      staticStage("m1", stage1Body(memberSums)));

  // CPO call.
  citor::parallelChain.template operator()<ChainHintsDefaults>(
      pool, kN, globalStage("c0", stage0Body(cpoSums)),
      staticStage("c1", stage1Body(cpoSums)));

  for (std::size_t s = 0; s < participants; ++s) {
    EXPECT_EQ(memberSums[s].load(std::memory_order_acquire),
              cpoSums[s].load(std::memory_order_acquire))
        << "slot=" << s;
  }
}

// Runtime sibling smoke test: parallelChainRuntime with default ChainHints runs
// the same engine.
TEST(ParallelChainCpoEquivalence,
     RuntimeHintsSiblingProducesSameTotalAsCompileTimeMemberCall) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 32;
  const std::size_t participants = pool.participants();

  std::atomic<std::int64_t> total{0};
  const ChainHints rh;
  pool.parallelChainRuntime(
      kN, rh, CancellationToken{},
      staticStage("rt", [&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                            std::size_t lo, std::size_t hi) {
        total.fetch_add(static_cast<std::int64_t>(hi - lo),
                        std::memory_order_acq_rel);
      }));
  EXPECT_EQ(total.load(std::memory_order_acquire),
            static_cast<std::int64_t>(kN));
  (void)participants;
}
