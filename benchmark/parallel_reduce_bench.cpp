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
  return finalizeRow(name, samples);
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
// oneTBB -- native parallel_reduce
// =============================================================================

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbbPlain(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  return measureLoop<std::int64_t>("oneTBB::parallel_reduce", cal, [&] {
    return CompetitorTraits<::tbb::task_arena>::parallelReduce<std::int64_t>(
        *arena, std::size_t{0}, kN, std::int64_t{0},
        [&d](std::size_t lo, std::size_t hi, std::int64_t local) {
          return local + partialSumPlain(d.in, lo, hi);
        },
        std::plus<std::int64_t>{});
  });
}

[[nodiscard]] BenchRow measureTbbKahan(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  ReduceData<double> d = buildData<double>();
  return measureLoop<double>("oneTBB::parallel_reduce_kahan", cal, [&] {
    return CompetitorTraits<::tbb::task_arena>::parallelReduce<double>(
        *arena, std::size_t{0}, kN, 0.0,
        [&d](std::size_t lo, std::size_t hi, double local) {
          return local + partialSumKahan(d.in, lo, hi);
        },
        std::plus<double>{});
  });
}
#endif

// =============================================================================
// Taskflow -- for_each_index over per-block partials, serial merge
// =============================================================================

#ifdef CITOR_BENCH_HAS_TASKFLOW
template <class T, class Kernel>
[[nodiscard]] T runTaskflowTwoWave(::tf::Executor &exec, const std::vector<T> &in,
                                    std::size_t blocks, Kernel kernel) {
  std::vector<T> partials(blocks, T{0});
  ::tf::Taskflow flow;
  flow.for_each_index(std::size_t{0}, blocks, std::size_t{1},
                      [&in, &partials, blocks, kernel](std::size_t blockIdx) {
                        const std::size_t blockSize = (kN + blocks - 1) / blocks;
                        const std::size_t lo = std::min(kN, blockIdx * blockSize);
                        const std::size_t hi = std::min(kN, (blockIdx + 1) * blockSize);
                        if (lo < hi) {
                          partials[blockIdx] = kernel(in, lo, hi);
                        }
                      });
  exec.run(flow).wait();
  T total{0};
  for (T v : partials) {
    total += v;
  }
  return total;
}

[[nodiscard]] BenchRow measureTaskflowPlain(std::size_t participants,
                                              const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  return measureLoop<std::int64_t>("Taskflow::reduce_two_wave", cal, [&] {
    return runTaskflowTwoWave<std::int64_t>(*exec, d.in, participants,
                                              partialSumPlain<std::int64_t>);
  });
}

[[nodiscard]] BenchRow measureTaskflowKahan(std::size_t participants,
                                              const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  ReduceData<double> d = buildData<double>();
  return measureLoop<double>("Taskflow::reduce_kahan_two_wave", cal, [&] {
    return runTaskflowTwoWave<double>(*exec, d.in, participants, partialSumKahan);
  });
}
#endif

// =============================================================================
// Eigen::ThreadPool -- per-block partials, Schedule + Barrier
// =============================================================================

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
template <class T, class Kernel>
[[nodiscard]] T runEigenTwoWave(::Eigen::ThreadPool &pool, const std::vector<T> &in,
                                  std::size_t blocks, Kernel kernel) {
  std::vector<T> partials(blocks, T{0});
  const std::size_t blockSize = (kN + blocks - 1) / blocks;
  CompetitorTraits<::Eigen::ThreadPool>::parallelFor(
      pool, std::size_t{0}, kN, blocks,
      [&in, &partials, blockSize, kernel](std::size_t lo, std::size_t hi) {
        const std::size_t blockIdx = lo / blockSize;
        partials[blockIdx] = kernel(in, lo, hi);
      });
  T total{0};
  for (T v : partials) {
    total += v;
  }
  return total;
}

[[nodiscard]] BenchRow measureEigenPlain(std::size_t participants,
                                          const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  return measureLoop<std::int64_t>("Eigen::ThreadPool::reduce_two_wave", cal, [&] {
    return runEigenTwoWave<std::int64_t>(*pool, d.in, participants,
                                          partialSumPlain<std::int64_t>);
  });
}

[[nodiscard]] BenchRow measureEigenKahan(std::size_t participants,
                                          const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  ReduceData<double> d = buildData<double>();
  return measureLoop<double>("Eigen::ThreadPool::reduce_kahan_two_wave", cal, [&] {
    return runEigenTwoWave<double>(*pool, d.in, participants, partialSumKahan);
  });
}
#endif

// =============================================================================
// OpenMP -- parallel for reduction(+:)
// =============================================================================

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMpPlain(std::size_t participants,
                                            const CyclesPerNanosecond &cal) {
  ReduceData<std::int64_t> d = buildData<std::int64_t>();
  const auto threads = static_cast<int>(participants);
  return measureLoop<std::int64_t>("OpenMP::reduce_plus", cal, [&] {
    std::int64_t total = 0;
    const auto n = static_cast<std::ptrdiff_t>(kN);
#pragma omp parallel for num_threads(threads) reduction(+ : total)
    for (std::ptrdiff_t i = 0; i < n; ++i) {
      total += d.in[static_cast<std::size_t>(i)];
    }
    return total;
  });
}

[[nodiscard]] BenchRow measureOpenMpKahan(std::size_t participants,
                                            const CyclesPerNanosecond &cal) {
  // OpenMP's `reduction(+:)` on `double` is not Kahan-compensated; emulate by
  // computing per-thread Kahan sums into a per-thread partials array and merging
  // serially, matching the shape of the other emulations.
  ReduceData<double> d = buildData<double>();
  const auto threads = static_cast<int>(participants);
  return measureLoop<double>("OpenMP::reduce_kahan_two_wave", cal, [&] {
    std::vector<double> partials(static_cast<std::size_t>(threads), 0.0);
    const auto blocks = static_cast<std::ptrdiff_t>(threads);
#pragma omp parallel for num_threads(threads) schedule(static)
    for (std::ptrdiff_t b = 0; b < blocks; ++b) {
      const auto blockIdx = static_cast<std::size_t>(b);
      const std::size_t blockSize = (kN + static_cast<std::size_t>(threads) - 1) /
                                    static_cast<std::size_t>(threads);
      const std::size_t lo = std::min(kN, blockIdx * blockSize);
      const std::size_t hi = std::min(kN, (blockIdx + 1) * blockSize);
      if (lo < hi) {
        partials[blockIdx] = partialSumKahan(d.in, lo, hi);
      }
    }
    double total = 0.0;
    for (const double v : partials) {
      total += v;
    }
    return total;
  });
}
#endif

// =============================================================================
// Table builders + registrar
// =============================================================================

BenchTable buildPlainTable(std::size_t participants, const char *suffix,
                           const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"reduce_plus_int64_"} + suffix;
  table.rows.push_back(measureCitorPlain(participants, cal));
  table.rows.push_back(measureBsPlain(participants, cal));
  table.rows.push_back(measureDpPlain(participants, cal));
  table.rows.push_back(measureTaskPlain(participants, cal));
  table.rows.push_back(measureRiftenPlain(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbbPlain(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureTaskflowPlain(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureEigenPlain(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOpenMpPlain(participants, cal));
#endif
  return table;
}

BenchTable buildKahanTable(std::size_t participants, const char *suffix,
                           const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"reduce_kahan_double_"} + suffix;
  table.rows.push_back(measureCitorKahan(participants, cal));
  table.rows.push_back(measureBsKahan(participants, cal));
  table.rows.push_back(measureDpKahan(participants, cal));
  table.rows.push_back(measureTaskKahan(participants, cal));
  table.rows.push_back(measureRiftenKahan(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbbKahan(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureTaskflowKahan(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureEigenKahan(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOpenMpKahan(participants, cal));
#endif
  return table;
}

BenchTable runReducePlainJ2(const CyclesPerNanosecond &cal) {
  return buildPlainTable(2, "j2_n1M", cal);
}
BenchTable runReducePlainJ8(const CyclesPerNanosecond &cal) {
  return buildPlainTable(8, "j8_n1M", cal);
}
BenchTable runReducePlainJ16(const CyclesPerNanosecond &cal) {
  return buildPlainTable(16, "j16_n1M", cal);
}
BenchTable runReduceKahanJ8(const CyclesPerNanosecond &cal) {
  return buildKahanTable(8, "j8_n1M", cal);
}
BenchTable runReduceKahanJ16(const CyclesPerNanosecond &cal) {
  return buildKahanTable(16, "j16_n1M", cal);
}

struct ReduceRegistrar {
  ReduceRegistrar() {
    registerWorkload({.name = "reduce_plus_int64_j2_n1M", .run = &runReducePlainJ2});
    registerWorkload({.name = "reduce_plus_int64_j8_n1M", .run = &runReducePlainJ8});
    registerWorkload({.name = "reduce_plus_int64_j16_n1M", .run = &runReducePlainJ16});
    registerWorkload({.name = "reduce_kahan_double_j8_n1M", .run = &runReduceKahanJ8});
    registerWorkload({.name = "reduce_kahan_double_j16_n1M", .run = &runReduceKahanJ16});
  }
};

const ReduceRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
