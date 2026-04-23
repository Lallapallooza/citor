// parallelReduce workload for the comparative pool bench.
//
// Sum reduction over a fixed-seed input vector. The citor pool drives the
// native two-pass `parallelReduce`; competitor pools that lack a first-class
// reduce primitive (BS, dp, task, riften) emulate it via the same per-block
// partials + serial merge shape `scan_bench` uses, keeping the comparison
// apples-to-apples.
//
// Shapes registered:
//   - sum int64 over n=1M at j in {2, 8, 16}
//   - sum double Kahan vs plain at n=1M, j in {8, 16}
//
// Primitive mapping per pool:
//   - citor pool              -> `parallelReduce<ReduceBenchHints>` (native).
//   - BS::light_thread_pool   -> two-wave: per-block partials via `submit_blocks`,
//                                serial merge.
//   - dp::thread_pool         -> two-wave: per-block partials via `enqueue`
//                                returning `std::future<T>`.
//   - task_thread_pool        -> same as dp via `submit`.
//   - riften::Thiefpool       -> same shape via `enqueue`.
//   - oneTBB                  -> `tbb::parallel_reduce` via `task_arena::execute`.
//   - Taskflow                -> per-block partials via `for_each_index`.
//   - Eigen::ThreadPool       -> per-block partials via Schedule + Barrier wave.
//   - OpenMP                  -> `parallel for reduction(+:)`.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

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
#include <taskflow/algorithm/for_each.hpp>
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
struct ReduceBenchHints {
  static constexpr citor::Balance balance = citor::Balance::StaticUniform;
  static constexpr citor::Determinism determinism = citor::Determinism::FixedBlockOrder;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  static constexpr citor::Partition partition = citor::Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
};

struct ReduceBenchKahanHints {
  static constexpr citor::Balance balance = citor::Balance::StaticUniform;
  static constexpr citor::Determinism determinism = citor::Determinism::KahanCompensated;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  static constexpr citor::Partition partition = citor::Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
};

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 50;
constexpr std::size_t kWarmupIterations = 5;
constexpr std::size_t kN = 1'000'000;

template <class T> struct ReduceData {
  std::vector<T> in;
};

template <class T> [[nodiscard]] ReduceData<T> buildData() {
  ReduceData<T> d;
  d.in.assign(kN, T{0});
  for (std::size_t i = 0; i < kN; ++i) {
    d.in[i] = static_cast<T>(1 + (i & 0x3FU));
  }
  return d;
}

/// Generic sample loop. The runner returns the reduced value so the optimizer
/// cannot prove the bench is dead code.
template <class T, class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name, const CyclesPerNanosecond &cal, RunFn run) {
  std::atomic<T> sink{T{0}};
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    sink.store(run(), std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const T value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
  }
  (void)sink.load(std::memory_order_relaxed);
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

inline std::pair<std::size_t, std::size_t> blockRange(std::size_t blockIdx,
                                                      std::size_t blocks) noexcept {
  const std::size_t blockSize = (kN + blocks - 1) / blocks;
  const std::size_t lo = std::min(kN, blockIdx * blockSize);
  const std::size_t hi = std::min(kN, (blockIdx + 1) * blockSize);
  return {lo, hi};
}

/// Per-block plain partial-sum kernel; competitor emulations reuse it.
template <class T> [[nodiscard]] T partialSumPlain(const std::vector<T> &in, std::size_t lo,
                                                   std::size_t hi) noexcept {
  T s{0};
  for (std::size_t i = lo; i < hi; ++i) {
    s += in[i];
  }
  return s;
}

/// Per-block Kahan-compensated partial-sum kernel.
[[nodiscard]] inline double partialSumKahan(const std::vector<double> &in, std::size_t lo,
                                            std::size_t hi) noexcept {
  double s = 0.0;
  double c = 0.0;
  for (std::size_t i = lo; i < hi; ++i) {
    const double y = in[i] - c;
    const double t = s + y;
    c = (t - s) - y;
    s = t;
  }
  return s;
}

// =============================================================================
// citor pool
// =============================================================================

[[nodiscard]] BenchRow measureCitorPlain(std::size_t participants,
                                          const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  return measureLoop<std::int64_t>("citor::ThreadPool::parallelReduce", cal, [&] {
    return pool.parallelReduce<ReduceBenchHints>(
        std::size_t{0}, kN, std::int64_t{0},
        [&d](std::size_t lo, std::size_t hi) { return partialSumPlain(d.in, lo, hi); },
        std::plus<std::int64_t>{});
  });
}

[[nodiscard]] BenchRow measureCitorKahan(std::size_t participants,
                                          const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  ReduceData<double> d = buildData<double>();
  return measureLoop<double>("citor::ThreadPool::parallelReduce_kahan", cal, [&] {
    return pool.parallelReduce<ReduceBenchKahanHints>(
        std::size_t{0}, kN, 0.0,
        [&d](std::size_t lo, std::size_t hi) { return partialSumKahan(d.in, lo, hi); },
        std::plus<double>{});
  });
}

// =============================================================================
// BS::thread_pool -- per-block partials via `submit_blocks`, serial merge.
// =============================================================================

template <class T, class Kernel>
[[nodiscard]] T runBsTwoWave(BS::light_thread_pool &pool, const std::vector<T> &in,
                              std::size_t blocks, Kernel kernel) {
  std::vector<T> partials(blocks, T{0});
  const std::size_t blockSize = (kN + blocks - 1) / blocks;
  pool.submit_blocks(
          std::size_t{0}, kN,
          [&in, &partials, blockSize, kernel](std::size_t lo, std::size_t hi) {
            const std::size_t blockIdx = lo / blockSize;
            partials[blockIdx] = kernel(in, lo, hi);
          },
          blocks)
      .wait();
  T total{0};
  for (T v : partials) {
    total += v;
  }
  return total;
}

[[nodiscard]] BenchRow measureBsPlain(std::size_t participants, const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  return measureLoop<std::int64_t>("BS::thread_pool::reduce_two_wave", cal, [&] {
    return runBsTwoWave<std::int64_t>(pool, d.in, participants, partialSumPlain<std::int64_t>);
  });
}

[[nodiscard]] BenchRow measureBsKahan(std::size_t participants, const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  ReduceData<double> d = buildData<double>();
  return measureLoop<double>("BS::thread_pool::reduce_kahan_two_wave", cal, [&] {
    return runBsTwoWave<double>(pool, d.in, participants, partialSumKahan);
  });
}

// =============================================================================
// dp / task / riften -- futures-based two-wave
// =============================================================================

template <class T, class Pool, class EnqueueFn, class Kernel>
[[nodiscard]] T runFutureTwoWave(Pool &pool, const std::vector<T> &in, std::size_t blocks,
                                 EnqueueFn enqueue, Kernel kernel) {
  std::vector<std::future<T>> futs;
  futs.reserve(blocks);
  for (std::size_t b = 0; b < blocks; ++b) {
    auto [lo, hi] = blockRange(b, blocks);
    if (lo == hi) {
      continue;
    }
    futs.emplace_back(enqueue(pool, [&in, lo, hi, kernel] { return kernel(in, lo, hi); }));
  }
  T total{0};
  for (auto &f : futs) {
    total += f.get();
  }
  return total;
}

[[nodiscard]] BenchRow measureDpPlain(std::size_t participants, const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  return measureLoop<std::int64_t>("dp::thread_pool::reduce_two_wave", cal, [&] {
    return runFutureTwoWave<std::int64_t>(
        pool, d.in, participants,
        [](dp::thread_pool<> &p, auto fn) { return p.enqueue(std::move(fn)); },
        partialSumPlain<std::int64_t>);
  });
}

[[nodiscard]] BenchRow measureDpKahan(std::size_t participants, const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  ReduceData<double> d = buildData<double>();
  return measureLoop<double>("dp::thread_pool::reduce_kahan_two_wave", cal, [&] {
    return runFutureTwoWave<double>(
        pool, d.in, participants,
        [](dp::thread_pool<> &p, auto fn) { return p.enqueue(std::move(fn)); }, partialSumKahan);
  });
}

[[nodiscard]] BenchRow measureTaskPlain(std::size_t participants, const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(static_cast<unsigned int>(participants));
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  return measureLoop<std::int64_t>("task_thread_pool::reduce_two_wave", cal, [&] {
    return runFutureTwoWave<std::int64_t>(
        pool, d.in, participants,
        [](::task_thread_pool::task_thread_pool &p, auto fn) { return p.submit(std::move(fn)); },
        partialSumPlain<std::int64_t>);
  });
}

[[nodiscard]] BenchRow measureTaskKahan(std::size_t participants, const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(static_cast<unsigned int>(participants));
  ReduceData<double> d = buildData<double>();
  return measureLoop<double>("task_thread_pool::reduce_kahan_two_wave", cal, [&] {
    return runFutureTwoWave<double>(
        pool, d.in, participants,
        [](::task_thread_pool::task_thread_pool &p, auto fn) { return p.submit(std::move(fn)); },
        partialSumKahan);
  });
}

[[nodiscard]] BenchRow measureRiftenPlain(std::size_t participants,
                                            const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  return measureLoop<std::int64_t>("riften::Thiefpool::reduce_two_wave", cal, [&] {
    return runFutureTwoWave<std::int64_t>(
        pool, d.in, participants,
        [](riften::Thiefpool &p, auto fn) { return p.enqueue(std::move(fn)); },
        partialSumPlain<std::int64_t>);
  });
}

[[nodiscard]] BenchRow measureRiftenKahan(std::size_t participants,
                                            const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  ReduceData<double> d = buildData<double>();
  return measureLoop<double>("riften::Thiefpool::reduce_kahan_two_wave", cal, [&] {
    return runFutureTwoWave<double>(
        pool, d.in, participants,
        [](riften::Thiefpool &p, auto fn) { return p.enqueue(std::move(fn)); }, partialSumKahan);
  });
}

// =============================================================================
