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

/// Sample one pool's per-dispatch latency at one (`j`, `bodyNs`) cell.
///
/// PoolT          Concrete pool type (the trait's specialization key).
/// participants   Total worker count to construct the pool with.
/// bodyNs         Per-block body cost in wall-time nanoseconds.
/// cal            Calibration constant for converting cycles to ns.
/// A populated `BenchRow` ready for the comparison table.
template <class PoolT>
[[nodiscard]] BenchRow measureGranularity(std::size_t participants, double bodyNs,
                                          const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  std::atomic<std::uint64_t> sink{0};
  const auto body = [&sink, bodyNs, &cal](std::size_t lo, std::size_t hi) noexcept {
    spinForNs(bodyNs, cal);
    sink.fetch_add(hi - lo, std::memory_order_relaxed);
  };

  // Time-budget collection: many short samples for the body=0 / body=100 ns cells, fewer
  // samples for the body=1 ms cell. Keeps per-cell wall time roughly constant and gives the
  // sub-microsecond cells enough draws to stabilize the lower-quartile headline.
  auto samples = runUntilBudget(cal, kDefaultSampleBudget, [&]() noexcept {
    const std::uint64_t startCycles = readCyclesStart();
    Traits::submitBlocksAndWait(*pool, 0, participants, body);
    const std::uint64_t endCycles = readCyclesEnd();
    return endCycles - startCycles;
  });

  (void)sink.load(std::memory_order_relaxed);

  return finalizeRow(Traits::name, samples);
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

  table.rows.push_back(measureGranularity<citor::ThreadPool>(participants, cell.bodyNs, cal));
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
