// Pareto-body parallelFor workload.
//
// Claim: citor wins among 8 pools at default Balance under heavy-tailed body
// cost. Each iteration's spin time is drawn from a Pareto distribution with
// shape alpha = 1.16 and minimum xm chosen so the median spin is approximately
// 1 us. With kN = 1'000'000 iterations the largest 1% of bodies dominate the
// total work; a parallelFor over this range exposes how each pool's default
// Balance handles imbalanced distributions where naive static partitioning
// produces stragglers.
//
// The bench reports per-call median ns for the parallel for. Each pool's
// invocation must visit every iteration exactly once; a side-effect counter
// (parallel sum of per-iteration costs) is checked against the sequential
// reference via CITOR_ALWAYS_ASSERT before timing on every pool.
//
// Per-pool primitive mapping (default Balance for each pool):
//   - citor pool              -> `parallelFor<ParetoBodyHints>` (StaticUniform,
//                                Balance::StaticUniform is the citor default).
//   - oneTBB                  -> `tbb::parallel_for` via task arena.
//   - BS / dp / task / riften -> `Traits::parallelFor(pool, first, last,
//                                participantCount, fn)` -- block-shaped fan-out
//                                via per-block futures.
//   - Taskflow                -> per-block taskflow run.
//   - Eigen::ThreadPool       -> per-block Schedule + Barrier.
//   - OpenMP                  -> manual `parallel num_threads(...)` with each
//                                thread sweeping its own block (matches the
//                                reduce-pareto bench's block-shape).

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "citor/always_assert.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
#include <unsupported/Eigen/CXX11/ThreadPool>
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
#include <omp.h>
#endif

// Hint preset at TU scope so clang-tidy treats every static-constexpr member
// as a public field of a named type rather than an unused constant.
struct ParetoBodyHints : citor::HintsDefaults {
  static constexpr bool cancellationChecks = false;
};

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 30;
constexpr std::size_t kWarmupIterations = 3;
constexpr std::size_t kN = 1'000'000;

/// Pareto shape (alpha). 1.16 is the classic "80/20 rule": the top 20% of
/// outcomes contribute 80% of the mass.
constexpr double kAlpha = 1.16;

/// Pareto minimum (xm) chosen so the distribution's median is tuned to a microsecond.
/// Median = xm * 2^(1/alpha); solving for xm at median = 1000 ns gives
/// xm ~ 549 ns. Rounded down to keep the smallest spin above the TSC noise
/// noise floor.
constexpr double kParetoXmNs = 540.0;

/// Cap the per-iteration spin so a single sample's tail does not dominate the
/// bracket. The Pareto upper tail at alpha=1.16 has unbounded variance; for
/// the bench's wall-time budget any draw above 200 us is clipped.
constexpr double kSpinCapNs = 200'000.0;

/// Pareto inverse-CDF draw via uniform u in (0, 1):
///   x = xm / (1 - u)^(1/alpha)
[[nodiscard]] inline double paretoDrawNs(double u) noexcept {
  const double denom = std::pow(1.0 - u, 1.0 / kAlpha);
  if (denom <= 0.0) {
    return kSpinCapNs;
  }
  const double draw = kParetoXmNs / denom;
  if (draw > kSpinCapNs) {
    return kSpinCapNs;
  }
  return draw;
}

/// Spin on the TSC until |targetNs| wall-time nanoseconds elapse.
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

struct ParetoData {
  /// Per-iteration spin cost in ns (rounded to integer).
  std::vector<std::int64_t> costNs;
  /// Sum of `costNs`; correctness gate compares parallel sum against this.
  std::int64_t totalCostNs = 0;
};

[[nodiscard]] ParetoData buildData() {
  ParetoData d;
  d.costNs.assign(kN, 0);
  // Fixed seed: every pool sees the same body costs in the same iteration order.
  std::mt19937_64 rng(0xC1701F1A2E2DULL);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  std::int64_t total = 0;
  for (std::size_t i = 0; i < kN; ++i) {
    const double draw = paretoDrawNs(uni(rng));
    const auto cost = static_cast<std::int64_t>(draw);
    d.costNs[i] = cost;
    total += cost;
  }
  d.totalCostNs = total;
  return d;
}

/// Per-block kernel: spin each iteration's body and accumulate its nominal
/// cost into the shared atomic sink. Used by every parallelFor row so the
/// correctness gate fires on the same observable.
inline void paretoBlockBody(const ParetoData &d, std::size_t lo, std::size_t hi,
                            const CyclesPerNanosecond &cal,
                            std::atomic<std::int64_t> &sink) noexcept {
  std::int64_t local = 0;
  for (std::size_t i = lo; i < hi; ++i) {
    spinForNs(static_cast<double>(d.costNs[i]), cal);
    local += d.costNs[i];
  }
  sink.fetch_add(local, std::memory_order_relaxed);
}

/// Generic sample loop. Returns p25 ns/op via finalizeRow and asserts every
/// iteration's accumulated sum equals the reference total.
template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name, const CyclesPerNanosecond &cal, RunFn run,
                                   std::int64_t referenceTotal) {
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    const std::int64_t v = run();
    CITOR_ALWAYS_ASSERT(v == referenceTotal);
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(value == referenceTotal);
  }
  return finalizeRow(name, samples);
}

// =============================================================================
// citor pool -- native parallelFor, default Balance::StaticUniform
// =============================================================================

template <class HintsT>
[[nodiscard]] BenchRow measureCitorWithHint(const char *displayName, std::size_t participants,
                                             const ParetoData &d, const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  return measureLoop(
      displayName, cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        pool.parallelFor<HintsT>(std::size_t{0}, kN,
                                  [&d, &cal, &sink](std::size_t lo, std::size_t hi) {
                                    paretoBlockBody(d, lo, hi, cal, sink);
                                  });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}

[[nodiscard]] BenchRow measureCitor(std::size_t participants, const ParetoData &d,
                                    const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  return measureLoop(
      "citor::ThreadPool::parallelFor", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        pool.parallelFor<ParetoBodyHints>(std::size_t{0}, kN,
                                          [&d, &cal, &sink](std::size_t lo, std::size_t hi) {
                                            paretoBlockBody(d, lo, hi, cal, sink);
                                          });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}

// =============================================================================
// Future-pool shims (BS / dp / task / riften) -- block-shaped parallelFor
// =============================================================================

[[nodiscard]] BenchRow measureBs(std::size_t participants, const ParetoData &d,
                                 const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  return measureLoop(
      "BS::thread_pool::parallelFor", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        CompetitorTraits<BS::light_thread_pool>::parallelFor(
            pool, std::size_t{0}, kN, participants,
            [&d, &cal, &sink](std::size_t lo, std::size_t hi) {
              paretoBlockBody(d, lo, hi, cal, sink);
            });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}

[[nodiscard]] BenchRow measureDp(std::size_t participants, const ParetoData &d,
                                 const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  return measureLoop(
      "dp::thread_pool::parallelFor", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        CompetitorTraits<dp::thread_pool<>>::parallelFor(
            pool, std::size_t{0}, kN, participants,
            [&d, &cal, &sink](std::size_t lo, std::size_t hi) {
              paretoBlockBody(d, lo, hi, cal, sink);
            });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}

[[nodiscard]] BenchRow measureTask(std::size_t participants, const ParetoData &d,
                                   const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(static_cast<unsigned int>(participants));
  return measureLoop(
      "task_thread_pool::parallelFor", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        CompetitorTraits<::task_thread_pool::task_thread_pool>::parallelFor(
            pool, std::size_t{0}, kN, participants,
            [&d, &cal, &sink](std::size_t lo, std::size_t hi) {
              paretoBlockBody(d, lo, hi, cal, sink);
            });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}

[[nodiscard]] BenchRow measureRiften(std::size_t participants, const ParetoData &d,
                                     const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  return measureLoop(
      "riften::Thiefpool::parallelFor", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        CompetitorTraits<riften::Thiefpool>::parallelFor(
            pool, std::size_t{0}, kN, participants,
            [&d, &cal, &sink](std::size_t lo, std::size_t hi) {
              paretoBlockBody(d, lo, hi, cal, sink);
            });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}

// =============================================================================
// Native primitives: oneTBB, Taskflow, Eigen, OpenMP
// =============================================================================

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbb(std::size_t participants, const ParetoData &d,
                                  const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  return measureLoop(
      "oneTBB::parallel_for", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        arena->execute([&] {
          ::tbb::parallel_for(::tbb::blocked_range<std::size_t>{std::size_t{0}, kN},
                              [&d, &cal, &sink](const ::tbb::blocked_range<std::size_t> &r) {
                                paretoBlockBody(d, r.begin(), r.end(), cal, sink);
                              });
        });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflow(std::size_t participants, const ParetoData &d,
                                       const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  return measureLoop(
      "Taskflow::for_each_block", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        const std::size_t blocks = participants;
        const std::size_t span = kN;
        const std::size_t block = (span + blocks - 1U) / blocks;
        ::tf::Taskflow flow;
        for (std::size_t b = 0; b < blocks; ++b) {
          const std::size_t lo = std::min(span, b * block);
          const std::size_t hi = std::min(span, (b + 1U) * block);
          if (lo == hi) {
            continue;
          }
          flow.emplace([lo, hi, &d, &cal, &sink]() { paretoBlockBody(d, lo, hi, cal, sink); });
        }
        exec->run(flow).wait();
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] BenchRow measureEigen(std::size_t participants, const ParetoData &d,
                                    const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  return measureLoop(
      "Eigen::ThreadPool::for_each_block", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        const std::size_t blocks = participants;
        const std::size_t span = kN;
        const std::size_t block = (span + blocks - 1U) / blocks;
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        ranges.reserve(blocks);
        for (std::size_t b = 0; b < blocks; ++b) {
          const std::size_t lo = std::min(span, b * block);
          const std::size_t hi = std::min(span, (b + 1U) * block);
          if (lo == hi) {
            continue;
          }
          ranges.emplace_back(lo, hi);
        }
        ::Eigen::Barrier bar(static_cast<unsigned int>(ranges.size()));
        for (const auto &range : ranges) {
          const std::size_t lo = range.first;
          const std::size_t hi = range.second;
          pool->Schedule([lo, hi, &d, &cal, &sink, &bar]() {
            paretoBlockBody(d, lo, hi, cal, sink);
            bar.Notify();
          });
        }
        bar.Wait();
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMp(std::size_t participants, const ParetoData &d,
                                     const CyclesPerNanosecond &cal) {
  const auto threads = static_cast<int>(participants);
  return measureLoop(
      "OpenMP::parallel_for_block", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        const auto n = static_cast<std::ptrdiff_t>(kN);
#pragma omp parallel num_threads(threads)
        {
          const auto threadCount = static_cast<std::size_t>(omp_get_num_threads());
          const auto threadIdx = static_cast<std::size_t>(omp_get_thread_num());
          const std::size_t span = static_cast<std::size_t>(n);
          const std::size_t block = (span + threadCount - 1U) / threadCount;
          const std::size_t lo = std::min(span, threadIdx * block);
          const std::size_t hi = std::min(span, (threadIdx + 1U) * block);
          if (lo < hi) {
            paretoBlockBody(d, lo, hi, cal, sink);
          }
        }
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}
#endif

// =============================================================================
// Table builder + registrar
// =============================================================================

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"pareto_body_for_"} + suffix;
  ParetoData d = buildData();
  table.rows.push_back(measureCitorWithHint<citor::StaticHints>(
      "citor::ThreadPool[Static]", participants, d, cal));
  table.rows.push_back(measureCitorWithHint<citor::DynamicHints>(
      "citor::ThreadPool[Dynamic]", participants, d, cal));
  table.rows.push_back(measureBs(participants, d, cal));
  table.rows.push_back(measureDp(participants, d, cal));
  table.rows.push_back(measureTask(participants, d, cal));
  table.rows.push_back(measureRiften(participants, d, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb(participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureTaskflow(participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureEigen(participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOpenMp(participants, d, cal));
#endif
  return table;
}

BenchTable runParetoBodyJ8(const CyclesPerNanosecond &cal) { return buildTable(8, "j8_n1M", cal); }
BenchTable runParetoBodyJ16(const CyclesPerNanosecond &cal) {
  return buildTable(16, "j16_n1M", cal);
}

struct ParetoBodyRegistrar {
  ParetoBodyRegistrar() {
    registerWorkload({.name = "pareto_body_for_j8_n1M", .run = &runParetoBodyJ8});
    registerWorkload({.name = "pareto_body_for_j16_n1M", .run = &runParetoBodyJ16});
  }
};

const ParetoBodyRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
