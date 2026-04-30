// Empty fan-out workload for the comparative pool bench.
//
// Measures per-dispatch latency at j in {2, 8, 16}, hot variant (back-to-back
// submits, workers stay warm in their poll loops). Each pool row reports
// median ns/op and err% across `kIterations` samples; the table renders
// relative to `citor::ThreadPool`.
//
// Until the dispatch primitive lands the `citor::ThreadPool`
// row reports the trivial no-op time of the synchronous sentinel adapter; the
// bench infrastructure ships intact so future runs report real numbers as
// soon as the primitive replaces the sentinel.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

namespace citor::bench {
namespace {

/// Number of measurement brackets per row. Each bracket times an inner batch
/// of `kBatchSize` dispatches so the per-bracket wall time stays well above
/// the host's OS-jitter timescale (~1 kHz timer ticks); the headline is the
/// p25 of per-dispatch ns across `kIterations` brackets.
constexpr std::size_t kIterations = 2'000;

/// Inner-batch size. Empty fan-out dispatch spans roughly two orders of magnitude across the
/// surveyed pools; batching 64 dispatches per bracket pushes the slowest pool
/// safely above the timer-tick floor and amortizes the `__rdtscp` overhead
/// below the noise floor for the fastest pool.
constexpr std::size_t kBatchSize = 64;

/// Warmup iterations dropped from the sample window. Hot dispatch numbers are
/// only meaningful once each worker's spin-then-park budget has been observed
/// at least once, which typically takes ~100 dispatches.
constexpr std::size_t kWarmupIterations = 50;

/// Sample one pool's empty-fan-out latency over `kIterations` rounds.
///
/// For each iteration the trait dispatches an empty closure over the trivial
/// range `[0, 1)`; the body is `[](std::size_t, std::size_t){}` so the only
/// cost on the critical path is the pool's own dispatch + wait protocol.
///
/// Cycle deltas are taken with `__rdtsc`/`__rdtscp` (the harness uses
/// `lfence`-bracketed serialization, never `chrono::steady_clock`) and
/// converted to wall-clock nanoseconds via the calibration constant.
///
/// PoolT   Concrete pool type (the trait's specialization key).
/// participants Total worker count to construct the pool with.
/// cal     Calibration constant for converting cycles to ns.
/// A populated `BenchRow` ready for the comparison table.
template <class PoolT, class Dispatch>
[[nodiscard]] BenchRow measureEmptyFanoutWith(const char *displayName, std::size_t participants,
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
    for (std::size_t k = 0; k < kBatchSize; ++k) {
      dispatch(*pool, 0, participants, body);
    }
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    for (std::size_t k = 0; k < kBatchSize; ++k) {
      dispatch(*pool, 0, participants, body);
    }
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal) / static_cast<double>(kBatchSize));
  }

  (void)sink.load(std::memory_order_relaxed);

  return finalizeRow(displayName, samples);
}

template <class PoolT>
[[nodiscard]] BenchRow measureEmptyFanout(std::size_t participants,
                                          const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  return measureEmptyFanoutWith<PoolT>(
      Traits::name, participants, cal,
      [](PoolT &pool, std::size_t first, std::size_t last, auto fn) {
        Traits::submitBlocksAndWait(pool, first, last, fn);
      });
}

template <class HintsT>
[[nodiscard]] BenchRow measureCitorEmptyFanoutWithHint(const char *displayName,
                                                       std::size_t participants,
                                                       const CyclesPerNanosecond &cal) {
  return measureEmptyFanoutWith<citor::ThreadPool>(
      displayName, participants, cal,
      [](citor::ThreadPool &pool, std::size_t first, std::size_t last, auto fn) {
        pool.parallelFor<HintsT>(first, last, fn);
      });
}

/// Build an empty-fan-out comparison table for a single `j` value.
///
/// Iterates every competitor pool, samples its dispatch latency, and assembles
/// the rows in a deterministic order so the table stays readable across
/// invocations. The new pool is always rendered first so the relative column
/// lines up against its 100 % baseline.
///
/// participants Total worker count for every pool.
/// suffix       Workload name suffix (e.g. `"j16_hot"`).
/// cal          Calibration constant for converting cycles to ns.
/// Fully-populated `BenchTable`.
BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"empty_fan_out_"} + suffix;

  table.rows.push_back(measureCitorEmptyFanoutWithHint<citor::StaticHints>(
      "citor::ThreadPool[Static]", participants, cal));
  table.rows.push_back(measureCitorEmptyFanoutWithHint<citor::DynamicHints>(
      "citor::ThreadPool[Dynamic]", participants, cal));
  table.rows.push_back(measureEmptyFanout<BS::light_thread_pool>(participants, cal));
  table.rows.push_back(measureEmptyFanout<dp::thread_pool<>>(participants, cal));
  table.rows.push_back(measureEmptyFanout<::task_thread_pool::task_thread_pool>(participants, cal));
  table.rows.push_back(measureEmptyFanout<riften::Thiefpool>(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureEmptyFanout<::tbb::task_arena>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureEmptyFanout<::tf::Executor>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureEmptyFanout<::Eigen::ThreadPool>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureEmptyFanout<OpenMpRunner>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(measureEmptyFanout<hmthrp::ThreadPool>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureEmptyFanout<dispenso::ThreadPool>(participants, cal));
#endif

  return table;
}

/// Bench runner for the j=16 hot variant; the headline workload at the
/// a representative dominant workload shape on multi-CCD AMD chips.
BenchTable runEmptyFanoutJ16Hot(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/16, "j16_hot", cal);
}

/// Bench runner for the j=8 hot variant; covers the canonical mid-tier
/// representative shape so the harness's multi-`j` story is exercised end-to-end.
BenchTable runEmptyFanoutJ8Hot(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/8, "j8_hot", cal);
}

/// Bench runner for the j=2 hot variant. Sanity check that small-pool
/// dispatch latency does not regress under the same harness.
BenchTable runEmptyFanoutJ2Hot(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/2, "j2_hot", cal);
}

/// File-scope registrar; runs at TU initialization, registering the workloads
/// with the bench driver in registration order.
struct EmptyFanoutRegistrar {
  EmptyFanoutRegistrar() {
    registerWorkload({.name = "empty_fan_out_j2_hot", .run = &runEmptyFanoutJ2Hot});
    registerWorkload({.name = "empty_fan_out_j8_hot", .run = &runEmptyFanoutJ8Hot});
    registerWorkload({.name = "empty_fan_out_j16_hot", .run = &runEmptyFanoutJ16Hot});
  }
};

const EmptyFanoutRegistrar kRegistrar;

} // namespace

} // namespace citor::bench
