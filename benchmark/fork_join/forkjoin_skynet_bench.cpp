// Skynet 1M -- 10-way recursive fanout, depth 6, sum 0..(10^6 - 1).
//
// Cengiz / ATS Microbenchmark (2016) headline workload for fan-out-heavy
// task schedulers. Each non-leaf node spawns 10 children labeled
// `parent_label * 10 + i`. Leaves return their numeric label; non-leaves
// sum their children. Total leaves = 10^6 = 1M. Verification:
// sum = N * (N - 1) / 2 = 499999500000 for N = 10^6.
//
// This is tzcnt/runtime-benchmarks's headline cell and the workload that
// libfork's published numbers cite for "10-way fan-out wins". Citor's
// just-landed `forkJoinAll(10, ...)` (commit 40a610f) is the runtime-N
// path that this bench drives.
//
// Pool eligibility: every recursive-spawn-capable pool. The fan-out is
// regular (10 every level), so unlike Strassen / matmul DAC we don't
// need two phases per level -- a single `recursiveSpawnN(pool, 10)`
// dispatches the children and sums on join. That makes Subflow OK here
// (one spawn-and-join per recursion level).

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "citor/always_assert.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"
#include "libfork_runners.h"
#include "recursive_forkjoin_helper.h"
#include "tmc_runners.h"

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

namespace citor::bench {
namespace {

constexpr int kFanout = 10;
constexpr int kDepth = 6;
constexpr std::int64_t kLeafCount = 1'000'000; // 10^6
constexpr std::int64_t kExpectedSum = (kLeafCount * (kLeafCount - 1)) / 2;

constexpr std::size_t kIterations = 25;
constexpr std::size_t kWarmupIterations = 3;

template <class Pool>
std::int64_t skynetRec(Pool &pool, std::int64_t label, int depth) {
  if (depth == 0) {
    return label;
  }
  const std::int64_t base = label * kFanout;
  std::array<std::int64_t, kFanout> partials{};
  recursiveSpawnN(
      pool, static_cast<std::size_t>(kFanout), [&](Pool &p, std::size_t i) {
        partials[i] =
            skynetRec(p, base + static_cast<std::int64_t>(i), depth - 1);
      });
  std::int64_t total = 0;
  for (const std::int64_t v : partials) {
    total += v;
  }
  return total;
}

template <class PoolT>
[[nodiscard]] BenchRow measureSkynet(const char *name, std::size_t participants,
                                     const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<PoolT>::supportsRecursiveSpawn,
                "skynet bench requires recursive-spawn-capable pool");
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  std::int64_t result = 0;
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    result = skynetRec(*pool, std::int64_t{0}, kDepth);
  }
  BENCH_CHECK_OR_THROW(result == kExpectedSum, "forkjoin_skynet_bench.cpp");

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    result = skynetRec(*pool, std::int64_t{0}, kDepth);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    BENCH_CHECK_OR_THROW(result == kExpectedSum, "forkjoin_skynet_bench.cpp");
  }
  return finalizeRow(name, samples);
}

[[nodiscard]] BenchRow measureCitor(std::size_t participants,
                                    const CyclesPerNanosecond &cal) {
  return measureSkynet<citor::ThreadPool>("citor::ThreadPool", participants,
                                          cal);
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbb(std::size_t participants,
                                  const CyclesPerNanosecond &cal) {
  return measureSkynet<::tbb::task_arena>("oneTBB", participants, cal);
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispenso(std::size_t participants,
                                       const CyclesPerNanosecond &cal) {
  static_assert(
      RecursiveForkJoinTraits<::dispenso::ThreadPool>::supportsRecursiveSpawn,
      "dispenso must opt into recursive spawn for skynet");
  return measureSkynet<::dispenso::ThreadPool>("dispenso::ThreadPool",
                                               participants, cal);
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOmp(std::size_t participants,
                                  const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<OpenMpRunner>::supportsRecursiveSpawn,
                "OpenMP runner must opt into recursive spawn for skynet");
  OpenMpRunner runner{participants};

  auto runOnce = [&]() -> std::int64_t {
    std::int64_t result = 0;
#pragma omp parallel num_threads(static_cast<int>(participants))
    {
#pragma omp single
      {
        result = skynetRec(runner, std::int64_t{0}, kDepth);
      }
    }
    return result;
  };

  std::int64_t result = 0;
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    result = runOnce();
  }
  BENCH_CHECK_OR_THROW(result == kExpectedSum, "forkjoin_skynet_bench.cpp");

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    result = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    BENCH_CHECK_OR_THROW(result == kExpectedSum, "forkjoin_skynet_bench.cpp");
  }
  return finalizeRow("OpenMP", samples);
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflow(std::size_t participants,
                                       const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<::tf::Subflow>::supportsRecursiveSpawn,
                "Taskflow Subflow must opt into recursive spawn for skynet");
  ::tf::Executor exec(participants);

  auto runOnce = [&]() -> std::int64_t {
    std::int64_t result = 0;
    ::tf::Taskflow flow;
    flow.emplace([&result](::tf::Subflow &rootSub) {
      result = skynetRec(rootSub, std::int64_t{0}, kDepth);
    });
    exec.run(flow).wait();
    return result;
  };

  std::int64_t result = 0;
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    result = runOnce();
  }
  BENCH_CHECK_OR_THROW(result == kExpectedSum, "forkjoin_skynet_bench.cpp");

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    result = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    BENCH_CHECK_OR_THROW(result == kExpectedSum, "forkjoin_skynet_bench.cpp");
  }
  return finalizeRow("Taskflow::Subflow", samples);
}
#endif

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_skynet_1m_"} + suffix;
  table.rows.push_back(measureCitor(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOmp(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispenso(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureTaskflow(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LIBFORK
  table.rows.push_back(runLibforkSkynet(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(runTmcSkynet(participants, cal));
#endif
  return table;
}

template <std::size_t JParticipants>
BenchTable runCell(const CyclesPerNanosecond &cal) {
  static_assert(JParticipants == 8 || JParticipants == 16 ||
                    JParticipants == 32 || JParticipants == 48,
                "unsupported j-value");
  constexpr const char *jSuffix = []() -> const char * {
    if constexpr (JParticipants == 8) {
      return "j8";
    } else if constexpr (JParticipants == 16) {
      return "j16";
    } else if constexpr (JParticipants == 32) {
      return "j32";
    } else {
      return "j48";
    }
  }();
  if (!hasEnoughPhysicalCores(JParticipants)) {
    throw std::runtime_error("needs " + std::to_string(JParticipants) +
                             " physical cores");
  }
  return buildTable(JParticipants, jSuffix, cal);
}

struct SkynetRegistrar {
  SkynetRegistrar() {
    registerWorkload({.name = "forkjoin_skynet_1m_j8", .run = &runCell<8>});
    registerWorkload({.name = "forkjoin_skynet_1m_j16", .run = &runCell<16>});
    registerWorkload({.name = "forkjoin_skynet_1m_j32", .run = &runCell<32>});
    registerWorkload({.name = "forkjoin_skynet_1m_j48", .run = &runCell<48>});
  }
};

const SkynetRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
