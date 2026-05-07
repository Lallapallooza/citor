// Cross-CCD recursive fork-join via `citor::PoolGroup`.
//
// Drives recursive `forkJoin`-style fan-out on every arena of
// `PoolGroup::global()` and verifies the cross-arena guard (TLS participant
// token + inline-fallback path) under recursion. Three rows per cell:
//
//   1. citor::ThreadPool [single 16-thread]   -- baseline; one full-machine
//      pool with no per-CCD pinning. Producer can land on either CCD.
//   2. citor::PoolGroup  [arena 0 native]     -- recursion stays inside the
//      arena that owns the producer; no cross-arena hops. This is the hot
//      path we want PoolGroup users to land on.
//   3. citor::PoolGroup  [arena 1 cross-CCD]  -- producer drives recursion
//      on the OTHER arena, exercising the inline-fallback path the engine
//      takes when a primitive call would cross arenas. Verifies no
//      deadlock and measures the cost of the fallback.
//
// The bench is citor-only by design: `PoolGroup` is a citor-specific
// construct (per-CCD pinned arenas + TLS participant token), and no
// surveyed competitor pool exposes a comparable abstraction symmetrically.
//
// Workload: parFib(28) with cutoff=16 -- same shape as fork_join_bench's
// fib28 cell so the absolute numbers compare directly to other forkJoin
// benches. Iteration / warmup counts mirror the fork-join harness.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "citor/always_assert.h"
#include "citor/hints.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "cycle_clock.h"
#include "multi_arena_harness.h"

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 50;
constexpr std::size_t kWarmupIterations = 5;

constexpr int kFibN = 28;
constexpr int kFibCutoff = 16;

struct ForkJoinHints : citor::HintsDefaults {
  static constexpr citor::Affinity affinity = citor::Affinity::CcdLocal;
};

[[nodiscard]] std::int64_t seqFib(int n) noexcept {
  if (n < 2) {
    return n;
  }
  std::int64_t a = 0;
  std::int64_t b = 1;
  for (int i = 2; i <= n; ++i) {
    const std::int64_t c = a + b;
    a = b;
    b = c;
  }
  return b;
}

template <class Spawn>
[[nodiscard]] std::int64_t parFib(int n, Spawn spawn) {
  if (n <= kFibCutoff) {
    return seqFib(n);
  }
  std::int64_t a = 0;
  std::int64_t b = 0;
  spawn([&] { a = parFib(n - 1, spawn); }, [&] { b = parFib(n - 2, spawn); });
  return a + b;
}

template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name,
                                   const CyclesPerNanosecond &cal, RunFn run) {
  std::atomic<std::int64_t> sink{0};
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    sink.store(run(), std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
  }
  (void)sink.load(std::memory_order_relaxed);
  return finalizeRow(name, samples);
}

// Baseline: single 16-thread ThreadPool; no PoolGroup involvement.
[[nodiscard]] BenchRow measureSinglePool(std::size_t participants,
                                         const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  return measureLoop("citor::ThreadPool[single]", cal, [&] {
    return parFib(kFibN, [&](auto &&a, auto &&b) {
      pool.forkJoin<ForkJoinHints>(std::forward<decltype(a)>(a),
                                   std::forward<decltype(b)>(b));
    });
  });
}

// PoolGroup arena 0: producer drives recursion on its own arena. Hot path.
[[nodiscard]] BenchRow measurePoolGroupNative(MultiArenaHarness &harness,
                                              const CyclesPerNanosecond &cal) {
  ThreadPool &arena = harness.arena(0);
  return measureLoop("citor::PoolGroup[arena0 native]", cal, [&] {
    return parFib(kFibN, [&](auto &&a, auto &&b) {
      arena.forkJoin<ForkJoinHints>(std::forward<decltype(a)>(a),
                                    std::forward<decltype(b)>(b));
    });
  });
}

// PoolGroup arena 1: producer drives recursion on the OTHER arena's pool.
// The producer thread is not a worker of arena(1), so the dispatch goes
// through the cross-arena guard's inline-fallback path on every recursion
// level. The bench measures the wall time of this fallback.
[[nodiscard]] BenchRow
measurePoolGroupCrossCcd(MultiArenaHarness &harness,
                         const CyclesPerNanosecond &cal) {
  ThreadPool &arena = harness.arena(1);
  return measureLoop("citor::PoolGroup[arena1 cross-CCD]", cal, [&] {
    return parFib(kFibN, [&](auto &&a, auto &&b) {
      arena.forkJoin<ForkJoinHints>(std::forward<decltype(a)>(a),
                                    std::forward<decltype(b)>(b));
    });
  });
}

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_pool_group_cross_ccd_"} + suffix;

  // Construct harness BEFORE any thread pinning so PoolGroup::global()'s
  // topology probe sees the unrestricted CPU set. (Cross-CCD bench TU
  // contract documented in multi_arena_harness.h:78-86.)
  MultiArenaHarness harness(/*requiredCcds=*/2U);

  table.rows.push_back(measureSinglePool(participants, cal));
  table.rows.push_back(measurePoolGroupNative(harness, cal));
  table.rows.push_back(measurePoolGroupCrossCcd(harness, cal));
  return table;
}

BenchTable runJ8(const CyclesPerNanosecond &cal) {
  return buildTable(8, "j8", cal);
}
BenchTable runJ16(const CyclesPerNanosecond &cal) {
  return buildTable(16, "j16", cal);
}

struct PoolGroupForkJoinRegistrar {
  PoolGroupForkJoinRegistrar() {
    try {
      (void)requireMultipleCcds(2U);
    } catch (const std::runtime_error &) {
      // Single-CCD host: skip registration entirely (bench output gets no
      // rows for this workload). Mirrors cross_ccd_parallel_for_bench's
      // skip path.
      return;
    }
    registerWorkload(
        {.name = "forkjoin_pool_group_cross_ccd_j8", .run = &runJ8});
    registerWorkload(
        {.name = "forkjoin_pool_group_cross_ccd_j16", .run = &runJ16});
  }
};

const PoolGroupForkJoinRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
