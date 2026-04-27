// Cross-pool byte-identity workload for the comparative pool bench.
//
// Drives every pool over a fixed-seed input running parallelReduce on int64,
// and asserts that the result is byte-equal to the citor pool's result over
// `kRuns` repetitions. Reports per-pool the median run time (ns/op column)
// and the divergence rate (errPercent column repurposed as a percentage of
// runs that did NOT match citor; 0.0% means all runs matched).
//
// The harness's `BenchTable` is a perf-shape table; differential testing is
// pass/fail. The encoding above keeps the workload renderable without
// extending the format and surfaces failures directly: a row with errPercent
// > 0 indicates the pool produced a different bit pattern from citor on at
// least one of the `kRuns` repetitions.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
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

struct DifferentialReduceHints {
  static constexpr citor::Balance balance = citor::Balance::StaticUniform;
  static constexpr citor::Determinism determinism = citor::Determinism::FixedBlockOrder;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  static constexpr citor::Partition partition = citor::Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
};

namespace citor::bench {
namespace {

constexpr std::size_t kN = 1'000'000;
constexpr std::size_t kRuns = 100;
constexpr std::size_t kWarmupRuns = 10;

struct Samples {
  std::vector<double> times;
  std::vector<std::uint64_t> bits;
};

[[nodiscard]] std::vector<std::int64_t> buildInputInt64() {
  std::vector<std::int64_t> in(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    in[i] = static_cast<std::int64_t>(1 + (i & 0x3FU));
  }
  return in;
}

[[nodiscard]] inline std::int64_t partialSum(const std::vector<std::int64_t> &in, std::size_t lo,
                                             std::size_t hi) noexcept {
  std::int64_t s = 0;
  for (std::size_t i = lo; i < hi; ++i) {
    s += in[i];
  }
  return s;
}

inline std::pair<std::size_t, std::size_t> blockRange(std::size_t blockIdx,
                                                      std::size_t blocks) noexcept {
  const std::size_t blockSize = (kN + blocks - 1) / blocks;
  const std::size_t lo = std::min(kN, blockIdx * blockSize);
  const std::size_t hi = std::min(kN, (blockIdx + 1) * blockSize);
  return {lo, hi};
}

template <class RunFn> [[nodiscard]] Samples runSamples(const CyclesPerNanosecond &cal, RunFn run) {
  Samples out;
  out.times.reserve(kRuns);
  out.bits.reserve(kRuns);
  for (std::size_t i = 0; i < kWarmupRuns; ++i) {
    (void)run();
  }
  for (std::size_t i = 0; i < kRuns; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    out.times.push_back(cyclesToNs(endCycles - startCycles, cal));
    out.bits.push_back(std::bit_cast<std::uint64_t>(value));
  }
  return out;
}

[[nodiscard]] BenchRow makeRow(const char *name, const Samples &s,
                               const std::vector<std::uint64_t> &refBits) {
  std::vector<double> sorted = s.times;
  std::sort(sorted.begin(), sorted.end());
  const double medianNs = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];
  const double opsPerSec = medianNs > 0.0 ? 1.0e9 / medianNs : 0.0;
  std::size_t mismatches = 0;
  const std::size_t pairs = std::min(s.bits.size(), refBits.size());
  for (std::size_t i = 0; i < pairs; ++i) {
    if (s.bits[i] != refBits[i]) {
      ++mismatches;
    }
  }
  const double divergencePct =
      pairs == 0 ? 0.0 : 100.0 * static_cast<double>(mismatches) / static_cast<double>(pairs);
  return BenchRow{
      .name = name,
      .nsPerOp = medianNs,
      .opsPerSec = opsPerSec,
      .errPercent = divergencePct,
      .tailNs = std::nullopt,
  };
}

[[nodiscard]] std::int64_t runBsReduce(BS::light_thread_pool &pool,
                                       const std::vector<std::int64_t> &in, std::size_t blocks) {
  std::vector<std::int64_t> partials(blocks, 0);
  const std::size_t blockSize = (kN + blocks - 1) / blocks;
  pool.submit_blocks(
          std::size_t{0}, kN,
          [&in, &partials, blockSize](std::size_t lo, std::size_t hi) {
            const std::size_t blockIdx = lo / blockSize;
            partials[blockIdx] = partialSum(in, lo, hi);
          },
          blocks)
      .wait();
  std::int64_t total = 0;
  for (const std::int64_t v : partials) {
    total += v;
  }
  return total;
}

template <class Pool, class EnqueueFn>
[[nodiscard]] std::int64_t runFutureReduce(Pool &pool, const std::vector<std::int64_t> &in,
                                           std::size_t blocks, EnqueueFn enqueue) {
  std::vector<std::future<std::int64_t>> futs;
  futs.reserve(blocks);
  for (std::size_t b = 0; b < blocks; ++b) {
    auto [lo, hi] = blockRange(b, blocks);
    if (lo == hi) {
      continue;
    }
    futs.emplace_back(enqueue(pool, [&in, lo, hi] { return partialSum(in, lo, hi); }));
  }
  std::int64_t total = 0;
  for (auto &f : futs) {
    total += f.get();
  }
  return total;
}

[[nodiscard]] Samples sampleCitor(std::size_t participants, const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  const auto in = buildInputInt64();
  return runSamples(cal, [&] {
    return pool.parallelReduce<DifferentialReduceHints>(
        std::size_t{0}, kN, std::int64_t{0},
        [&in](std::size_t lo, std::size_t hi) { return partialSum(in, lo, hi); },
        std::plus<std::int64_t>{});
  });
}

[[nodiscard]] Samples sampleBs(std::size_t participants, const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  const auto in = buildInputInt64();
  return runSamples(cal, [&] { return runBsReduce(pool, in, participants); });
}

[[nodiscard]] Samples sampleDp(std::size_t participants, const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  const auto in = buildInputInt64();
  return runSamples(cal, [&] {
    return runFutureReduce(pool, in, participants,
                           [](dp::thread_pool<> &p, auto fn) { return p.enqueue(std::move(fn)); });
  });
}

[[nodiscard]] Samples sampleTask(std::size_t participants, const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(static_cast<unsigned int>(participants));
  const auto in = buildInputInt64();
  return runSamples(cal, [&] {
    return runFutureReduce(
        pool, in, participants,
        [](::task_thread_pool::task_thread_pool &p, auto fn) { return p.submit(std::move(fn)); });
  });
}

[[nodiscard]] Samples sampleRiften(std::size_t participants, const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  const auto in = buildInputInt64();
  return runSamples(cal, [&] {
    return runFutureReduce(pool, in, participants,
                           [](riften::Thiefpool &p, auto fn) { return p.enqueue(std::move(fn)); });
  });
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] Samples sampleTbb(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  const auto in = buildInputInt64();
  return runSamples(cal, [&] {
    return CompetitorTraits<::tbb::task_arena>::parallelReduce<std::int64_t>(
        *arena, std::size_t{0}, kN, std::int64_t{0},
        [&in](std::size_t lo, std::size_t hi, std::int64_t local) {
          return local + partialSum(in, lo, hi);
        },
        std::plus<std::int64_t>{});
  });
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] Samples sampleTaskflow(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  const auto in = buildInputInt64();
  return runSamples(cal, [&] {
    std::vector<std::int64_t> partials(participants, 0);
    ::tf::Taskflow flow;
    flow.for_each_index(std::size_t{0}, participants, std::size_t{1},
                        [&in, &partials, participants](std::size_t blockIdx) {
                          const std::size_t blockSize = (kN + participants - 1) / participants;
                          const std::size_t lo = std::min(kN, blockIdx * blockSize);
                          const std::size_t hi = std::min(kN, (blockIdx + 1) * blockSize);
                          if (lo < hi) {
                            partials[blockIdx] = partialSum(in, lo, hi);
                          }
                        });
    exec->run(flow).wait();
    std::int64_t total = 0;
    for (const std::int64_t v : partials) {
      total += v;
    }
    return total;
  });
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] Samples sampleEigen(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  const auto in = buildInputInt64();
  return runSamples(cal, [&] {
    std::vector<std::int64_t> partials(participants, 0);
    const std::size_t blockSize = (kN + participants - 1) / participants;
    CompetitorTraits<::Eigen::ThreadPool>::parallelFor(
        *pool, std::size_t{0}, kN, participants,
        [&in, &partials, blockSize](std::size_t lo, std::size_t hi) {
          const std::size_t blockIdx = lo / blockSize;
          partials[blockIdx] = partialSum(in, lo, hi);
        });
    std::int64_t total = 0;
    for (const std::int64_t v : partials) {
      total += v;
    }
    return total;
  });
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] Samples sampleOpenMp(std::size_t participants, const CyclesPerNanosecond &cal) {
  const auto in = buildInputInt64();
  const auto threads = static_cast<int>(participants);
  return runSamples(cal, [&] {
    std::int64_t total = 0;
    const auto n = static_cast<std::ptrdiff_t>(kN);
#pragma omp parallel for num_threads(threads) reduction(+ : total) schedule(static)
    for (std::ptrdiff_t i = 0; i < n; ++i) {
      total += in[static_cast<std::size_t>(i)];
    }
    return total;
  });
}
#endif

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"differential_reduce_int64_"} + suffix;

  const auto citor = sampleCitor(participants, cal);
  const auto &refBits = citor.bits;

  table.rows.push_back(makeRow("citor::ThreadPool", citor, refBits));
  table.rows.push_back(makeRow("BS::thread_pool", sampleBs(participants, cal), refBits));
  table.rows.push_back(makeRow("dp::thread_pool", sampleDp(participants, cal), refBits));
  table.rows.push_back(makeRow("task_thread_pool", sampleTask(participants, cal), refBits));
  table.rows.push_back(makeRow("riften::Thiefpool", sampleRiften(participants, cal), refBits));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(makeRow("oneTBB", sampleTbb(participants, cal), refBits));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(makeRow("Taskflow", sampleTaskflow(participants, cal), refBits));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(makeRow("Eigen::ThreadPool", sampleEigen(participants, cal), refBits));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(makeRow("OpenMP", sampleOpenMp(participants, cal), refBits));
#endif
  return table;
}

BenchTable runDifferentialJ8(const CyclesPerNanosecond &cal) { return buildTable(8, "j8", cal); }
BenchTable runDifferentialJ16(const CyclesPerNanosecond &cal) { return buildTable(16, "j16", cal); }

struct DifferentialRegistrar {
  DifferentialRegistrar() {
    registerWorkload({.name = "differential_reduce_int64_j8", .run = &runDifferentialJ8});
    registerWorkload({.name = "differential_reduce_int64_j16", .run = &runDifferentialJ16});
  }
};

const DifferentialRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
