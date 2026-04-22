#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

#include "citor/detail/topology.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"

using citor::PoolGroup;
using citor::PoolKind;
using citor::ThreadPool;

namespace {

// Hint preset shared by every parallelFor below; the deadlock guard test cares only about whether
// the body runs on a background worker, not about chunk shapes.
struct PoolGroupHints {
  static constexpr citor::Balance balance = citor::Balance::StaticUniform;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  // The dispatch engine inspects every static-constexpr member; the unused-const-variable check
  // fires when one is omitted, so the full preset is provided even though `estimatedItemNs` and
  // `minTaskUs` only matter for the inline-fallback gate.
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0; // NOLINT(misc-unused-using-decls)
  static constexpr std::size_t chunk = 0;
};

// Force every member of `PoolGroupHints` to participate in odr-use so clang-tidy does not flag
// individual fields as unused constants under `-Werror`.
[[maybe_unused]] constexpr auto kPoolGroupHintsOdrAnchor =
    PoolGroupHints::balance == citor::Balance::StaticUniform &&
    PoolGroupHints::priority == citor::Priority::Throughput &&
    PoolGroupHints::estimatedItemNs == 0.0 && PoolGroupHints::minTaskUs == 0.0 &&
    PoolGroupHints::chunk == 0U;

} // namespace

TEST(PoolGroup, GlobalReturnsSingleton) {
  const PoolGroup *const first = &PoolGroup::global();
  const PoolGroup *const second = &PoolGroup::global();
  EXPECT_EQ(first, second);
}

TEST(PoolGroup, CcdCountMatchesTopology) {
  PoolGroup &group = PoolGroup::global();
  ASSERT_GE(group.ccdCount(), std::size_t{1});

  // Probe the topology directly so we can log the host's CCD enumeration alongside the
  // arena's view; the two must agree.
  const auto enumerated = citor::detail::enumerateCcds();
  ASSERT_EQ(enumerated.size(), group.ccdCount());

  std::cerr << "[pool_group_test] topology probe reports " << enumerated.size() << " CCD(s); ";
  for (std::size_t ccd = 0; ccd < enumerated.size(); ++ccd) {
    std::cerr << "CCD " << ccd << " has " << enumerated[ccd].size() << " CPU(s)";
    if (ccd + 1 < enumerated.size()) {
      std::cerr << ", ";
    }
  }
  std::cerr << '\n';

  for (std::size_t ccd = 0; ccd < group.ccdCount(); ++ccd) {
    EXPECT_GE(group.arena(ccd).participants(), std::size_t{1});
    EXPECT_EQ(group.arena(ccd).kind(), PoolKind::Arena);
    EXPECT_EQ(group.arena(ccd).arenaIndex(), static_cast<std::uint32_t>(ccd));
  }
}

TEST(PoolGroup, ArenaIsolation) {
  PoolGroup &group = PoolGroup::global();
  if (group.ccdCount() < std::size_t{2}) {
    GTEST_SKIP() << "host has only one CCD; arena isolation requires two CCDs";
  }

  // Workers on arena 0 must report `arenaIndex == 0` from inside their body. Workers on arena 1
  // must not appear in arena 0's TLS view -- a worker can only carry its own arena's token.
  std::atomic<std::uint32_t> observed{std::numeric_limits<std::uint32_t>::max()};
  group.arena(0).parallelFor<PoolGroupHints>(
      std::size_t{0}, std::size_t{1024}, [&observed](std::size_t /*lo*/, std::size_t /*hi*/) {
        const std::uint32_t hint = ThreadPool::currentArenaIndexHint();
        // Producer-side bodies during the inline path
        // would report the sentinel; only background
        // workers stamp their own arena.
        if (hint != std::numeric_limits<std::uint32_t>::max()) {
          observed.store(hint, std::memory_order_relaxed);
        }
      });
  // At least one background worker must have run; the observed token must be 0 (arena 0).
  EXPECT_EQ(observed.load(std::memory_order_acquire), 0U);
}

TEST(PoolGroup, DeadlockGuardFallsThroughToInline) {
  PoolGroup &group = PoolGroup::global();
  if (group.ccdCount() < std::size_t{2}) {
    GTEST_SKIP() << "deadlock-guard test requires at least two CCD arenas";
  }

  // Counts how many times the body of the inner parallelFor on arena 1 ran on a background
  // worker (i.e., a worker whose `currentArenaIndexHint() == 1`) when it was invoked from a
  // worker of arena 0. The cross-arena guard must make this zero: the inner call falls through
  // to inline-on-caller, so the body runs on the same arena-0 worker that issued the call.
  //
  // The outer body skips the producer slot (slot 0 runs on the test thread, which is not a
  // worker on any arena and so is allowed to dispatch into arena 1 normally) so the assertion
  // exercises only the cross-arena worker-to-worker case the deadlock guard targets.
  std::atomic<std::uint64_t> arena1WorkerWakes{0};
  std::atomic<std::uint64_t> outerWorkerInvocations{0};

  group.arena(0).parallelFor<PoolGroupHints>(
      std::size_t{0}, std::size_t{8},
      [&arena1WorkerWakes, &outerWorkerInvocations, &group](std::size_t /*lo*/,
                                                            std::size_t /*hi*/) {
        if (ThreadPool::currentArenaIndexHint() != 0U) {
          // Producer-side body or non-worker context; the cross-arena guard does not apply
          // here, so dispatching into arena 1 from this site is permitted.
          return;
        }
        outerWorkerInvocations.fetch_add(1, std::memory_order_relaxed);
        group.arena(1).parallelFor<PoolGroupHints>(
            std::size_t{0}, std::size_t{4096},
            [&arena1WorkerWakes](std::size_t /*ilo*/, std::size_t /*ihi*/) {
              if (ThreadPool::currentArenaIndexHint() == 1U) {
                arena1WorkerWakes.fetch_add(1, std::memory_order_relaxed);
              }
            });
      });

  EXPECT_GT(outerWorkerInvocations.load(std::memory_order_acquire), 0U);
  EXPECT_EQ(arena1WorkerWakes.load(std::memory_order_acquire), 0U);
}

TEST(PoolGroup, ReentrancySafe) {
  PoolGroup &group = PoolGroup::global();
  // Same-pool reentrancy: an arena worker calls `parallelFor` on its own arena. The cross-arena
  // guard treats every worker-side dispatch as "fall through to inline-on-caller" so the inner
  // body executes on the calling worker without a fresh dispatch generation. The outer body
  // gates the inner call on `insidePoolWorker` so the producer slot (which has no worker context)
  // does not race the in-flight outer dispatch by issuing a second generation.
  //
  // Skipped when arena 0 has only one participant: there is no background worker for the inner
  // dispatch to land on, so `insidePoolWorker()` is false on every body invocation and
  // `workerInvocations` stays zero. The reentrancy guard is exercised on hosts where arena 0
  // spawns at least one background worker.
  if (group.arena(0).participants() < 2U) {
    GTEST_SKIP() << "arena 0 has " << group.arena(0).participants()
                 << " participant(s); reentrancy probe needs a background worker to exercise the "
                    "guard";
  }
  std::atomic<std::uint64_t> nestedBodyExecutions{0};
  std::atomic<std::uint64_t> workerInvocations{0};
  group.arena(0).parallelFor<PoolGroupHints>(
      std::size_t{0}, std::size_t{16},
      [&nestedBodyExecutions, &workerInvocations, &group](std::size_t /*lo*/, std::size_t /*hi*/) {
        if (!ThreadPool::insidePoolWorker()) {
          return; // Producer side; dispatching here would publish a colliding generation.
        }
        workerInvocations.fetch_add(1, std::memory_order_relaxed);
        group.arena(0).parallelFor<PoolGroupHints>(
            std::size_t{0}, std::size_t{32},
            [&nestedBodyExecutions](std::size_t lo, std::size_t hi) {
              nestedBodyExecutions.fetch_add(hi - lo, std::memory_order_relaxed);
            });
      });
  EXPECT_GT(workerInvocations.load(std::memory_order_acquire), 0U);
  // Each worker that invoked the inner call must have observed all 32 elements (since it ran
  // inline on the same thread); total executions = workerInvocations * 32.
  EXPECT_EQ(nestedBodyExecutions.load(std::memory_order_acquire),
            workerInvocations.load(std::memory_order_acquire) * 32U);
}

TEST(PoolGroup, SingleCcdHostBehavior) {
  PoolGroup &group = PoolGroup::global();
  if (group.ccdCount() != std::size_t{1}) {
    GTEST_SKIP() << "host has " << group.ccdCount()
                 << " CCDs; single-CCD acceptance only applies on hosts with one CCD";
  }
  // On a one-CCD host the only path is `arena(0)`; verify it runs work end-to-end.
  std::atomic<std::uint64_t> total{0};
  group.arena(0).parallelFor<PoolGroupHints>(std::size_t{0}, std::size_t{1024},
                                             [&total](std::size_t lo, std::size_t hi) {
                                               total.fetch_add(hi - lo, std::memory_order_relaxed);
                                             });
  EXPECT_EQ(total.load(std::memory_order_acquire), 1024U);
}
