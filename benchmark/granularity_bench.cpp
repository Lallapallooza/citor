// Granularity (METG) workload for the comparative pool bench.
//
// Sweeps `parallelFor` body cost across six decades and three `j` values to
// produce the curve Task Bench (Slaughter et al., SC20) defines as the
// "minimum effective task granularity": the smallest body cost where the
// runtime still hits its peak parallel efficiency. Each cell measures the
// per-dispatch wall time with a body that busy-spins on the TSC for
// `targetNs`; the relative column reports each pool's number against
// `citor::ThreadPool` at the same cell.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"
#include "harness.h"

namespace citor::bench {
namespace {

template <class PoolT> class MeasurementScope {
public:
  explicit MeasurementScope(PoolT &) noexcept {}
};

template <> class MeasurementScope<citor::ThreadPool> {
public:
  explicit MeasurementScope(citor::ThreadPool &pool) noexcept
      : m_producerGuard(pool.bindProducerSlot()), m_latencyGuard(pool.lowLatencyScope()) {}

private:
  citor::ThreadPool::ProducerAffinityGuard m_producerGuard;
  citor::ThreadPool::LowLatencyGuard m_latencyGuard;
};

/// Spin on the TSC until |targetNs| wall-time nanoseconds elapse.
///
/// The body of every pool's dispatch invokes this once with the cell's
/// configured cost so dispatch overhead is the only thing that varies between
/// pools at fixed `(j, bodyNs)`.
///
/// targetNs Target spin duration in wall-time nanoseconds.
/// cal      Calibration constant for converting cycles to ns.
inline void spinForNs(double targetNs, const CyclesPerNanosecond &cal) noexcept {
  if (targetNs <= 0.0) {
    return;
  }
  const std::uint64_t cycles =
      cal.value > 0.0 ? static_cast<std::uint64_t>(targetNs * cal.value) : 0ULL;
  const std::uint64_t start = readCyclesStart();
  while ((readCyclesEnd() - start) < cycles) {
    // Tight spin; empty body.
  }
}

/// Inner-batch size for one measurement bracket at body cost |bodyNs|.
///
/// Each `runUntilBudget` sample times a tight loop of |batchSize| dispatches
/// and reports the average per-dispatch cycle delta. Two effects tighten the
/// sample distribution:
///
/// 1. The fixed per-bracket `__rdtscp` overhead amortizes
///    across the batch, so on body=0 / body=100 ns cells a sub-microsecond
///    dispatch is no longer competing with bracket noise of comparable size.
/// 2. Short OS jitter events (1 kHz timer tick, IRQ delivery, soft-IRQ
///    processing, THP defrag pass) cluster into individual dispatches; with a
///    batch of K, one perturbed dispatch contributes only `1/K` of the bracket
///    average instead of becoming a bimodal slow-mode sample on its own.
///
/// The batch sizes target a per-bracket wall time of roughly 50-300 us across
/// the slowest adapter at each cell, which is above the dominant OS-jitter
/// timescale on this host while still leaving hundreds of brackets per cell
/// within the standard 100 ms budget.
[[nodiscard]] constexpr std::size_t pickBatchSize(double bodyNs) noexcept {
  if (bodyNs <= 100.0) {
    return 256U;
  }
  if (bodyNs <= 1'000.0) {
    return 64U;
  }
  if (bodyNs <= 10'000.0) {
    return 8U;
  }
  // Body cells at >= 100 us run a single dispatch per bracket: their per-bracket
  // wall time already dwarfs both the `__rdtscp` overhead and the host's
  // 1 ms timer-tick interval, so further batching only risks straddling the
  // tick boundary and reintroducing bimodal noise (observed: at body=100 us,
  // batch=2 made `task_thread_pool`'s err% go from 17 % to 24 %).
  return 1U;
}

/// Sample one pool's per-dispatch latency at one (`j`, `bodyNs`) cell.
///
/// PoolT          Concrete pool type (the trait's specialization key).
/// participants   Total worker count to construct the pool with.
/// bodyNs         Per-block body cost in wall-time nanoseconds.
/// cal            Calibration constant for converting cycles to ns.
/// A populated `BenchRow` ready for the comparison table.
template <class PoolT, class Dispatch>
[[nodiscard]] BenchRow measureGranularityWith(const char *displayName, std::size_t participants,
                                              double bodyNs, const CyclesPerNanosecond &cal,
                                              Dispatch dispatch) {
  if (!engineEnabled(displayName)) {
    BenchRow row{};
    row.name = displayName;
    row.skipped = true;
    return row;
  }
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);
  [[maybe_unused]] const MeasurementScope<PoolT> measurementScope(*pool);

  std::atomic<std::uint64_t> sink{0};
  const auto body = [&sink, bodyNs, &cal](std::size_t lo, std::size_t hi) noexcept {
    spinForNs(bodyNs, cal);
    sink.fetch_add(hi - lo, std::memory_order_relaxed);
  };

  const std::size_t batchSize = pickBatchSize(bodyNs);
  auto samples = runUntilBudget(cal, kDefaultSampleBudget, [&]() noexcept {
    const std::uint64_t startCycles = readCyclesStart();
    for (std::size_t k = 0; k < batchSize; ++k) {
      dispatch(*pool, std::size_t{0}, participants, body);
    }
    const std::uint64_t endCycles = readCyclesEnd();
    return (endCycles - startCycles) / batchSize;
  });

  (void)sink.load(std::memory_order_relaxed);

  return finalizeRow(displayName, samples);
}

template <class PoolT>
[[nodiscard]] BenchRow measureGranularity(std::size_t participants, double bodyNs,
                                          const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  return measureGranularityWith<PoolT>(
      Traits::name, participants, bodyNs, cal,
      [](PoolT &pool, std::size_t first, std::size_t last, auto fn) {
        Traits::submitBlocksAndWait(pool, first, last, fn);
      });
}

template <class HintsT>
[[nodiscard]] BenchRow measureCitorGranularityWithHint(const char *displayName,
                                                       std::size_t participants, double bodyNs,
                                                       const CyclesPerNanosecond &cal) {
  return measureGranularityWith<citor::ThreadPool>(
      displayName, participants, bodyNs, cal,
      [](citor::ThreadPool &pool, std::size_t first, std::size_t last, auto fn) {
        pool.parallelFor<HintsT>(first, last, fn);
      });
}

/// One body-cost decade in the METG sweep.
struct BodyCell {
  double bodyNs;
  const char *suffix;
};

/// Six-decade body-cost grid: 0 / 100 ns / 1 us / 10 us / 100 us / 1 ms.
constexpr std::array<BodyCell, 6> kBodyCells{{
    {.bodyNs = 0.0, .suffix = "body0"},
    {.bodyNs = 100.0, .suffix = "body100ns"},
    {.bodyNs = 1'000.0, .suffix = "body1us"},
    {.bodyNs = 10'000.0, .suffix = "body10us"},
    {.bodyNs = 100'000.0, .suffix = "body100us"},
    {.bodyNs = 1'000'000.0, .suffix = "body1ms"},
}};

/// Build a granularity table for one cell of the sweep.
///
/// participants Worker count for every pool.
/// jSuffix      `j`-value suffix (e.g. `"j16"`).
/// cell         Body-cost decade.
/// cal          Calibration constant for converting cycles to ns.
/// Fully-populated `BenchTable`.
BenchTable buildTable(std::size_t participants, const char *jSuffix, BodyCell cell,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"granularity_"} + jSuffix + "_" + cell.suffix;

  table.rows.push_back(measureCitorGranularityWithHint<citor::StaticHints>(
      "citor::ThreadPool[Static]", participants, cell.bodyNs, cal));
  table.rows.push_back(measureCitorGranularityWithHint<citor::DynamicHints>(
      "citor::ThreadPool[Dynamic]", participants, cell.bodyNs, cal));
  table.rows.push_back(measureGranularity<BS::light_thread_pool>(participants, cell.bodyNs, cal));
  table.rows.push_back(measureGranularity<dp::thread_pool<>>(participants, cell.bodyNs, cal));
  table.rows.push_back(
      measureGranularity<::task_thread_pool::task_thread_pool>(participants, cell.bodyNs, cal));
  table.rows.push_back(measureGranularity<riften::Thiefpool>(participants, cell.bodyNs, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureGranularity<::tbb::task_arena>(participants, cell.bodyNs, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureGranularity<::tf::Executor>(participants, cell.bodyNs, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureGranularity<::Eigen::ThreadPool>(participants, cell.bodyNs, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureGranularity<OpenMpRunner>(participants, cell.bodyNs, cal));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(measureGranularity<hmthrp::ThreadPool>(participants, cell.bodyNs, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureGranularity<dispenso::ThreadPool>(participants, cell.bodyNs, cal));
#endif

  return table;
}

/// Build a runner closure for a single `(j, body)` cell. The runner captures
/// `participants` and `cell` by value so the function pointer registered with
/// the bench driver carries no state of its own.
template <std::size_t JParticipants, std::size_t CellIdx>
BenchTable runCell(const CyclesPerNanosecond &cal) {
  constexpr BodyCell cell = kBodyCells[CellIdx];
  static_assert(JParticipants == 2 || JParticipants == 8 || JParticipants == 16,
                "unsupported j-value");
  constexpr const char *jSuffix = []() -> const char * {
    if constexpr (JParticipants == 2) {
      return "j2";
    } else if constexpr (JParticipants == 8) {
      return "j8";
    } else {
      return "j16";
    }
  }();
  return buildTable(JParticipants, jSuffix, cell, cal);
}

struct GranularityRegistrar {
  GranularityRegistrar() {
    // j = 2
    registerWorkload({.name = "granularity_j2_body0", .run = &runCell<2, 0>});
    registerWorkload({.name = "granularity_j2_body100ns", .run = &runCell<2, 1>});
    registerWorkload({.name = "granularity_j2_body1us", .run = &runCell<2, 2>});
    registerWorkload({.name = "granularity_j2_body10us", .run = &runCell<2, 3>});
    registerWorkload({.name = "granularity_j2_body100us", .run = &runCell<2, 4>});
    registerWorkload({.name = "granularity_j2_body1ms", .run = &runCell<2, 5>});
    // j = 8
    registerWorkload({.name = "granularity_j8_body0", .run = &runCell<8, 0>});
    registerWorkload({.name = "granularity_j8_body100ns", .run = &runCell<8, 1>});
    registerWorkload({.name = "granularity_j8_body1us", .run = &runCell<8, 2>});
    registerWorkload({.name = "granularity_j8_body10us", .run = &runCell<8, 3>});
    registerWorkload({.name = "granularity_j8_body100us", .run = &runCell<8, 4>});
    registerWorkload({.name = "granularity_j8_body1ms", .run = &runCell<8, 5>});
    // j = 16
    registerWorkload({.name = "granularity_j16_body0", .run = &runCell<16, 0>});
    registerWorkload({.name = "granularity_j16_body100ns", .run = &runCell<16, 1>});
    registerWorkload({.name = "granularity_j16_body1us", .run = &runCell<16, 2>});
    registerWorkload({.name = "granularity_j16_body10us", .run = &runCell<16, 3>});
    registerWorkload({.name = "granularity_j16_body100us", .run = &runCell<16, 4>});
    registerWorkload({.name = "granularity_j16_body1ms", .run = &runCell<16, 5>});
  }
};

const GranularityRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
