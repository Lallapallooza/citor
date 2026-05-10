// Recursive fork-join workload for the comparative pool bench.
//
// Two workloads:
//   - fib(N): naive recursive Fibonacci with a sequential cutoff.
//   - nqueens(N): backtracking N-queens count, parallelized over root branches.
//
// Pool eligibility: only pools that integrate recursive submit-and-wait with
// the scheduler participate. The recursive shape used here (`spawn(a, b)`
// followed by waiting on both) requires the waiting thread to either run
// children itself or yield work to other workers; otherwise every level of
// recursion consumes a worker forever and the call deadlocks once depth
// exceeds the worker count.
//
// Pools rendered:
//   - citor::ThreadPool  -> native `forkJoin<Hints>(taskA, taskB)` recursion;
//                            the producer participates and runs children
//                            in-place.
//   - oneTBB             -> `tbb::task_group::run` + `wait`; TBB's wait drains
//                            the arena, descheduling itself to run children.
//   - OpenMP             -> `#pragma omp parallel single` wrapper opens a
//                            region; the inner recursion uses `#pragma omp
//                            task` + `#pragma omp taskwait`.
//   - dispenso           -> `dispenso::TaskSet::schedule` + dtor wait; the
//                            TaskSet drains via the scheduler so nested calls
//                            from a worker do not deadlock (verified via
//                            scratch/forkjoin_dispenso_probe.cpp).
//   - Taskflow::Subflow  -> per-recursion-level `tf::Subflow::emplace` +
//                            `subflow.join()`. Each recursion creates a fresh
//                            child Subflow because the spawn(a,b) shape used
//                            for the other pools cannot share a single Subflow
//                            across recursion levels (Subflow.join() is
//                            single-shot per Subflow); a Subflow-specific
//                            recursion implementation lives below.
//   - libfork            -> coroutine-based fork-join via `lf::fork` /
//                            `lf::call` / `lf::join`; runner lives in
//                            libfork_runners.cpp.
//   - tmc                -> coroutine-based fork-join via
//                            `tmc::spawn_tuple(a, b)`; runner lives in
//                            tmc_runners.cpp.
//   - Sequential         -> single-thread baseline so parallel speedup is
//                            visible in the table.
//
// Pools excluded:
//   - BS / dp / task_thread_pool / Eigen -- no recursive scheduling.
//   - Taskflow `Executor::async` + `std::future::get()` -- `.get()` blocks the
//     calling worker on a condition variable that Taskflow's scheduler does
//     not coordinate with, so deep recursion deadlocks. The Subflow-based
//     row above is Taskflow's first-class recursive shape.
//   - riften::Thiefpool -- same `std::future::get()` blocking issue as
//     Taskflow `Executor::async` when called from outside the pool.
//   - Leopard::ThreadPool -- nested `dispatch(false, fn).get()` deadlocks the
//     calling worker on its own future, livelocking the global queue. Verified
//     via scratch/forkjoin_leopard_probe.cpp (10s timeout on fib(22)).

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
#include "libfork_runners.h"
#include "recursive_forkjoin_helper.h"
#include "tmc_runners.h"

namespace citor::bench {

// Compile-time gate confirming the recursive-spawn trait specializations
// match the fork-join eligibility list documented at the top of this file.
// Recursive workloads (cilksort, Strassen) consume these specializations
// directly; keeping the asserts in the bench TU that already pulls the
// header keeps the gate close to the contract it enforces.
static_assert(
    RecursiveForkJoinTraits<::citor::ThreadPool>::supportsRecursiveSpawn,
    "citor::ThreadPool must support recursive spawn for the fork-join bench");
#ifdef CITOR_BENCH_HAS_TBB
static_assert(
    RecursiveForkJoinTraits<::tbb::task_arena>::supportsRecursiveSpawn,
    "oneTBB task_arena must support recursive spawn for the fork-join bench");
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
static_assert(
    RecursiveForkJoinTraits<::dispenso::ThreadPool>::supportsRecursiveSpawn,
    "dispenso::ThreadPool must support recursive spawn for the fork-join "
    "bench");
#endif

} // namespace citor::bench

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
#include <dispenso/task_set.h>
#include <dispenso/thread_pool.h>
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
#include <omp.h>
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

struct ForkJoinHints : citor::HintsDefaults {
  static constexpr citor::StealPolicy stealPolicy =
      citor::StealPolicy::ClusterLocal;
};

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 50;
constexpr std::size_t kWarmupIterations = 5;

constexpr int kFibN = 28;
constexpr int kFibCutoff = 16;

// Fine-grained variant: fib(20) with cutoff=2 (n<2 base case only). ~13K
// recursive spawns per iteration. Mirrors libfork's published shape
// (`bench/source/fib/config.hpp` runs fib(42) with no cutoff so every level
// forks); 20 is the largest fan-out we can run here without overwhelming
// Taskflow Subflow's per-emplace bookkeeping (which crashes at fib(25)).
constexpr int kFibFineN = 20;
constexpr int kFibFineCutoff = 2;

constexpr int kQueensN = 12;
constexpr int kQueensRootDepth = 2;

[[nodiscard]] std::int64_t seqFib(int n) noexcept {
  if (n < 2) {
    return n;
  }
  std::int64_t a = 0;
  std::int64_t b = 1;
  for (int i = 2; i <= n; ++i) {
    const std::int64_t c = a + b;
    a = b;
    b = c;
  }
  return b;
}

template <class Spawn>
[[nodiscard]] std::int64_t parFibCutoff(int n, int cutoff, Spawn spawn) {
  if (n <= cutoff) {
    return seqFib(n);
  }
  std::int64_t a = 0;
  std::int64_t b = 0;
  spawn([&] { a = parFibCutoff(n - 1, cutoff, spawn); },
        [&] { b = parFibCutoff(n - 2, cutoff, spawn); });
  return a + b;
}

template <class Spawn>
[[nodiscard]] std::int64_t parFib(int n, Spawn spawn) {
  return parFibCutoff(n, kFibCutoff, spawn);
}

/// Sequential N-queens count, used both standalone and as a leaf of the
/// parallel variant once the root-branching depth has been exhausted.
void seqQueensRec(int n, int row, std::uint64_t cols, std::uint64_t diag1,
                  std::uint64_t diag2, std::int64_t &count) noexcept {
  if (row == n) {
    ++count;
    return;
  }
  std::uint64_t bits = ~(cols | diag1 | diag2) &
                       ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
  while (bits != 0U) {
    const std::uint64_t pick = bits & (~bits + 1U);
    bits ^= pick;
    seqQueensRec(n, row + 1, cols | pick, (diag1 | pick) << 1U,
                 (diag2 | pick) >> 1U, count);
  }
}

[[nodiscard]] std::int64_t seqQueens(int n) noexcept {
  std::int64_t count = 0;
  seqQueensRec(n, 0, 0, 0, 0, count);
  return count;
}

/// Parallel N-queens: enumerate the first `kQueensRootDepth` rows in parallel
/// (each a separate fork-join task), then drop into the sequential walker for
/// the remaining rows.
template <class Spawn>
[[nodiscard]] std::int64_t parQueens(int n, Spawn spawn) {
  // Generate root states by enumerating the first `kQueensRootDepth` rows
  // sequentially; each root is then a parallel task.
  struct State {
    std::uint64_t cols;
    std::uint64_t diag1;
    std::uint64_t diag2;
  };
  std::vector<State> roots;
  roots.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
  std::vector<State> frontier{State{0U, 0U, 0U}};
  for (int depth = 0; depth < kQueensRootDepth && !frontier.empty(); ++depth) {
    std::vector<State> next;
    next.reserve(frontier.size() * static_cast<std::size_t>(n));
    for (const State &s : frontier) {
      std::uint64_t bits = ~(s.cols | s.diag1 | s.diag2) &
                           ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
      while (bits != 0U) {
        const std::uint64_t pick = bits & (~bits + 1U);
        bits ^= pick;
        next.push_back(
            {s.cols | pick, (s.diag1 | pick) << 1U, (s.diag2 | pick) >> 1U});
      }
    }
    frontier = std::move(next);
  }
  roots = frontier;

  std::vector<std::int64_t> partials(roots.size(), 0);

  // Recursive parallel walk: split the root list in half until we hit a single
  // task, then run the sequential walker for that root.
  auto run = [&](std::size_t lo, std::size_t hi, auto &self) -> void {
    if (hi - lo == 1) {
      const State &s = roots[lo];
      std::int64_t count = 0;
      seqQueensRec(n, kQueensRootDepth, s.cols, s.diag1, s.diag2, count);
      partials[lo] = count;
      return;
    }
    const std::size_t mid = lo + ((hi - lo) / 2);
    spawn([&] { self(lo, mid, self); }, [&] { self(mid, hi, self); });
  };
  if (!roots.empty()) {
    run(0, roots.size(), run);
  }
  std::int64_t total = 0;
  for (const std::int64_t v : partials) {
    total += v;
  }
  return total;
}

template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name,
                                   const CyclesPerNanosecond &cal, RunFn run) {
  std::atomic<std::int64_t> sink{0};
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    sink.store(run(), std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
  }
  (void)sink.load(std::memory_order_relaxed);
  return finalizeRow(name, samples);
}

// =============================================================================
// citor -- native forkJoin
// =============================================================================

template <class Workload>
[[nodiscard]] BenchRow measureCitor(const char *name, std::size_t participants,
                                    const CyclesPerNanosecond &cal,
                                    Workload workload) {
  ThreadPool pool(participants);
  return measureLoop(name, cal, [&] {
    return workload([&](auto &&a, auto &&b) {
      pool.forkJoin<ForkJoinHints>(std::forward<decltype(a)>(a),
                                   std::forward<decltype(b)>(b));
    });
  });
}

// =============================================================================
// oneTBB -- task_group::run + wait
// =============================================================================

#ifdef CITOR_BENCH_HAS_TBB
template <class Workload>
[[nodiscard]] BenchRow measureTbb(const char *name, std::size_t participants,
                                  const CyclesPerNanosecond &cal,
                                  Workload workload) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  return measureLoop(name, cal, [&] {
    std::int64_t result = 0;
    arena->execute([&] {
      result = workload([&](auto &&a, auto &&b) {
        ::tbb::task_group tg;
        tg.run(std::forward<decltype(a)>(a));
        tg.run(std::forward<decltype(b)>(b));
        tg.wait();
      });
    });
    return result;
  });
}
#endif

// =============================================================================
// dispenso -- TaskSet::schedule + dtor wait
// =============================================================================

#ifdef CITOR_BENCH_HAS_DISPENSO
template <class Workload>
[[nodiscard]] BenchRow
measureDispenso(const char *name, std::size_t participants,
                const CyclesPerNanosecond &cal, Workload workload) {
  auto pool = CompetitorTraits<::dispenso::ThreadPool>::make(participants);
  return measureLoop(name, cal, [&] {
    return workload([&pool](auto &&a, auto &&b) {
      // ForceQueuingTag bypasses dispenso's anti-recursion-flooding throttle;
      // without it both children inline on the calling worker once the queue
      // has > numThreads*1.5 tasks in flight (task_set_impl.h:180-183), which
      // serializes the fork-join and defeats the spawn(a, b) contract.
      ::dispenso::TaskSet ts(*pool);
      ts.schedule(std::forward<decltype(a)>(a), ::dispenso::ForceQueuingTag{});
      ts.schedule(std::forward<decltype(b)>(b), ::dispenso::ForceQueuingTag{});
    });
  });
}
#endif

// =============================================================================
// OpenMP -- parallel single + task / taskwait
// =============================================================================

#ifdef CITOR_BENCH_HAS_OPENMP
template <class Workload>
[[nodiscard]] BenchRow measureOmp(const char *name, std::size_t participants,
                                  const CyclesPerNanosecond &cal,
                                  Workload workload) {
  return measureLoop(name, cal, [&] {
    std::int64_t result = 0;
#pragma omp parallel num_threads(static_cast<int>(participants))
    {
#pragma omp single
      {
        result = workload([](auto &&a, auto &&b) {
          using A = decltype(a);
          using B = decltype(b);
#pragma omp task shared(a)
          std::forward<A>(a)();
#pragma omp task shared(b)
          std::forward<B>(b)();
#pragma omp taskwait
        });
      }
    }
    return result;
  });
}
#endif

// =============================================================================
// Taskflow Subflow -- emplace + join (per-level subflow)
// =============================================================================

#ifdef CITOR_BENCH_HAS_TASKFLOW
// Subflow-specific parFib: each recursion level creates two child subflows via
// `subflow.emplace(...)`, then joins. Cannot share the top-level `parFib`'s
// `spawn(a, b)` shape because each recursion needs the local `tf::Subflow&`.
[[nodiscard]] std::int64_t parFibSubflowCutoff(int n, int cutoff,
                                               ::tf::Subflow &sub) {
  if (n <= cutoff) {
    return seqFib(n);
  }
  std::int64_t a = 0;
  std::int64_t b = 0;
  sub.emplace([&a, n, cutoff](::tf::Subflow &child) {
    a = parFibSubflowCutoff(n - 1, cutoff, child);
  });
  sub.emplace([&b, n, cutoff](::tf::Subflow &child) {
    b = parFibSubflowCutoff(n - 2, cutoff, child);
  });
  sub.join();
  return a + b;
}

[[nodiscard]] std::int64_t parFibSubflow(int n, ::tf::Subflow &sub) {
  return parFibSubflowCutoff(n, kFibCutoff, sub);
}

[[nodiscard]] std::int64_t parQueensSubflow(int n, ::tf::Subflow &rootSub) {
  struct State {
    std::uint64_t cols;
    std::uint64_t diag1;
    std::uint64_t diag2;
  };
  std::vector<State> roots;
  std::vector<State> frontier{State{0U, 0U, 0U}};
  for (int depth = 0; depth < kQueensRootDepth && !frontier.empty(); ++depth) {
    std::vector<State> next;
    next.reserve(frontier.size() * static_cast<std::size_t>(n));
    for (const State &s : frontier) {
      std::uint64_t bits = ~(s.cols | s.diag1 | s.diag2) &
                           ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
      while (bits != 0U) {
        const std::uint64_t pick = bits & (~bits + 1U);
        bits ^= pick;
        next.push_back(
            {s.cols | pick, (s.diag1 | pick) << 1U, (s.diag2 | pick) >> 1U});
      }
    }
    frontier = std::move(next);
  }
  roots = frontier;
  std::vector<std::int64_t> partials(roots.size(), 0);

  auto run = [&](std::size_t lo, std::size_t hi, ::tf::Subflow &sub,
                 auto &self) -> void {
    if (hi - lo == 1) {
      const State &s = roots[lo];
      std::int64_t count = 0;
      seqQueensRec(n, kQueensRootDepth, s.cols, s.diag1, s.diag2, count);
      partials[lo] = count;
      return;
    }
    const std::size_t mid = lo + ((hi - lo) / 2);
    sub.emplace(
        [&self, lo, mid](::tf::Subflow &child) { self(lo, mid, child, self); });
    sub.emplace(
        [&self, mid, hi](::tf::Subflow &child) { self(mid, hi, child, self); });
    sub.join();
  };
  if (!roots.empty()) {
    run(0, roots.size(), rootSub, run);
  }
  std::int64_t total = 0;
  for (const std::int64_t v : partials) {
    total += v;
  }
  return total;
}

[[nodiscard]] BenchRow measureTaskflowFib(const char *name,
                                          std::size_t participants,
                                          const CyclesPerNanosecond &cal) {
  ::tf::Executor exec(participants);
  return measureLoop(name, cal, [&] {
    std::int64_t result = 0;
    ::tf::Taskflow flow;
    flow.emplace([&result](::tf::Subflow &root) {
      result = parFibSubflow(kFibN, root);
    });
    exec.run(flow).wait();
    return result;
  });
}

[[nodiscard]] BenchRow measureTaskflowFibFine(const char *name,
                                              std::size_t participants,
                                              const CyclesPerNanosecond &cal) {
  ::tf::Executor exec(participants);
  return measureLoop(name, cal, [&] {
    std::int64_t result = 0;
    ::tf::Taskflow flow;
    flow.emplace([&result](::tf::Subflow &root) {
      result = parFibSubflowCutoff(kFibFineN, kFibFineCutoff, root);
    });
    exec.run(flow).wait();
    return result;
  });
}

[[nodiscard]] BenchRow measureTaskflowQueens(const char *name,
                                             std::size_t participants,
                                             const CyclesPerNanosecond &cal) {
  ::tf::Executor exec(participants);
  return measureLoop(name, cal, [&] {
    std::int64_t result = 0;
    ::tf::Taskflow flow;
    flow.emplace([&result](::tf::Subflow &root) {
      result = parQueensSubflow(kQueensN, root);
    });
    exec.run(flow).wait();
    return result;
  });
}
#endif

// =============================================================================
// Sequential baseline
// =============================================================================

[[nodiscard]] BenchRow measureSeqFib(const CyclesPerNanosecond &cal) {
  return measureLoop("Sequential", cal, [&] { return seqFib(kFibN); });
}

[[nodiscard]] BenchRow measureSeqQueens(const CyclesPerNanosecond &cal) {
  return measureLoop("Sequential", cal, [&] { return seqQueens(kQueensN); });
}

// =============================================================================
// Workload definitions -- body returns the result so the harness can sink it.
// =============================================================================

auto fibWorkload() {
  return [](auto spawn) { return parFib(kFibN, spawn); };
}

auto fibFineWorkload() {
  return
      [](auto spawn) { return parFibCutoff(kFibFineN, kFibFineCutoff, spawn); };
}

[[nodiscard]] BenchRow measureSeqFibFine(const CyclesPerNanosecond &cal) {
  return measureLoop("Sequential", cal, [&] { return seqFib(kFibFineN); });
}

// Torture-grade: fib(N) with cutoff=2 (every n>=2 forks). Mirrors libfork's
// published bench shape (`bench/source/fib/config.hpp` runs fib(42) with no
// cutoff). Drop Taskflow Subflow + dispenso here -- they crash or stall in
// the per-emplace bookkeeping at this depth. Only citor / TBB / OMP /
// libfork / TMC participate.
auto fibTortureWorkload(int n) {
  return [n](auto spawn) { return parFibCutoff(n, 2, spawn); };
}

[[nodiscard]] BenchRow measureSeqFibTorture(int n,
                                            const CyclesPerNanosecond &cal) {
  return measureLoop("Sequential", cal, [n] { return seqFib(n); });
}

auto queensWorkload() {
  return [](auto spawn) { return parQueens(kQueensN, spawn); };
}

// =============================================================================
// Tables
// =============================================================================

BenchTable buildFibFineTable(std::size_t participants, const char *suffix,
                             const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_fib_fine_"} + suffix;
  table.rows.push_back(
      measureCitor("citor::ThreadPool", participants, cal, fibFineWorkload()));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(
      measureTbb("oneTBB", participants, cal, fibFineWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(
      measureOmp("OpenMP", participants, cal, fibFineWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispenso("dispenso::ThreadPool", participants,
                                       cal, fibFineWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(
      measureTaskflowFibFine("Taskflow::Subflow", participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LIBFORK
  table.rows.push_back(
      runLibforkFibFine(participants, kFibFineN, kFibFineCutoff, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(
      runTmcFibFine(participants, kFibFineN, kFibFineCutoff, cal));
#endif
  table.rows.push_back(measureSeqFibFine(cal));
  return table;
}

BenchTable buildFibTortureTable(std::size_t participants, int n,
                                const char *suffix,
                                const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_fib_torture_"} + suffix;
  table.rows.push_back(measureCitor("citor::ThreadPool", participants, cal,
                                    fibTortureWorkload(n)));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(
      measureTbb("oneTBB", participants, cal, fibTortureWorkload(n)));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(
      measureOmp("OpenMP", participants, cal, fibTortureWorkload(n)));
#endif
#ifdef CITOR_BENCH_HAS_LIBFORK
  table.rows.push_back(runLibforkFibFine(participants, n, /*cutoff=*/2, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(runTmcFibFine(participants, n, /*cutoff=*/2, cal));
#endif
  table.rows.push_back(measureSeqFibTorture(n, cal));
  return table;
}

BenchTable buildFibTable(std::size_t participants, const char *suffix,
                         const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_fib28_"} + suffix;
  table.rows.push_back(
      measureCitor("citor::ThreadPool", participants, cal, fibWorkload()));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb("oneTBB", participants, cal, fibWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOmp("OpenMP", participants, cal, fibWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispenso("dispenso::ThreadPool", participants,
                                       cal, fibWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(
      measureTaskflowFib("Taskflow::Subflow", participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LIBFORK
  table.rows.push_back(runLibforkFib28(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(runTmcFib28(participants, cal));
#endif
  table.rows.push_back(measureSeqFib(cal));
  return table;
}

BenchTable buildQueensTable(std::size_t participants, const char *suffix,
                            const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_nqueens12_"} + suffix;
  table.rows.push_back(
      measureCitor("citor::ThreadPool", participants, cal, queensWorkload()));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(
      measureTbb("oneTBB", participants, cal, queensWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(
      measureOmp("OpenMP", participants, cal, queensWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispenso("dispenso::ThreadPool", participants,
                                       cal, queensWorkload()));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(
      measureTaskflowQueens("Taskflow::Subflow", participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(
      measureTaskflowQueens("Taskflow::Subflow", participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LIBFORK
  table.rows.push_back(runLibforkNQueens12(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(runTmcNQueens12(participants, cal));
#endif
  table.rows.push_back(measureSeqQueens(cal));
  return table;
}

BenchTable runFibJ8(const CyclesPerNanosecond &cal) {
  return buildFibTable(8, "j8", cal);
}
BenchTable runFibJ16(const CyclesPerNanosecond &cal) {
  return buildFibTable(16, "j16", cal);
}
BenchTable runFibFineJ8(const CyclesPerNanosecond &cal) {
  return buildFibFineTable(8, "j8", cal);
}
BenchTable runFibFineJ16(const CyclesPerNanosecond &cal) {
  return buildFibFineTable(16, "j16", cal);
}
BenchTable runFibTortureN30J8(const CyclesPerNanosecond &cal) {
  return buildFibTortureTable(8, 30, "n30_j8", cal);
}
BenchTable runFibTortureN30J16(const CyclesPerNanosecond &cal) {
  return buildFibTortureTable(16, 30, "n30_j16", cal);
}
BenchTable runFibTortureN35J8(const CyclesPerNanosecond &cal) {
  return buildFibTortureTable(8, 35, "n35_j8", cal);
}
BenchTable runFibTortureN35J16(const CyclesPerNanosecond &cal) {
  return buildFibTortureTable(16, 35, "n35_j16", cal);
}
BenchTable runQueensJ8(const CyclesPerNanosecond &cal) {
  return buildQueensTable(8, "j8", cal);
}
BenchTable runQueensJ16(const CyclesPerNanosecond &cal) {
  return buildQueensTable(16, "j16", cal);
}

struct ForkJoinRegistrar {
  ForkJoinRegistrar() {
    registerWorkload({.name = "forkjoin_fib28_j8", .run = &runFibJ8});
    registerWorkload({.name = "forkjoin_fib28_j16", .run = &runFibJ16});
    registerWorkload({.name = "forkjoin_fib_fine_j8", .run = &runFibFineJ8});
    registerWorkload({.name = "forkjoin_fib_fine_j16", .run = &runFibFineJ16});
    registerWorkload(
        {.name = "forkjoin_fib_torture_n30_j8", .run = &runFibTortureN30J8});
    registerWorkload(
        {.name = "forkjoin_fib_torture_n30_j16", .run = &runFibTortureN30J16});
    registerWorkload(
        {.name = "forkjoin_fib_torture_n35_j8", .run = &runFibTortureN35J8});
    registerWorkload(
        {.name = "forkjoin_fib_torture_n35_j16", .run = &runFibTortureN35J16});
    registerWorkload({.name = "forkjoin_nqueens12_j8", .run = &runQueensJ8});
    registerWorkload({.name = "forkjoin_nqueens12_j16", .run = &runQueensJ16});
  }
};

const ForkJoinRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
