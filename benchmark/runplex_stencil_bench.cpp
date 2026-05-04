// Phase-dependent stencil workload for `runPlex`.
//
// 30 phases x participants slots. Each phase writes one element per slot,
// where the value depends on the slot's value from the previous phase plus a
// per-phase factor. The dependency makes this distinct from `runplex_heat`
// (which has per-cell parallelism within a timestep but stages are
// data-independent across the row partition); here the cross-phase
// dependency is what runPlex's persistent-worker rendezvous accelerates --
// the join-republish-wake on every phase is the cost competitor pools pay
// without a plex primitive.
//
// Per-pool primitive mapping:
//   - citor pool              -> `runPlex<StencilHints>` (native plex).
//   - oneTBB                  -> nPhases back-to-back `parallel_for` waves.
//   - Taskflow                -> nPhases back-to-back taskflow runs.
//   - Eigen::ThreadPool       -> nPhases back-to-back Schedule + Barrier.
//   - OpenMP                  -> nPhases back-to-back `parallel for`.
//   - BS / dp / task / riften -> `Traits::parallelChain(...)` future-pool
//                                shim: nPhases back-to-back fanout waves
//                                joined via futures / barriers.
//
// Correctness gate: each phase's per-slot value follows the closed-form
//   v(phase, slot) = slot + sum_{p=0..phase} p
//                  = slot + phase * (phase + 1) / 2
// The final-phase checksum (sum across slots) is:
//   (P-1)*P/2 + P * (nPhases-1) * nPhases / 2
// computed at build time and asserted before timing fires.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "citor/always_assert.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

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

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 50;
constexpr std::size_t kWarmupIterations = 5;
constexpr std::size_t kPhases = 30;
constexpr double kBodyNs = 5'000.0;

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

/// Closed-form expected final-phase checksum across slots.
[[nodiscard]] constexpr std::int64_t expectedFinalChecksum(std::size_t participants,
                                                           std::size_t nPhases) noexcept {
  const auto p = static_cast<std::int64_t>(participants);
  const auto nph = static_cast<std::int64_t>(nPhases);
  // sum_{slot=0..P-1} slot = P*(P-1)/2
  // sum_{phase=0..N-1} phase = (N-1)*N/2
  // Final per-slot value = slot + (N-1)*N/2; per-slot sum across slots * P
  return (p * (p - 1) / 2) + (p * (nph - 1) * nph / 2);
}

/// Per-phase per-slot value: slot + sum_{k=0..phase} k = slot + phase*(phase+1)/2.
/// The body computes this iteratively from the previous phase's value:
///   v(phase, slot) = v(phase-1, slot) + phase
[[nodiscard]] inline std::int64_t advance(std::int64_t prevValue, std::size_t phase) noexcept {
  return prevValue + static_cast<std::int64_t>(phase);
}

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
// citor pool -- native runPlex
// =============================================================================

[[nodiscard]] BenchRow measureCitor(std::size_t participants, std::int64_t referenceTotal,
                                    const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  // Per-slot scratch: holds the running "previous phase" value for that slot.
  std::vector<std::int64_t> slotState(participants, 0);
  auto phaseFn = [&slotState, &cal](std::size_t phaseIdx, std::uint32_t slot, std::size_t /*lo*/,
                                    std::size_t /*hi*/, void * /*tlsArena*/) {
    spinForNs(kBodyNs, cal);
    slotState[slot] = advance(slotState[slot], phaseIdx);
  };
  return measureLoop(
      "citor::ThreadPool::runPlex", cal,
      [&] {
        for (std::size_t s = 0; s < participants; ++s) {
          slotState[s] = static_cast<std::int64_t>(s);
        }
        pool.runPlex<citor::HintsDefaults>(kPhases, /*n=*/participants, phaseFn);
        std::int64_t total = 0;
        for (const std::int64_t v : slotState) {
          total += v;
        }
        return total;
      },
      referenceTotal);
}

// =============================================================================
// Native primitives: oneTBB, Taskflow, Eigen, OpenMP
// (each emulates plex as nPhases back-to-back parallelFor waves)
// =============================================================================

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbb(std::size_t participants, std::int64_t referenceTotal,
                                  const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  std::vector<std::int64_t> slotState(participants, 0);
  return measureLoop(
      "oneTBB::parallel_for x30", cal,
      [&] {
        for (std::size_t s = 0; s < participants; ++s) {
          slotState[s] = static_cast<std::int64_t>(s);
        }
        for (std::size_t phase = 0; phase < kPhases; ++phase) {
          CompetitorTraits<::tbb::task_arena>::parallelFor(
              *arena, std::size_t{0}, participants, /*grain=*/1,
              [phase, &slotState, &cal](std::size_t lo, std::size_t hi) {
                for (std::size_t i = lo; i < hi; ++i) {
                  spinForNs(kBodyNs, cal);
                  slotState[i] = advance(slotState[i], phase);
                }
              });
        }
        std::int64_t total = 0;
        for (const std::int64_t v : slotState) {
          total += v;
        }
        return total;
      },
      referenceTotal);
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflow(std::size_t participants, std::int64_t referenceTotal,
                                       const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  std::vector<std::int64_t> slotState(participants, 0);
  return measureLoop(
      "Taskflow::run x30", cal,
      [&] {
        for (std::size_t s = 0; s < participants; ++s) {
          slotState[s] = static_cast<std::int64_t>(s);
        }
        for (std::size_t phase = 0; phase < kPhases; ++phase) {
          CompetitorTraits<::tf::Executor>::parallelFor(
              *exec, std::size_t{0}, participants, participants,
              [phase, &slotState, &cal](std::size_t lo, std::size_t hi) {
                for (std::size_t i = lo; i < hi; ++i) {
                  spinForNs(kBodyNs, cal);
                  slotState[i] = advance(slotState[i], phase);
                }
              });
        }
        std::int64_t total = 0;
        for (const std::int64_t v : slotState) {
          total += v;
        }
        return total;
      },
      referenceTotal);
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] BenchRow measureEigen(std::size_t participants, std::int64_t referenceTotal,
                                    const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  std::vector<std::int64_t> slotState(participants, 0);
  return measureLoop(
      "Eigen::ThreadPool::Schedule x30", cal,
      [&] {
        for (std::size_t s = 0; s < participants; ++s) {
          slotState[s] = static_cast<std::int64_t>(s);
        }
        for (std::size_t phase = 0; phase < kPhases; ++phase) {
          CompetitorTraits<::Eigen::ThreadPool>::parallelFor(
              *pool, std::size_t{0}, participants, participants,
              [phase, &slotState, &cal](std::size_t lo, std::size_t hi) {
                for (std::size_t i = lo; i < hi; ++i) {
                  spinForNs(kBodyNs, cal);
                  slotState[i] = advance(slotState[i], phase);
                }
              });
        }
        std::int64_t total = 0;
        for (const std::int64_t v : slotState) {
          total += v;
        }
        return total;
      },
      referenceTotal);
}
#endif

#ifdef CITOR_BENCH_HAS_LEOPARD
[[nodiscard]] BenchRow measureLeopard(std::size_t participants, std::int64_t referenceTotal,
                                      const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<hmthrp::ThreadPool>::make(participants);
  std::vector<std::int64_t> slotState(participants, 0);
  return measureLoop(
      "Leopard::dispatch x30", cal,
      [&] {
        for (std::size_t s = 0; s < participants; ++s) {
          slotState[s] = static_cast<std::int64_t>(s);
        }
        for (std::size_t phase = 0; phase < kPhases; ++phase) {
          CompetitorTraits<hmthrp::ThreadPool>::parallelFor(
              *pool, std::size_t{0}, participants, participants,
              [phase, &slotState, &cal](std::size_t lo, std::size_t hi) {
                for (std::size_t i = lo; i < hi; ++i) {
                  spinForNs(kBodyNs, cal);
                  slotState[i] = advance(slotState[i], phase);
                }
              });
        }
        std::int64_t total = 0;
        for (const std::int64_t v : slotState) {
          total += v;
        }
        return total;
      },
      referenceTotal);
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispenso(std::size_t participants, std::int64_t referenceTotal,
                                       const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<dispenso::ThreadPool>::make(participants);
  std::vector<std::int64_t> slotState(participants, 0);
  return measureLoop(
      "dispenso::parallel_for x30", cal,
      [&] {
        for (std::size_t s = 0; s < participants; ++s) {
          slotState[s] = static_cast<std::int64_t>(s);
        }
        for (std::size_t phase = 0; phase < kPhases; ++phase) {
          CompetitorTraits<dispenso::ThreadPool>::parallelFor(
              *pool, std::size_t{0}, participants, participants,
              [phase, &slotState, &cal](std::size_t lo, std::size_t hi) {
                for (std::size_t i = lo; i < hi; ++i) {
                  spinForNs(kBodyNs, cal);
                  slotState[i] = advance(slotState[i], phase);
                }
              });
        }
        std::int64_t total = 0;
        for (const std::int64_t v : slotState) {
          total += v;
        }
        return total;
      },
      referenceTotal);
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMp(std::size_t participants, std::int64_t referenceTotal,
                                     const CyclesPerNanosecond &cal) {
  std::vector<std::int64_t> slotState(participants, 0);
  const auto threads = static_cast<int>(participants);
  return measureLoop(
      "OpenMP::parallel_for x30", cal,
      [&] {
        for (std::size_t s = 0; s < participants; ++s) {
          slotState[s] = static_cast<std::int64_t>(s);
        }
        const auto pSigned = static_cast<std::ptrdiff_t>(participants);
        for (std::size_t phase = 0; phase < kPhases; ++phase) {
#pragma omp parallel for num_threads(threads) schedule(static)
          for (std::ptrdiff_t i = 0; i < pSigned; ++i) {
            spinForNs(kBodyNs, cal);
            slotState[static_cast<std::size_t>(i)] =
                advance(slotState[static_cast<std::size_t>(i)], phase);
          }
        }
        std::int64_t total = 0;
        for (const std::int64_t v : slotState) {
          total += v;
        }
        return total;
      },
      referenceTotal);
}
#endif

// =============================================================================
// Future-pool shims (BS / dp / task / riften) -- uniform parallelChain shim
// =============================================================================

template <class PoolT>
[[nodiscard]] BenchRow measureFutureChain(const char *name, std::size_t participants,
                                          std::int64_t referenceTotal,
                                          const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);
  std::vector<std::int64_t> slotState(participants, 0);
  return measureLoop(
      name, cal,
      [&] {
        for (std::size_t s = 0; s < participants; ++s) {
          slotState[s] = static_cast<std::int64_t>(s);
        }
        Traits::parallelChain(
            *pool, std::size_t{0}, participants, participants, kPhases,
            [&slotState, &cal](std::size_t phase, std::size_t lo, std::size_t hi) {
              for (std::size_t i = lo; i < hi; ++i) {
                spinForNs(kBodyNs, cal);
                slotState[i] = advance(slotState[i], phase);
              }
            });
        std::int64_t total = 0;
        for (const std::int64_t v : slotState) {
          total += v;
        }
        return total;
      },
      referenceTotal);
}

// =============================================================================
// Table builder + registrar
// =============================================================================

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"runplex_stencil_"} + suffix;
  const std::int64_t referenceTotal = expectedFinalChecksum(participants, kPhases);
  table.rows.push_back(measureCitor(participants, referenceTotal, cal));
  table.rows.push_back(measureFutureChain<BS::light_thread_pool>(
      "BS::thread_pool[chainAdapter]", participants, referenceTotal, cal));
  table.rows.push_back(measureFutureChain<dp::thread_pool<>>("dp::thread_pool[chainAdapter]",
                                                             participants, referenceTotal, cal));
  table.rows.push_back(measureFutureChain<::task_thread_pool::task_thread_pool>(
      "task_thread_pool[chainAdapter]", participants, referenceTotal, cal));
  table.rows.push_back(measureFutureChain<riften::Thiefpool>("riften::Thiefpool[chainAdapter]",
                                                             participants, referenceTotal, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb(participants, referenceTotal, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureTaskflow(participants, referenceTotal, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureEigen(participants, referenceTotal, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOpenMp(participants, referenceTotal, cal));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(measureLeopard(participants, referenceTotal, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispenso(participants, referenceTotal, cal));
#endif
  return table;
}

BenchTable runStencilJ8(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/8, "j8_30phases_5us", cal);
}

BenchTable runStencilJ16(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/16, "j16_30phases_5us", cal);
}

struct StencilRegistrar {
  StencilRegistrar() {
    registerWorkload({.name = "runplex_stencil_j8_30phases_5us", .run = &runStencilJ8});
    registerWorkload({.name = "runplex_stencil_j16_30phases_5us", .run = &runStencilJ16});
  }
};

const StencilRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
