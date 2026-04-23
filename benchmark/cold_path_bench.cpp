// Cold-dispatch workload for the comparative pool bench.
//
// Empty-fan-out variant where the producer sleeps 100 ms between iterations so
// every pool's workers park before the next dispatch. Measures the futex
// round-trip (or whatever the pool's park/wake mechanism is) rather than the
// hot spin-poll path that `empty_fanout_bench.cpp` exercises.
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

/// Per-row sample budget. The cool-off sleep dominates wall time, so the
/// budget is tight: 50 samples plus 5 warmup gives a stable
/// lower-quartile while keeping the sweep interactive (about 2 min wall total).
constexpr std::size_t kIterations = 50;

/// Warmup iterations dropped from the sample window. The first dispatch may
/// observe lazy worker spin-up (citor and Eigen lazily allocate their wake
/// scratch) which would skew the cold p25 high.
constexpr std::size_t kWarmupIterations = 5;

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
template <class PoolT>
[[nodiscard]] BenchRow measureColdFanout(std::size_t participants,
                                         const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  std::atomic<std::uint64_t> sink{0};
  const auto body = [&sink](std::size_t lo, std::size_t hi) noexcept {
    sink.fetch_add(hi - lo, std::memory_order_relaxed);
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    std::this_thread::sleep_for(kCoolOff);
    Traits::submitBlocksAndWait(*pool, 0, participants, body);
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    std::this_thread::sleep_for(kCoolOff);
    const std::uint64_t startCycles = readCyclesStart();
    Traits::submitBlocksAndWait(*pool, 0, participants, body);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
  }

  (void)sink.load(std::memory_order_relaxed);

  return finalizeRow(Traits::name, samples);
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

  table.rows.push_back(measureColdFanout<citor::ThreadPool>(participants, cal));
  table.rows.push_back(measureColdFanout<BS::light_thread_pool>(participants, cal));
  table.rows.push_back(measureColdFanout<dp::thread_pool<>>(participants, cal));
  table.rows.push_back(measureColdFanout<::task_thread_pool::task_thread_pool>(participants, cal));
  table.rows.push_back(measureColdFanout<riften::Thiefpool>(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureColdFanout<::tbb::task_arena>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureColdFanout<::tf::Executor>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureColdFanout<::Eigen::ThreadPool>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureColdFanout<OpenMpRunner>(participants, cal));
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
