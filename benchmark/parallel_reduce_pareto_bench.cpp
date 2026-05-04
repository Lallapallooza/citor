// Pareto-body parallelReduce workload.
//
// Heavy-tailed body cost: each iteration's spin time is drawn from a Pareto
// distribution with shape alpha = 1.16 and minimum xm chosen so the median
// spin is approximately 1 us. With kN = 1'000'000 iterations the largest 1 %
// of bodies dominate the total work; the workload exposes how each pool
// handles imbalanced load distributions where naive static partitioning
// produces stragglers.
//
// The bench reports per-call median ns for the parallel reduction. The
// reduction itself sums the spin-cost array (in ns), which equals a known
// precomputed total -- the correctness gate fires before timing on every
// pool.
//
// Per-pool primitive mapping:
//   - citor pool              -> `parallelReduce<ReducePareto*Hints>` (native).
//   - oneTBB                  -> `tbb::parallel_reduce` via task arena.
//   - BS / dp / task / riften -> `Traits::parallelReduce(pool, first, last,
//                                participantCount, identity, map, combine)` --
//                                future-pool reduce shim (per-block partials,
//                                serial merge after future barrier).
//   - Taskflow                -> per-block partials via taskflow run + serial merge.
//   - Eigen::ThreadPool       -> per-block partials via Schedule + Barrier.
//   - OpenMP                  -> `parallel for reduction(+:)`.
//
// All rows produce the same int64 sum (sum of spin costs in ns, 1 ns per unit
// of `costNs`); a `CITOR_ALWAYS_ASSERT` before timing fails the bench if any
// row diverges from the sequential reference.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
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
#include <oneapi/tbb/parallel_reduce.h>
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

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 30;
constexpr std::size_t kWarmupIterations = 3;
constexpr std::size_t kN = 1'000'000;

/// Pareto shape (alpha). 1.16 is the classic "80/20 rule": the top 20 % of
/// outcomes contribute 80 % of the mass.
constexpr double kAlpha = 1.16;

/// Pareto minimum (xm) chosen so the distribution's median is tuned to a microsecond.
/// Median = xm * 2^(1/alpha); solving for xm at median = 1000 ns gives
/// xm ~ 549 ns. Rounded down to keep the smallest spin above the TSC noise
/// noise floor.
constexpr double kParetoXmNs = 540.0;

/// Cap the per-iteration spin so a single sample's tail does not dominate the
/// bracket. The Pareto upper tail at alpha=1.16 has unbounded variance; for
/// the bench's wall-time budget any draw above 200 us is clipped. With
/// kN=1M iterations the clip fires on roughly 1 in 50,000 draws, well below
/// the 1 % "dominant tail" the workload aims to expose.
constexpr double kSpinCapNs = 200'000.0;

/// Pareto inverse-CDF draw via uniform u in (0, 1):
///   x = xm / (1 - u)^(1/alpha)
/// Returns ns; clipped to [kParetoXmNs, kSpinCapNs].
[[nodiscard]] inline double paretoDrawNs(double u) noexcept {
  // Guard the (1 - u) factor away from zero so the cap is the only clip path.
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
  /// Per-iteration spin cost in ns (rounded to integer for the reduction sum).
  std::vector<std::int64_t> costNs;
  /// Sum of `costNs`; the parallel reduction must match this exactly.
  std::int64_t totalCostNs = 0;
};

[[nodiscard]] ParetoData buildData() {
  ParetoData d;
  d.costNs.assign(kN, 0);
  // Fixed seed: the same draw across pools so every row sees the same body
  // costs in the same iteration order.
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

/// Per-block sum kernel: spin the bodies and accumulate their nominal cost
/// into the partial sum. The reduction's combine is plain integer addition,
/// so the order of partial merges is irrelevant for correctness.
[[nodiscard]] inline std::int64_t paretoBlockSum(const ParetoData &d, std::size_t lo,
                                                 std::size_t hi,
                                                 const CyclesPerNanosecond &cal) noexcept {
  std::int64_t s = 0;
  for (std::size_t i = lo; i < hi; ++i) {
    spinForNs(static_cast<double>(d.costNs[i]), cal);
    s += d.costNs[i];
  }
  return s;
}

/// Generic sample loop. Returns the reduced value to defeat dead-code
/// elimination and asserts every iteration matches the reference total.
template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name, const CyclesPerNanosecond &cal, RunFn run,
                                   std::int64_t referenceTotal) {
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    const std::int64_t v = run();
    CITOR_ALWAYS_ASSERT(v == referenceTotal);
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  std::atomic<std::int64_t> sink{0};
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
    CITOR_ALWAYS_ASSERT(value == referenceTotal);
  }
  (void)sink.load(std::memory_order_relaxed);
  return finalizeRow(name, samples);
}

// =============================================================================
// citor pool -- native parallelReduce
// =============================================================================

[[nodiscard]] BenchRow measureCitor(std::size_t participants, const ParetoData &d,
                                    const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  return measureLoop(
      "citor::ThreadPool::parallelReduce", cal,
      [&] {
        return pool.parallelReduce<citor::HintsDefaults>(
            std::size_t{0}, kN, std::int64_t{0},
            [&d, &cal](std::size_t lo, std::size_t hi) { return paretoBlockSum(d, lo, hi, cal); },
            std::plus<std::int64_t>{});
      },
      d.totalCostNs);
}

// =============================================================================
// Future-pool shims (BS / dp / task / riften) -- uniform parallelReduce shim
// =============================================================================

[[nodiscard]] BenchRow measureBs(std::size_t participants, const ParetoData &d,
                                 const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  return measureLoop(
      "BS::thread_pool[reduceAdapter]", cal,
      [&] {
        return CompetitorTraits<BS::light_thread_pool>::parallelReduce<std::int64_t>(
            pool, std::size_t{0}, kN, participants, std::int64_t{0},
            [&d, &cal](std::size_t lo, std::size_t hi, std::int64_t identity) {
              return identity + paretoBlockSum(d, lo, hi, cal);
            },
            std::plus<std::int64_t>{});
      },
      d.totalCostNs);
}

[[nodiscard]] BenchRow measureDp(std::size_t participants, const ParetoData &d,
                                 const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  return measureLoop(
      "dp::thread_pool[reduceAdapter]", cal,
      [&] {
        return CompetitorTraits<dp::thread_pool<>>::parallelReduce<std::int64_t>(
            pool, std::size_t{0}, kN, participants, std::int64_t{0},
            [&d, &cal](std::size_t lo, std::size_t hi, std::int64_t identity) {
              return identity + paretoBlockSum(d, lo, hi, cal);
            },
            std::plus<std::int64_t>{});
      },
      d.totalCostNs);
}

[[nodiscard]] BenchRow measureTask(std::size_t participants, const ParetoData &d,
                                   const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(static_cast<unsigned int>(participants));
  return measureLoop(
      "task_thread_pool[reduceAdapter]", cal,
      [&] {
        return CompetitorTraits<::task_thread_pool::task_thread_pool>::parallelReduce<std::int64_t>(
            pool, std::size_t{0}, kN, participants, std::int64_t{0},
            [&d, &cal](std::size_t lo, std::size_t hi, std::int64_t identity) {
              return identity + paretoBlockSum(d, lo, hi, cal);
            },
            std::plus<std::int64_t>{});
      },
      d.totalCostNs);
}

[[nodiscard]] BenchRow measureRiften(std::size_t participants, const ParetoData &d,
                                     const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  return measureLoop(
      "riften::Thiefpool[reduceAdapter]", cal,
      [&] {
        return CompetitorTraits<riften::Thiefpool>::parallelReduce<std::int64_t>(
            pool, std::size_t{0}, kN, participants, std::int64_t{0},
            [&d, &cal](std::size_t lo, std::size_t hi, std::int64_t identity) {
              return identity + paretoBlockSum(d, lo, hi, cal);
            },
            std::plus<std::int64_t>{});
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
      "oneTBB::parallel_reduce", cal,
      [&] {
        return CompetitorTraits<::tbb::task_arena>::parallelReduce<std::int64_t>(
            *arena, std::size_t{0}, kN, std::int64_t{0},
            [&d, &cal](std::size_t lo, std::size_t hi, std::int64_t local) {
              return local + paretoBlockSum(d, lo, hi, cal);
            },
            std::plus<std::int64_t>{});
      },
      d.totalCostNs);
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflow(std::size_t participants, const ParetoData &d,
                                       const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  return measureLoop(
      "Taskflow::reduce_two_wave", cal,
      [&] {
        const std::size_t blocks = participants;
        std::vector<std::int64_t> partials(blocks, 0);
        const std::size_t span = kN;
        const std::size_t block = (span + blocks - 1U) / blocks;
        ::tf::Taskflow flow;
        for (std::size_t b = 0; b < blocks; ++b) {
          const std::size_t lo = std::min(span, b * block);
          const std::size_t hi = std::min(span, (b + 1U) * block);
          if (lo == hi) {
            continue;
          }
          flow.emplace(
              [lo, hi, b, &partials, &d, &cal]() { partials[b] = paretoBlockSum(d, lo, hi, cal); });
        }
        exec->run(flow).wait();
        std::int64_t total = 0;
        for (const std::int64_t v : partials) {
          total += v;
        }
        return total;
      },
      d.totalCostNs);
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] BenchRow measureEigen(std::size_t participants, const ParetoData &d,
                                    const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  return measureLoop(
      "Eigen::ThreadPool::reduce_two_wave", cal,
      [&] {
        const std::size_t blocks = participants;
        std::vector<std::int64_t> partials(blocks, 0);
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
        for (std::size_t i = 0; i < ranges.size(); ++i) {
          const std::size_t lo = ranges[i].first;
          const std::size_t hi = ranges[i].second;
          pool->Schedule([lo, hi, i, &partials, &d, &cal, &bar]() {
            partials[i] = paretoBlockSum(d, lo, hi, cal);
            bar.Notify();
          });
        }
        bar.Wait();
        std::int64_t total = 0;
        for (const std::int64_t v : partials) {
          total += v;
        }
        return total;
      },
      d.totalCostNs);
}
#endif

#ifdef CITOR_BENCH_HAS_LEOPARD
[[nodiscard]] BenchRow measureLeopard(std::size_t participants, const ParetoData &d,
                                      const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<hmthrp::ThreadPool>::make(participants);
  return measureLoop(
      "Leopard::reduce_two_wave", cal,
      [&] {
        // Direct dispatch (not Traits::parallelFor) so partials sizing matches block count
        // breaks `partials[blockIdx]` when callers expect 1:1 block-to-partial mapping.
        const std::size_t blocks = participants;
        std::vector<std::int64_t> partials(blocks, 0);
        const std::size_t span = kN;
        const std::size_t block = (span + blocks - 1U) / blocks;
        std::vector<std::future<void>> futures;
        futures.reserve(blocks);
        for (std::size_t b = 0; b < blocks; ++b) {
          const std::size_t lo = std::min(span, b * block);
          const std::size_t hi = std::min(span, (b + 1U) * block);
          if (lo == hi) {
            continue;
          }
          futures.emplace_back(pool->dispatch(false, [b, lo, hi, &partials, &d, &cal]() {
            partials[b] = paretoBlockSum(d, lo, hi, cal);
          }));
        }
        for (auto &f : futures) {
          f.get();
        }
        std::int64_t total = 0;
        for (const std::int64_t v : partials) {
          total += v;
        }
        return total;
      },
      d.totalCostNs);
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispenso(std::size_t participants, const ParetoData &d,
                                       const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<dispenso::ThreadPool>::make(participants);
  return measureLoop(
      "dispenso::reduce_two_wave", cal,
      [&] {
        // See Leopard above for the partials-sizing rationale.
        const std::size_t blocks = participants;
        std::vector<std::int64_t> partials(blocks, 0);
        const std::size_t span = kN;
        const std::size_t block = (span + blocks - 1U) / blocks;
        {
          dispenso::TaskSet taskSet(*pool);
          for (std::size_t b = 0; b < blocks; ++b) {
            const std::size_t lo = std::min(span, b * block);
            const std::size_t hi = std::min(span, (b + 1U) * block);
            if (lo == hi) {
              continue;
            }
            taskSet.schedule([b, lo, hi, &partials, &d, &cal]() {
              partials[b] = paretoBlockSum(d, lo, hi, cal);
            });
          }
        }
        std::int64_t total = 0;
        for (const std::int64_t v : partials) {
          total += v;
        }
        return total;
      },
      d.totalCostNs);
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMp(std::size_t participants, const ParetoData &d,
                                     const CyclesPerNanosecond &cal) {
  const auto threads = static_cast<int>(participants);
  return measureLoop(
      "OpenMP::reduce_plus", cal,
      [&] {
        std::int64_t total = 0;
        const auto n = static_cast<std::ptrdiff_t>(kN);
#pragma omp parallel num_threads(threads) reduction(+ : total)
        {
          const auto threadCount = static_cast<std::size_t>(omp_get_num_threads());
          const auto threadIdx = static_cast<std::size_t>(omp_get_thread_num());
          const auto span = static_cast<std::size_t>(n);
          const std::size_t block = (span + threadCount - 1U) / threadCount;
          const std::size_t lo = std::min(span, threadIdx * block);
          const std::size_t hi = std::min(span, (threadIdx + 1U) * block);
          if (lo < hi) {
            total += paretoBlockSum(d, lo, hi, cal);
          }
        }
        return total;
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
  table.workload = std::string{"reduce_pareto_int64_"} + suffix;
  const ParetoData d = buildData();
  table.rows.push_back(measureCitor(participants, d, cal));
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
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(measureLeopard(participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispenso(participants, d, cal));
#endif
  return table;
}

BenchTable runParetoJ8(const CyclesPerNanosecond &cal) { return buildTable(8, "j8_n1M", cal); }
BenchTable runParetoJ16(const CyclesPerNanosecond &cal) { return buildTable(16, "j16_n1M", cal); }

struct ParetoRegistrar {
  ParetoRegistrar() {
    registerWorkload({.name = "reduce_pareto_int64_j8_n1M", .run = &runParetoJ8});
    registerWorkload({.name = "reduce_pareto_int64_j16_n1M", .run = &runParetoJ16});
  }
};

const ParetoRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
