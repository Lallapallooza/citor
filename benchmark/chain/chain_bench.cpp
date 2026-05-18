// parallelChain dispatch latency benchmark.
//
// Measures the wall time of one `parallelChain<ChainHints>(n, stages...)` call
// versus the wall time of the same number of separate fan-out waves on each
// competitor pool. The chain primitive amortises one descriptor publish across
// `kStageCount` stages plus per-stage user-space rendezvous; every competitor's
// natural simulation is N back-to-back parallel-for waves, paying full publish
// + join + per-worker park-wake per stage.
//
// Per-pool primitive mapping (none of BS/dp/task/riften/Eigen/OpenMP/Taskflow
// ships a native chain primitive with a shared rendezvous; oneTBB has no chain
// either. Each competitor row simulates via `kStageCount` back-to-back
// parallel-for waves):
//   - citor pool              -> `parallelChain<ChainBenchHints>` /
//   dynamic-chain hint rows
//                                (native).
//   - citor pool baseline     -> `parallelFor` x kStageCount (own
//   apples-to-apples).
//   - BS::thread_pool          -> `submit_blocks(...).wait()` x kStageCount.
//   - dp::thread_pool          -> N enqueue futures + join, x kStageCount.
//   - task_thread_pool         -> N submit futures + join, x kStageCount.
//   - riften::Thiefpool        -> N enqueue futures + join, x kStageCount.
//   - oneTBB                   -> `tbb::parallel_for` x kStageCount.
//   - Taskflow                 -> taskflow run + wait, x kStageCount.
//   - Eigen::ThreadPool        -> `Schedule + Barrier` wave, x kStageCount.
//   - OpenMP                   -> `#pragma omp parallel for` region, x
//   kStageCount.
//
// MEASUREMENT PROTOCOL: this bench MUST run with an affinity mask wide enough
// to host every requested participant. `taskset -c 0` (single-CPU pinning)
// reduces the pool's topology probe (`detectTopology` in
// `citor/detail/topology.h`) to a single allowed CPU,
// which clamps `pool.participants()` to 1 and silently shunts the dispatch
// into `runChainInline` -- the single-threaded inline fallback.

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "citor/always_assert.h"
#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

/// Chain hint preset at TU scope (not in an anonymous namespace) so clang-tidy
/// treats every static-constexpr member as a public field of a named type
/// rather than an unused constant.
struct ChainBenchHints : citor::ChainHintsDefaults {
  static constexpr bool cancellationChecks = false;
};

namespace citor::bench {
namespace {

/// Number of measurement brackets per row. Each bracket times an inner batch
/// of `kBatchSize` chain calls so per-bracket wall time stays above the
/// host's timer-tick floor.
constexpr std::size_t kIterations = 500;

/// Inner-batch size. Chain dispatch is ~5-10 us at j={8,16}; batching 16
/// pushes per-bracket wall time to ~80-160 us, well above OS-jitter timescale.
constexpr std::size_t kBatchSize = 16;

/// Warmup iterations dropped from the sample window.
constexpr std::size_t kWarmupIterations = 25;

/// Number of stages per chain. 7 empty stages match the spec's headline.
constexpr std::size_t kStageCount = 7;

/// Iteration range upper bound forwarded to each stage. Empty body in practice.
constexpr std::size_t kRangeN = 16;

/// Generic measurement loop sampling per-call wall time. Each bracket runs
/// `kBatchSize` chain calls; per-bracket cycles divided by the batch size to
/// produce per-call ns samples.
template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name,
                                   const CyclesPerNanosecond &cal, RunFn run) {
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    for (std::size_t k = 0; k < kBatchSize; ++k) {
      run();
    }
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    for (std::size_t k = 0; k < kBatchSize; ++k) {
      run();
    }
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal) /
                      static_cast<double>(kBatchSize));
  }
  return finalizeRow(name, samples);
}

[[nodiscard]] BenchRow measureParallelChain(std::size_t participants,
                                            const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  if (pool.participants() <= 1U) {
    throw std::runtime_error("chain_bench: pool.participants() <= 1 (likely "
                             "run under taskset -c 0); "
                             "rerun unpinned or with taskset -c 0-N where N >= "
                             "requested participants.");
  }
  std::atomic<std::uint64_t> sink{0};
  auto emptyBody = [&sink](std::size_t /*stageIdx*/, std::uint32_t slot,
                           std::size_t /*lo*/, std::size_t /*hi*/) noexcept {
    sink.fetch_add(slot, std::memory_order_relaxed);
  };
  BenchRow row = measureLoop("citor::ThreadPool::parallelChain", cal, [&] {
    pool.parallelChain<ChainBenchHints>(
        kRangeN, globalStage("s0", emptyBody), globalStage("s1", emptyBody),
        globalStage("s2", emptyBody), globalStage("s3", emptyBody),
        globalStage("s4", emptyBody), globalStage("s5", emptyBody),
        globalStage("s6", emptyBody));
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}

[[nodiscard]] BenchRow measureSeparateFanout(std::size_t participants,
                                             const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  if (pool.participants() <= 1U) {
    throw std::runtime_error("chain_bench: pool.participants() <= 1 (likely "
                             "run under taskset -c 0); "
                             "rerun unpinned or with taskset -c 0-N where N >= "
                             "requested participants.");
  }
  std::atomic<std::uint64_t> sink{0};
  auto emptyBody = [&sink](std::size_t lo, std::size_t hi) noexcept {
    for (std::size_t i = lo; i < hi; ++i) {
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("citor::ThreadPool::parallelFor x7", cal, [&] {
    for (std::size_t s = 0; s < kStageCount; ++s) {
      pool.parallelFor<citor::HintsDefaults>(0, kRangeN, emptyBody);
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}

[[nodiscard]] BenchRow measureBsPool(std::size_t participants,
                                     const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  std::atomic<std::uint64_t> sink{0};
  auto emptyBody = [&sink](std::size_t lo, std::size_t hi) noexcept {
    for (std::size_t i = lo; i < hi; ++i) {
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("BS::thread_pool::submit_blocks x7", cal, [&] {
    for (std::size_t s = 0; s < kStageCount; ++s) {
      pool.submit_blocks(std::size_t{0}, kRangeN, emptyBody, participants)
          .wait();
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}

template <class Pool, class EnqueueFn>
[[nodiscard]] BenchRow measureFutureFanoutPool(const char *name,
                                               std::size_t participants,
                                               const CyclesPerNanosecond &cal,
                                               Pool &pool, EnqueueFn enqueue) {
  std::atomic<std::uint64_t> sink{0};
  // Per-slot body: the fanout chunks `[0, kRangeN)` across `participants`
  // workers; each task computes its slot's slice. Empty body equivalent.
  auto slotBody = [&sink](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  const std::size_t blockSize = (kRangeN + participants - 1) / participants;
  BenchRow row = measureLoop(name, cal, [&] {
    for (std::size_t s = 0; s < kStageCount; ++s) {
      std::vector<std::future<void>> futs;
      futs.reserve(participants);
      for (std::size_t slot = 0; slot < participants; ++slot) {
        const std::size_t lo = std::min(kRangeN, slot * blockSize);
        const std::size_t hi = std::min(kRangeN, (slot + 1) * blockSize);
        if (lo == hi) {
          continue;
        }
        futs.emplace_back(
            enqueue(pool, [lo, hi, &slotBody]() { slotBody(lo, hi); }));
      }
      for (auto &f : futs) {
        f.get();
      }
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}

[[nodiscard]] BenchRow measureDpPool(std::size_t participants,
                                     const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  return measureFutureFanoutPool(
      "dp::thread_pool::enqueue x7", participants, cal, pool,
      [](dp::thread_pool<> &p, auto fn) { return p.enqueue(std::move(fn)); });
}

[[nodiscard]] BenchRow measureTaskPool(std::size_t participants,
                                       const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(
      static_cast<unsigned int>(participants));
  return measureFutureFanoutPool(
      "task_thread_pool::submit x7", participants, cal, pool,
      [](::task_thread_pool::task_thread_pool &p, auto fn) {
        return p.submit(std::move(fn));
      });
}

[[nodiscard]] BenchRow measureRiftenPool(std::size_t participants,
                                         const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  return measureFutureFanoutPool(
      "riften::Thiefpool::enqueue x7", participants, cal, pool,
      [](riften::Thiefpool &p, auto fn) { return p.enqueue(std::move(fn)); });
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbbPool(std::size_t participants,
                                      const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  std::atomic<std::uint64_t> sink{0};
  auto emptyBody = [&sink](std::size_t lo, std::size_t hi) noexcept {
    for (std::size_t i = lo; i < hi; ++i) {
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("oneTBB::parallel_for x7", cal, [&] {
    for (std::size_t s = 0; s < kStageCount; ++s) {
      CompetitorTraits<::tbb::task_arena>::parallelFor(*arena, std::size_t{0},
                                                       kRangeN, 1, emptyBody);
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflowPool(std::size_t participants,
                                           const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  std::atomic<std::uint64_t> sink{0};
  auto emptyBody = [&sink](std::size_t lo, std::size_t hi) noexcept {
    for (std::size_t i = lo; i < hi; ++i) {
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("Taskflow::run x7", cal, [&] {
    for (std::size_t s = 0; s < kStageCount; ++s) {
      CompetitorTraits<::tf::Executor>::parallelFor(
          *exec, std::size_t{0}, kRangeN, participants, emptyBody);
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] BenchRow measureEigenPool(std::size_t participants,
                                        const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  std::atomic<std::uint64_t> sink{0};
  auto emptyBody = [&sink](std::size_t lo, std::size_t hi) noexcept {
    for (std::size_t i = lo; i < hi; ++i) {
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("Eigen::ThreadPool::Schedule x7", cal, [&] {
    for (std::size_t s = 0; s < kStageCount; ++s) {
      CompetitorTraits<::Eigen::ThreadPool>::parallelFor(
          *pool, std::size_t{0}, kRangeN, participants, emptyBody);
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_LEOPARD
[[nodiscard]] BenchRow measureLeopardPool(std::size_t participants,
                                          const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<hmthrp::ThreadPool>::make(participants);
  std::atomic<std::uint64_t> sink{0};
  auto emptyBody = [&sink](std::size_t lo, std::size_t hi) noexcept {
    for (std::size_t i = lo; i < hi; ++i) {
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("Leopard::dispatch x7", cal, [&] {
    for (std::size_t s = 0; s < kStageCount; ++s) {
      CompetitorTraits<hmthrp::ThreadPool>::parallelFor(
          *pool, std::size_t{0}, kRangeN, participants, emptyBody);
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispensoPool(std::size_t participants,
                                           const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<dispenso::ThreadPool>::make(participants);
  std::atomic<std::uint64_t> sink{0};
  auto emptyBody = [&sink](std::size_t lo, std::size_t hi) noexcept {
    for (std::size_t i = lo; i < hi; ++i) {
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("dispenso::parallel_for x7", cal, [&] {
    for (std::size_t s = 0; s < kStageCount; ++s) {
      CompetitorTraits<dispenso::ThreadPool>::parallelFor(
          *pool, std::size_t{0}, kRangeN, participants, emptyBody);
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMpPool(std::size_t participants,
                                         const CyclesPerNanosecond &cal) {
  std::atomic<std::uint64_t> sink{0};
  BenchRow row = measureLoop("OpenMP::parallel_for x7", cal, [&] {
    const int threads = static_cast<int>(participants);
    const auto lastSigned = static_cast<std::ptrdiff_t>(kRangeN);
    for (std::size_t s = 0; s < kStageCount; ++s) {
#pragma omp parallel for num_threads(threads) schedule(static)
      for (std::ptrdiff_t i = 0; i < lastSigned; ++i) {
        sink.fetch_add(static_cast<std::size_t>(i), std::memory_order_relaxed);
      }
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}
#endif

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"chain_dispatch_"} + suffix;
  table.rows.push_back(measureParallelChain(participants, cal));
  table.rows.push_back(measureSeparateFanout(participants, cal));
  table.rows.push_back(measureBsPool(participants, cal));
  table.rows.push_back(measureDpPool(participants, cal));
  table.rows.push_back(measureTaskPool(participants, cal));
  table.rows.push_back(measureRiftenPool(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbbPool(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureTaskflowPool(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureEigenPool(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOpenMpPool(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(measureLeopardPool(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispensoPool(participants, cal));
#endif
  return table;
}

template <std::size_t JParticipants>
BenchTable runChainCell(const CyclesPerNanosecond &cal) {
  static_assert(JParticipants == 8 || JParticipants == 16 ||
                    JParticipants == 32 || JParticipants == 48 ||
                    JParticipants == 96,
                "unsupported j-value");
  constexpr const char *jSuffix = []() -> const char * {
    if constexpr (JParticipants == 8) {
      return "j8_7stages_empty";
    } else if constexpr (JParticipants == 16) {
      return "j16_7stages_empty";
    } else if constexpr (JParticipants == 32) {
      return "j32_7stages_empty";
    } else if constexpr (JParticipants == 48) {
      return "j48_7stages_empty";
    } else {
      return "j96_7stages_empty";
    }
  }();
  if (!hasEnoughPhysicalCores(JParticipants)) {
    throw std::runtime_error("needs " + std::to_string(JParticipants) +
                             " physical cores");
  }
  return buildTable(JParticipants, jSuffix, cal);
}

// =============================================================================
// Pareto-body chain workload
// =============================================================================
//
// 7 stages, each running a body whose per-iteration cost is drawn from a
// Pareto distribution with shape alpha = 1.16 and median tuned to a
// microsecond. Each chain call sums the per-iteration costs across all stages;
// the parallel sum must match the precomputed sequential total before timing
// fires.
//
// Cost array: kPRangeN per-stage; indexed `(stage, i)` so every stage sees a
// different draw. The shared accumulator is a per-iteration `int64`
// running-sum accumulated atomically (one fetch_add per iteration); the
// chain's correctness gate compares the accumulator to the precomputed total.

constexpr std::size_t kPRangeN = 2'048;
constexpr double kPAlpha = 1.16;
constexpr double kPParetoXmNs = 540.0;
constexpr double kPSpinCapNs = 200'000.0;

[[nodiscard]] inline double pParetoDrawNs(double u) noexcept {
  const double denom = std::pow(1.0 - u, 1.0 / kPAlpha);
  if (denom <= 0.0) {
    return kPSpinCapNs;
  }
  const double draw = kPParetoXmNs / denom;
  if (draw > kPSpinCapNs) {
    return kPSpinCapNs;
  }
  return draw;
}

inline void pSpinForNs(double targetNs,
                       const CyclesPerNanosecond &cal) noexcept {
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

struct ChainParetoData {
  /// Per-stage spin costs in ns; row-major `[stage * kPRangeN + i]`.
  std::vector<std::int64_t> costs;
  /// Sum across all stages and all iterations; the parallel run must match.
  std::int64_t referenceTotal = 0;
};

[[nodiscard]] ChainParetoData buildChainParetoData() {
  ChainParetoData d;
  d.costs.assign(kStageCount * kPRangeN, 0);
  std::mt19937_64 rng(0x4A1701C2E2DULL);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  std::int64_t total = 0;
  for (std::size_t s = 0; s < kStageCount; ++s) {
    for (std::size_t i = 0; i < kPRangeN; ++i) {
      const auto cost = static_cast<std::int64_t>(pParetoDrawNs(uni(rng)));
      d.costs[(s * kPRangeN) + i] = cost;
      total += cost;
    }
  }
  d.referenceTotal = total;
  return d;
}

/// Per-stage body invoked across the chain stages and the future-pool
/// emulations. Spins for `costs[stage * kPRangeN + i]` ns and adds the cost
/// to the shared accumulator atomically.
inline void chainParetoBody(const ChainParetoData &d, std::size_t stage,
                            std::size_t lo, std::size_t hi,
                            std::atomic<std::int64_t> &accum,
                            const CyclesPerNanosecond &cal) noexcept {
  std::int64_t local = 0;
  for (std::size_t i = lo; i < hi; ++i) {
    const std::int64_t cost = d.costs[(stage * kPRangeN) + i];
    pSpinForNs(static_cast<double>(cost), cal);
    local += cost;
  }
  accum.fetch_add(local, std::memory_order_relaxed);
}

template <class RunFn>
[[nodiscard]] BenchRow
measureParetoLoop(const char *name, const CyclesPerNanosecond &cal, RunFn run,
                  std::int64_t referenceTotal) {
  // No batching: each iteration's wall time is already in the millisecond
  // range due to the Pareto bodies, well above the timer-tick floor.
  for (std::size_t i = 0; i < 2; ++i) {
    const std::int64_t v = run();
    BENCH_CHECK_OR_THROW(v == referenceTotal, "chain_bench.cpp");
  }
  std::vector<double> samples;
  constexpr std::size_t kParetoIterations = 20;
  samples.reserve(kParetoIterations);
  for (std::size_t i = 0; i < kParetoIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    BENCH_CHECK_OR_THROW(value == referenceTotal, "chain_bench.cpp");
  }
  return finalizeRow(name, samples);
}

template <class ChainHintsT>
[[nodiscard]] BenchRow measureCitorChainPareto(const char *name,
                                               std::size_t participants,
                                               const ChainParetoData &d,
                                               const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  if (pool.participants() <= 1U) {
    throw std::runtime_error("chain_bench: pool.participants() <= 1 (likely "
                             "run under taskset -c 0); "
                             "rerun unpinned or with taskset -c 0-N where N >= "
                             "requested participants.");
  }
  std::atomic<std::int64_t> accum{0};
  auto stageBody = [&accum, &d, &cal](std::size_t stageIdx,
                                      std::uint32_t /*slot*/, std::size_t lo,
                                      std::size_t hi) noexcept {
    chainParetoBody(d, stageIdx, lo, hi, accum, cal);
  };
  return measureParetoLoop(
      name, cal,
      [&] {
        accum.store(0, std::memory_order_relaxed);
        pool.parallelChain<ChainHintsT>(
            kPRangeN, globalStage("p0", stageBody),
            globalStage("p1", stageBody), globalStage("p2", stageBody),
            globalStage("p3", stageBody), globalStage("p4", stageBody),
            globalStage("p5", stageBody), globalStage("p6", stageBody));
        return accum.load(std::memory_order_relaxed);
      },
      d.referenceTotal);
}

/// Generic per-stage emulation via the trait's `parallelChain` shim. Used by
/// every adapter (BS / dp / task / riften / oneTBB / Taskflow / Eigen /
/// OpenMP). Each adapter's emulation pays its own per-stage barrier cost; the
/// body shape is identical across adapters so the comparison measures dispatch
/// + barrier-wait per stage with the same Pareto-distributed work load.
template <class PoolT>
[[nodiscard]] BenchRow
measureChainParetoAdapter(const char *name, std::size_t participants,
                          const ChainParetoData &d,
                          const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);
  std::atomic<std::int64_t> accum{0};
  return measureParetoLoop(
      name, cal,
      [&] {
        accum.store(0, std::memory_order_relaxed);
        Traits::parallelChain(
            *pool, std::size_t{0}, kPRangeN, participants, kStageCount,
            [&accum, &d, &cal](std::size_t stage, std::size_t lo,
                               std::size_t hi) {
              chainParetoBody(d, stage, lo, hi, accum, cal);
            });
        return accum.load(std::memory_order_relaxed);
      },
      d.referenceTotal);
}

BenchTable buildParetoTable(std::size_t participants, const char *suffix,
                            const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"chain_pareto_"} + suffix;
  const ChainParetoData d = buildChainParetoData();
  table.rows.push_back(measureCitorChainPareto<ChainBenchHints>(
      "citor::ThreadPool::parallelChain[Static]", participants, d, cal));
  table.rows.push_back(measureCitorChainPareto<citor::DynamicChainHints>(
      "citor::ThreadPool::parallelChain[Dynamic]", participants, d, cal));
  table.rows.push_back(measureChainParetoAdapter<BS::light_thread_pool>(
      "BS::thread_pool[chainAdapter]", participants, d, cal));
  table.rows.push_back(measureChainParetoAdapter<dp::thread_pool<>>(
      "dp::thread_pool[chainAdapter]", participants, d, cal));
  table.rows.push_back(
      measureChainParetoAdapter<::task_thread_pool::task_thread_pool>(
          "task_thread_pool[chainAdapter]", participants, d, cal));
  table.rows.push_back(measureChainParetoAdapter<riften::Thiefpool>(
      "riften::Thiefpool[chainAdapter]", participants, d, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureChainParetoAdapter<::tbb::task_arena>(
      "oneTBB::parallel_for x7", participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureChainParetoAdapter<::tf::Executor>(
      "Taskflow::run x7", participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureChainParetoAdapter<::Eigen::ThreadPool>(
      "Eigen::ThreadPool::Schedule x7", participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureChainParetoAdapter<OpenMpRunner>(
      "OpenMP::parallel_for x7", participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(measureChainParetoAdapter<hmthrp::ThreadPool>(
      "Leopard::dispatch x7", participants, d, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureChainParetoAdapter<dispenso::ThreadPool>(
      "dispenso::parallel_for x7", participants, d, cal));
#endif
  return table;
}

template <std::size_t JParticipants>
BenchTable runChainParetoCell(const CyclesPerNanosecond &cal) {
  static_assert(JParticipants == 8 || JParticipants == 16 ||
                    JParticipants == 32 || JParticipants == 48 ||
                    JParticipants == 96,
                "unsupported j-value");
  constexpr const char *jSuffix = []() -> const char * {
    if constexpr (JParticipants == 8) {
      return "j8_7stages_pareto_body";
    } else if constexpr (JParticipants == 16) {
      return "j16_7stages_pareto_body";
    } else if constexpr (JParticipants == 32) {
      return "j32_7stages_pareto_body";
    } else if constexpr (JParticipants == 48) {
      return "j48_7stages_pareto_body";
    } else {
      return "j96_7stages_pareto_body";
    }
  }();
  if (!hasEnoughPhysicalCores(JParticipants)) {
    throw std::runtime_error("needs " + std::to_string(JParticipants) +
                             " physical cores");
  }
  return buildParetoTable(JParticipants, jSuffix, cal);
}

/// File-scope registrar.
struct ChainRegistrar {
  ChainRegistrar() {
    registerWorkload(
        {.name = "chain_dispatch_j8_7stages_empty", .run = &runChainCell<8>});
    registerWorkload(
        {.name = "chain_dispatch_j16_7stages_empty", .run = &runChainCell<16>});
    registerWorkload(
        {.name = "chain_dispatch_j32_7stages_empty", .run = &runChainCell<32>});
    registerWorkload(
        {.name = "chain_dispatch_j48_7stages_empty", .run = &runChainCell<48>});
    registerWorkload(
        {.name = "chain_dispatch_j96_7stages_empty", .run = &runChainCell<96>});
    registerWorkload({.name = "chain_pareto_j8_7stages_pareto_body",
                      .run = &runChainParetoCell<8>});
    registerWorkload({.name = "chain_pareto_j16_7stages_pareto_body",
                      .run = &runChainParetoCell<16>});
    registerWorkload({.name = "chain_pareto_j32_7stages_pareto_body",
                      .run = &runChainParetoCell<32>});
    registerWorkload({.name = "chain_pareto_j48_7stages_pareto_body",
                      .run = &runChainParetoCell<48>});
    registerWorkload({.name = "chain_pareto_j96_7stages_pareto_body",
                      .run = &runChainParetoCell<96>});
  }
};

const ChainRegistrar kRegistrar;

} // namespace

} // namespace citor::bench
