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
//   - Sequential         -> single-thread baseline so parallel speedup is
//                            visible in the table.
//
// Pools excluded:
//   - BS / dp / task_thread_pool / Eigen / OpenMP -- no recursive scheduling.
//   - Taskflow `Executor::async` + `std::future::get()` -- `.get()` blocks the
//     calling worker on a condition variable that Taskflow's scheduler does
//     not coordinate with, so deep recursion deadlocks. Taskflow's first-class
//     recursive shape is `tf::Subflow::emplace`, which has a different
//     callback signature than the generic `spawn(a, b)` used here. A future
//     extension can register a Subflow-shaped variant.
//   - riften::Thiefpool -- same `std::future::get()` blocking issue as
//     Taskflow when called from outside the pool.

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
#include "recursive_forkjoin_helper.h"

namespace citor::bench {

// Compile-time gate confirming the recursive-spawn trait specializations
// match the fork-join eligibility list documented at the top of this file.
// Recursive workloads (cilksort, Strassen) consume these specializations
// directly; keeping the asserts in the bench TU that already pulls the
// header keeps the gate close to the contract it enforces.
static_assert(RecursiveForkJoinTraits<::citor::ThreadPool>::supportsRecursiveSpawn,
              "citor::ThreadPool must support recursive spawn for the fork-join bench");
#ifdef CITOR_BENCH_HAS_TBB
static_assert(RecursiveForkJoinTraits<::tbb::task_arena>::supportsRecursiveSpawn,
              "oneTBB task_arena must support recursive spawn for the fork-join bench");
#endif

} // namespace citor::bench

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

struct ForkJoinHints : citor::HintsDefaults {
  static constexpr citor::Affinity affinity = citor::Affinity::SplitCcd;
};

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 50;
constexpr std::size_t kWarmupIterations = 5;

constexpr int kFibN = 28;
constexpr int kFibCutoff = 16;

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

template <class Spawn> [[nodiscard]] std::int64_t parFib(int n, Spawn spawn) {
  if (n <= kFibCutoff) {
    return seqFib(n);
  }
  std::int64_t a = 0;
  std::int64_t b = 0;
  spawn([&] { a = parFib(n - 1, spawn); }, [&] { b = parFib(n - 2, spawn); });
  return a + b;
}

/// Sequential N-queens count, used both standalone and as a leaf of the
/// parallel variant once the root-branching depth has been exhausted.
void seqQueensRec(int n, int row, std::uint64_t cols, std::uint64_t diag1, std::uint64_t diag2,
                  std::int64_t &count) noexcept {
  if (row == n) {
    ++count;
    return;
  }
  std::uint64_t bits =
      ~(cols | diag1 | diag2) & ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
  while (bits != 0U) {
    const std::uint64_t pick = bits & (~bits + 1U);
    bits ^= pick;
    seqQueensRec(n, row + 1, cols | pick, (diag1 | pick) << 1U, (diag2 | pick) >> 1U, count);
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
template <class Spawn> [[nodiscard]] std::int64_t parQueens(int n, Spawn spawn) {
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
      std::uint64_t bits =
          ~(s.cols | s.diag1 | s.diag2) & ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
      while (bits != 0U) {
        const std::uint64_t pick = bits & (~bits + 1U);
        bits ^= pick;
        next.push_back({s.cols | pick, (s.diag1 | pick) << 1U, (s.diag2 | pick) >> 1U});
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
[[nodiscard]] BenchRow measureLoop(const char *name, const CyclesPerNanosecond &cal, RunFn run) {
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
                                    const CyclesPerNanosecond &cal, Workload workload) {
  ThreadPool pool(participants);
  return measureLoop(name, cal, [&] {
    return workload([&](auto &&a, auto &&b) {
      pool.forkJoin<ForkJoinHints>(std::forward<decltype(a)>(a), std::forward<decltype(b)>(b));
    });
  });
}

// =============================================================================
// oneTBB -- task_group::run + wait
// =============================================================================

#ifdef CITOR_BENCH_HAS_TBB
template <class Workload>
[[nodiscard]] BenchRow measureTbb(const char *name, std::size_t participants,
                                  const CyclesPerNanosecond &cal, Workload workload) {
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
// Taskflow -- Executor::async with future.get()
// =============================================================================

// Taskflow `Executor::async` + `std::future::get()` and riften::Thiefpool's
// future-based `enqueue` shape both deadlock under recursive `spawn(a, b)`
// because `.get()` blocks the calling worker without yielding scheduling
// rights to children. They are excluded from the registered fork-join
// workloads -- see file header.

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

auto queensWorkload() {
  return [](auto spawn) { return parQueens(kQueensN, spawn); };
}

// =============================================================================
// Tables
// =============================================================================

BenchTable buildFibTable(std::size_t participants, const char *suffix,
                         const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_fib28_"} + suffix;
  table.rows.push_back(measureCitor("citor::ThreadPool", participants, cal, fibWorkload()));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb("oneTBB", participants, cal, fibWorkload()));
#endif
  table.rows.push_back(measureSeqFib(cal));
  return table;
}

BenchTable buildQueensTable(std::size_t participants, const char *suffix,
                            const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_nqueens12_"} + suffix;
  table.rows.push_back(measureCitor("citor::ThreadPool", participants, cal, queensWorkload()));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb("oneTBB", participants, cal, queensWorkload()));
#endif
  table.rows.push_back(measureSeqQueens(cal));
  return table;
}

BenchTable runFibJ8(const CyclesPerNanosecond &cal) { return buildFibTable(8, "j8", cal); }
BenchTable runFibJ16(const CyclesPerNanosecond &cal) { return buildFibTable(16, "j16", cal); }
BenchTable runQueensJ8(const CyclesPerNanosecond &cal) { return buildQueensTable(8, "j8", cal); }
BenchTable runQueensJ16(const CyclesPerNanosecond &cal) { return buildQueensTable(16, "j16", cal); }

struct ForkJoinRegistrar {
  ForkJoinRegistrar() {
    registerWorkload({.name = "forkjoin_fib28_j8", .run = &runFibJ8});
    registerWorkload({.name = "forkjoin_fib28_j16", .run = &runFibJ16});
    registerWorkload({.name = "forkjoin_nqueens12_j8", .run = &runQueensJ8});
    registerWorkload({.name = "forkjoin_nqueens12_j16", .run = &runQueensJ16});
  }
};

const ForkJoinRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
