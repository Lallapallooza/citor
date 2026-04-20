// runPlex per-phase transition latency.
//
// Measures the gap between consecutive phases of `runPlex<NPhases>` at j=8 and
// j=16 workers, with each phase running a synthetic body of ~5 microseconds.
// The per-phase transition latency is the inter-arrival time between the
// producer publishing one phase epoch and the producer publishing the next,
// minus the known body cost; that difference captures the rendezvous and
// per-worker `done` flag round-trip the persistent-worker plex amortizes
// across one dispatch.
//
// Per-pool primitive mapping (every competitor lacks a native plex; the
// natural shape-equivalent is "N back-to-back parallelFor waves"):
//   - citor pool              -> `runPlex<PlexBenchHints>` (native plex).
//   - BS::thread_pool          -> N back-to-back `submit_blocks(0, j, body, j).wait()`.
//   - dp::thread_pool          -> N back-to-back fanouts of j enqueue futures + join.
//   - task_thread_pool         -> N back-to-back fanouts of j submit futures + join.
//   - riften::Thiefpool        -> N back-to-back fanouts of j enqueue futures + join.
//   - oneTBB                   -> N back-to-back `tbb::parallel_for` waves via arena.
//   - Taskflow                 -> N back-to-back taskflow runs (j tasks per flow).
//   - Eigen::ThreadPool        -> N back-to-back `Schedule + Barrier` waves.
//   - OpenMP                   -> N back-to-back `#pragma omp parallel for` regions.

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

// Hint preset at TU scope (not in an anonymous namespace) so clang-tidy treats every
// static-constexpr member as a public field of a named type rather than an unused constant.
struct PlexBenchHints {
  static constexpr citor::Balance balance =
      citor::Balance::StaticUniform;
  static constexpr citor::Priority priority =
      citor::Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
};

namespace citor::bench {
namespace {

/// Number of full plex calls per measurement.
constexpr std::size_t kIterations = 50;

/// Warmup plex calls dropped from the sample window.
constexpr std::size_t kWarmupIterations = 5;

/// Phases driven per plex call.
constexpr std::size_t kPlexPhases = 30;

/// Synthetic per-slot body cost in nanoseconds; ~5 microseconds.
constexpr double kBodyNs = 5'000.0;

/// Spin on the TSC for at least |targetNs| wall-time nanoseconds.
inline void spinForNs(double targetNs, const CyclesPerNanosecond &cal) noexcept {
  const std::uint64_t cycles =
      cal.value > 0.0 ? static_cast<std::uint64_t>(targetNs * cal.value) : 0ULL;
  const std::uint64_t start = readCyclesStart();
  while ((readCyclesEnd() - start) < cycles) {
    // Tight spin; empty body.
  }
}

/// Convert a measured per-call wall time (`callNs`) into a per-phase
/// transition latency by subtracting the known body cost (one body per phase
/// runs in parallel across slots, so total body time is `kPlexPhases * kBodyNs`)
/// and dividing by the phase count.
[[nodiscard]] inline double toTransitionNs(double callNs) noexcept {
  const double bodyNs = static_cast<double>(kPlexPhases) * kBodyNs;
  return (callNs - bodyNs) / static_cast<double>(kPlexPhases);
}

/// Generic measurement loop sampling per-call wall time.
template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name, const CyclesPerNanosecond &cal, RunFn run) {
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    run();
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(toTransitionNs(cyclesToNs(endCycles - startCycles, cal)));
  }
  std::sort(samples.begin(), samples.end());
  const double medianNs = samples[samples.size() / 2];
  const double opsPerSec = medianNs > 0.0 ? 1.0e9 / medianNs : 0.0;
  const double errPct = relativeStdDevPercent(samples);
  return BenchRow{
      .name = name,
      .nsPerOp = medianNs,
      .opsPerSec = opsPerSec,
      .errPercent = errPct,
  };
}

[[nodiscard]] BenchRow measureNewPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  std::atomic<std::uint64_t> sink{0};
  auto phaseFn = [&sink, &cal](std::size_t /*phaseIdx*/, std::uint32_t slot, std::size_t /*lo*/,
                               std::size_t /*hi*/, void * /*tlsArena*/) {
    spinForNs(kBodyNs, cal);
    sink.fetch_add(slot, std::memory_order_relaxed);
  };
  BenchRow row = measureLoop("citor::ThreadPool::runPlex", cal, [&] {
    pool.runPlex<PlexBenchHints>(kPlexPhases, /*n=*/participants, phaseFn);
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}

[[nodiscard]] BenchRow measureBsPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  std::atomic<std::uint64_t> sink{0};
  auto bodyChunk = [&sink, &cal](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      spinForNs(kBodyNs, cal);
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("BS::thread_pool::submit_blocks x30", cal, [&] {
    for (std::size_t p = 0; p < kPlexPhases; ++p) {
      pool.submit_blocks(std::size_t{0}, participants, bodyChunk, participants).wait();
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}

/// Common N-future fanout helper for dp/task/riften: each phase enqueues one
/// task per slot and joins. Documented per-row substitution: these pools have
/// no native multi-block primitive.
template <class Pool, class EnqueueFn>
[[nodiscard]] BenchRow measureFutureFanoutPool(const char *name, std::size_t participants,
                                               const CyclesPerNanosecond &cal, Pool &pool,
                                               EnqueueFn enqueue) {
  std::atomic<std::uint64_t> sink{0};
  auto slotBody = [&sink, &cal](std::size_t slot) {
    spinForNs(kBodyNs, cal);
    sink.fetch_add(slot, std::memory_order_relaxed);
  };
  BenchRow row = measureLoop(name, cal, [&] {
    for (std::size_t p = 0; p < kPlexPhases; ++p) {
      std::vector<std::future<void>> futs;
      futs.reserve(participants);
      for (std::size_t slot = 0; slot < participants; ++slot) {
        futs.emplace_back(enqueue(pool, [slot, &slotBody]() { slotBody(slot); }));
      }
      for (auto &f : futs) {
        f.get();
      }
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}

[[nodiscard]] BenchRow measureDpPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  return measureFutureFanoutPool(
      "dp::thread_pool::enqueue x30j", participants, cal, pool,
      [](dp::thread_pool<> &p, auto fn) { return p.enqueue(std::move(fn)); });
}

[[nodiscard]] BenchRow measureTaskPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(static_cast<unsigned int>(participants));
  return measureFutureFanoutPool(
      "task_thread_pool::submit x30j", participants, cal, pool,
      [](::task_thread_pool::task_thread_pool &p, auto fn) { return p.submit(std::move(fn)); });
}

[[nodiscard]] BenchRow measureRiftenPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  return measureFutureFanoutPool(
      "riften::Thiefpool::enqueue x30j", participants, cal, pool,
      [](riften::Thiefpool &p, auto fn) { return p.enqueue(std::move(fn)); });
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbbPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  std::atomic<std::uint64_t> sink{0};
  auto bodyChunk = [&sink, &cal](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      spinForNs(kBodyNs, cal);
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("oneTBB::parallel_for x30", cal, [&] {
    for (std::size_t p = 0; p < kPlexPhases; ++p) {
      CompetitorTraits<::tbb::task_arena>::parallelFor(*arena, std::size_t{0}, participants, 1,
                                                       bodyChunk);
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
  auto bodyChunk = [&sink, &cal](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      spinForNs(kBodyNs, cal);
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("Taskflow::run x30", cal, [&] {
    for (std::size_t p = 0; p < kPlexPhases; ++p) {
      CompetitorTraits<::tf::Executor>::parallelFor(*exec, std::size_t{0}, participants,
                                                    participants, bodyChunk);
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] BenchRow measureEigenPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  std::atomic<std::uint64_t> sink{0};
  auto bodyChunk = [&sink, &cal](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      spinForNs(kBodyNs, cal);
      sink.fetch_add(i, std::memory_order_relaxed);
    }
  };
  BenchRow row = measureLoop("Eigen::ThreadPool::Schedule x30", cal, [&] {
    for (std::size_t p = 0; p < kPlexPhases; ++p) {
      CompetitorTraits<::Eigen::ThreadPool>::parallelFor(*pool, std::size_t{0}, participants,
                                                         participants, bodyChunk);
    }
  });
  (void)sink.load(std::memory_order_relaxed);
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMpPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  std::atomic<std::uint64_t> sink{0};
  BenchRow row = measureLoop("OpenMP::parallel_for x30", cal, [&] {
    const int threads = static_cast<int>(participants);
    const auto lastSigned = static_cast<std::ptrdiff_t>(participants);
    for (std::size_t p = 0; p < kPlexPhases; ++p) {
#pragma omp parallel for num_threads(threads) schedule(static)
      for (std::ptrdiff_t i = 0; i < lastSigned; ++i) {
        spinForNs(kBodyNs, cal);
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
  table.workload = std::string{"plex_transition_"} + suffix;
  table.rows.push_back(measureNewPool(participants, cal));
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
  return table;
}

/// 30-phase x 16-worker x 5 us body; the spec's headline workload.
BenchTable runPlexJ16(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/16, "j16_30phases_5us", cal);
}

/// 30-phase x 8-worker variant; sanity-check that mid-tier participants do
/// not regress.
BenchTable runPlexJ8(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/8, "j8_30phases_5us", cal);
}

/// File-scope registrar; pushes the workloads into the bench registry at TU
/// initialization time.
struct PlexRegistrar {
  PlexRegistrar() {
    registerWorkload({.name = "plex_transition_j8_30phases_5us", .run = &runPlexJ8});
    registerWorkload({.name = "plex_transition_j16_30phases_5us", .run = &runPlexJ16});
  }
};

const PlexRegistrar kRegistrar;

} // namespace

} // namespace citor::bench
