// Cold-dispatch workload for the comparative pool bench.
//
// Empty-fan-out variant where the producer sleeps 30 ms between iterations so
// every pool's workers should park before the next dispatch. The cell measures
// the futex round-trip (or whatever the pool's park/wake mechanism is) rather
// than the hot spin-poll path that `empty_fanout_bench.cpp` exercises.
//
// **Apples-to-apples caveat.** `bench_main.cpp` calls `kmp_set_blocktime(0)`
// at startup so libomp parks immediately after each parallel region instead of
// holding workers in a 200 ms hot-spin loop. Without that override the cell
// measured policy mismatch (libomp burning 8 cores during the cool-off vs
// every other pool actually parking).
//
// **Trivial-body caveat.** Several runtimes (oneTBB, libomp) collapse a cold
// trivial-body dispatch onto the producer thread instead of waking N workers,
// because the work-stealing queue has no demand by the time the dispatch
// arrives. Verified empirically: oneTBB runs all 8 iterations of an 8-iter
// `parallel_for` on the calling thread (`distinct tids = 1`). Citor's static
// partition fans out unconditionally and pays the full park/wake cost on every
// participant. The numbers in this cell therefore compare different things
// across runtimes; treat them as "what does a trivial cold call cost," not
// "how fast is wake-from-park" specifically.
//
// Sample budget is small: each iteration costs ~100 ms of sleep
// plus the dispatch under measurement, so a row at 200 samples is ~20 s. With
// nine pools per j-value and three j-values, the full sweep is roughly ten
// minutes wall time.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"
#include "harness.h"

namespace citor::bench {
namespace {

/// Per-row sample budget. The cool-off sleep dominates wall time. 300 samples
/// gives the bottom-quartile MAD ~75 draws so even adapters with rare slow
/// wakes (e.g. OpenMP runtime's lazy thread-team adjustment after a cold
/// period) fall under the 10 % MAD bar. Per-row wall time is ~9 s; nine pools
/// across three j-values = ~4 min for the sweep.
constexpr std::size_t kIterations = 300;

/// Warmup iterations dropped from the sample window. The first dispatch may
/// observe lazy worker spin-up (citor and Eigen lazily allocate their wake
/// scratch) which would skew the cold p25 high. 30 covers OpenMP's lazy
/// thread-pool re-fill after a cold gap, which has a different cost profile
/// from the steady-state wake-from-park the bench wants to measure.
constexpr std::size_t kWarmupIterations = 30;

/// Sleep between iterations. Long enough for every pool's spin-then-park
/// budget to expire and workers to land in `FUTEX_WAIT_PRIVATE` (or the pool's
/// equivalent). 30 ms covers the longest park budget observed across the
/// surveyed pools while keeping per-row wall time under ~2 s.
constexpr std::chrono::milliseconds kCoolOff{30};

/// Sample one pool's cold-fan-out latency over `kIterations` rounds.
///
/// Each iteration sleeps `kCoolOff` to let workers park, then dispatches an
/// empty closure over `[0, participants)` and waits for completion. The cycle
/// delta covers the dispatch + wake + body + join sequence; the sleep itself
/// is outside the measurement bracket.
///
/// PoolT   Concrete pool type (the trait's specialization key).
/// participants Total worker count to construct the pool with.
/// cal     Calibration constant for converting cycles to ns.
/// A populated `BenchRow` ready for the comparison table.
template <class PoolT, class Dispatch>
[[nodiscard]] BenchRow
measureColdFanoutWith(const char *displayName, std::size_t participants,
                      const CyclesPerNanosecond &cal, Dispatch dispatch) {
  if (!engineEnabled(displayName)) {
    BenchRow row{};
    row.name = displayName;
    row.skipped = true;
    return row;
  }
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  std::atomic<std::uint64_t> sink{0};
  const auto body = [&sink](std::size_t lo, std::size_t hi) noexcept {
    sink.fetch_add(hi - lo, std::memory_order_relaxed);
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    std::this_thread::sleep_for(kCoolOff);
    dispatch(*pool, 0, participants, body);
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    std::this_thread::sleep_for(kCoolOff);
    const std::uint64_t startCycles = readCyclesStart();
    dispatch(*pool, 0, participants, body);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
  }

  (void)sink.load(std::memory_order_relaxed);

  return finalizeRow(displayName, samples);
}

template <class PoolT>
[[nodiscard]] BenchRow measureColdFanout(std::size_t participants,
                                         const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  return measureColdFanoutWith<PoolT>(
      Traits::name, participants, cal,
      [](PoolT &pool, std::size_t first, std::size_t last, auto fn) {
        Traits::submitBlocksAndWait(pool, first, last, fn);
      });
}

template <class HintsT>
[[nodiscard]] BenchRow
measureCitorColdFanoutWithHint(const char *displayName,
                               std::size_t participants,
                               const CyclesPerNanosecond &cal) {
  return measureColdFanoutWith<citor::ThreadPool>(
      displayName, participants, cal,
      [](citor::ThreadPool &pool, std::size_t first, std::size_t last,
         auto fn) { pool.parallelFor<HintsT>(first, last, fn); });
}

/// Build a cold-path comparison table for a single `j` value.
///
/// participants Total worker count for every pool.
/// suffix       Workload name suffix (e.g. `"j16_cold"`).
/// cal          Calibration constant for converting cycles to ns.
/// Fully-populated `BenchTable`.
BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"cold_fan_out_"} + suffix;

  table.rows.push_back(measureCitorColdFanoutWithHint<citor::StaticHints>(
      "citor::ThreadPool[Static]", participants, cal));
  table.rows.push_back(measureCitorColdFanoutWithHint<citor::DynamicHints>(
      "citor::ThreadPool[Dynamic]", participants, cal));
  table.rows.push_back(
      measureColdFanout<BS::light_thread_pool>(participants, cal));
  table.rows.push_back(measureColdFanout<dp::thread_pool<>>(participants, cal));
  table.rows.push_back(measureColdFanout<::task_thread_pool::task_thread_pool>(
      participants, cal));
  table.rows.push_back(measureColdFanout<riften::Thiefpool>(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureColdFanout<::tbb::task_arena>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureColdFanout<::tf::Executor>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(
      measureColdFanout<::Eigen::ThreadPool>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureColdFanout<OpenMpRunner>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(
      measureColdFanout<hmthrp::ThreadPool>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(
      measureColdFanout<dispenso::ThreadPool>(participants, cal));
#endif

  return table;
}

BenchTable runColdFanoutJ16(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/16, "j16_cold", cal);
}

BenchTable runColdFanoutJ8(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/8, "j8_cold", cal);
}

BenchTable runColdFanoutJ2(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/2, "j2_cold", cal);
}

struct ColdPathRegistrar {
  ColdPathRegistrar() {
    registerWorkload({.name = "cold_fan_out_j2", .run = &runColdFanoutJ2});
    registerWorkload({.name = "cold_fan_out_j8", .run = &runColdFanoutJ8});
    registerWorkload({.name = "cold_fan_out_j16", .run = &runColdFanoutJ16});
  }
};

const ColdPathRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
