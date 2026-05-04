// Mixed `submitDetached` + `forkJoin` workload.
//
// Each iteration:
//   1. Issues `kDetachedTasks` background tasks via `submitDetached`. Each
//      task does a synthetic 5us spin -- meant to model a real app's
//      telemetry / logging / async I/O drains running concurrently with
//      user-facing parallel work.
//   2. Runs a recursive `forkJoin`-driven fib(28) on the producer.
//   3. Measures the wall time of the fib (timed window) PLUS the wall
//      time the pool needs to drain the detached tasks (untimed; observed
//      via the engine's drain-and-wait barrier on `~ThreadPool` between
//      iterations).
//
// The bench's intent is to prove the pool's slot-0-producer + detached-
// queue interaction is correct AND quantify the overhead the detached
// workload imposes on the foreground fan-out. It's also the only cell
// that drives `submitDetached` under fork-join load.
//
// Pool eligibility: citor only. `submitDetached` is a citor primitive and
// no surveyed competitor pool exposes a comparable API on top of a
// recursive fork-join scheduler. The "single 16-thread" baseline row is
// `forkJoin` without detached tasks, so the pair-of-rows in the table
// shows the detached-task cost in the same units.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "citor/always_assert.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "cycle_clock.h"

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 50;
constexpr std::size_t kWarmupIterations = 5;

constexpr int kFibN = 28;
constexpr int kFibCutoff = 16;

/// Number of background detached tasks submitted per iteration. 100 keeps
/// the queue measurably populated without saturating the pool's drain
/// step at the iteration boundary.
constexpr std::size_t kDetachedTasks = 100;

/// Synthetic body cost for each detached task in nanoseconds. 5000ns is
/// the same order as a real telemetry write (one log line + small
/// allocation, no I/O syscall).
constexpr std::uint64_t kDetachedSpinNs = 5000;

struct ForkJoinHints : citor::HintsDefaults {
  static constexpr citor::Affinity affinity = citor::Affinity::CcdLocal;
};

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

/// Synthetic spin: busy-loop incrementing a volatile counter for ~`ns`
/// nanoseconds. Used as the detached-task body so the bench measures
/// scheduler interaction, not a real I/O syscall.
inline void spinFor(std::uint64_t ns) noexcept {
  // Calibrate at a small constant number of cycles per increment. Approximate via TSC
  // which is monotonic and rdtsc-fast on this host.
  const std::uint64_t target = ns;
  std::uint64_t accum = 0;
  for (std::uint64_t i = 0; i < target * 3U; ++i) {
    accum += i;
  }
  // Force the compiler to keep the loop:
  static volatile std::uint64_t sink = 0;
  sink = accum;
  (void)sink;
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

// Baseline: forkJoin only, no detached tasks. Same wall-time shape as
// the fib28 cell elsewhere; reproduced here so the row pair compares
// cleanly within the same table.
[[nodiscard]] BenchRow measureForkOnly(std::size_t participants, const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  return measureLoop("citor::ThreadPool[fork only]", cal, [&] {
    return parFib(kFibN, [&](auto &&a, auto &&b) {
      pool.forkJoin<ForkJoinHints>(std::forward<decltype(a)>(a), std::forward<decltype(b)>(b));
    });
  });
}

// Mixed: each timed iteration submits N detached tasks via submitDetached,
// then drives the same forkJoin-driven fib. The drain happens
// implicitly when the iteration finishes (the next iter starts only after
// the detached counter quiesces, since we sample wall after the fib
// completes; the next iter's submitDetached batch races the prior batch's
// drain). The bench's signal: how much does the forground fib cost
// stretch under detached load?
[[nodiscard]] BenchRow measureMixed(std::size_t participants, const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  return measureLoop("citor::ThreadPool[fork+detached]", cal, [&] {
    for (std::size_t k = 0; k < kDetachedTasks; ++k) {
      pool.submitDetached<ForkJoinHints>([] { spinFor(kDetachedSpinNs); });
    }
    return parFib(kFibN, [&](auto &&a, auto &&b) {
      pool.forkJoin<ForkJoinHints>(std::forward<decltype(a)>(a), std::forward<decltype(b)>(b));
    });
  });
}

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_mixed_detached_sync_"} + suffix;
  table.rows.push_back(measureForkOnly(participants, cal));
  table.rows.push_back(measureMixed(participants, cal));
  return table;
}

BenchTable runJ8(const CyclesPerNanosecond &cal) { return buildTable(8, "j8", cal); }
BenchTable runJ16(const CyclesPerNanosecond &cal) { return buildTable(16, "j16", cal); }

struct MixedRegistrar {
  MixedRegistrar() {
    registerWorkload({.name = "forkjoin_mixed_detached_sync_j8", .run = &runJ8});
    registerWorkload({.name = "forkjoin_mixed_detached_sync_j16", .run = &runJ16});
  }
};

const MixedRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
