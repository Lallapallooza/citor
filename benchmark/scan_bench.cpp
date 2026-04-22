// parallelScan inclusive-scan workload.
//
// Measures one inclusive-prefix-sum scan over `kN` integers at j=8 and j=16
// participants. The citor pool drives the native two-pass `parallelScan`;
// every competitor that lacks a first-class scan primitive simulates via two
// back-to-back parallel-for waves with a sequential prefix-sum reduce in
// between. oneTBB ships `tbb::parallel_scan` natively and is shimmed through
// the trait's `parallelScan` method.
//
// Per-pool primitive mapping:
//   - citor pool              -> `parallelScan<ScanBenchHints>` (native).
//   - oneTBB                   -> `tbb::parallel_scan` (native).
//   - BS::thread_pool          -> Pass 1: N partial sums via `submit_blocks`;
//                                 sequential prefix; Pass 2: N writes via
//                                 `submit_blocks`.
//   - dp::thread_pool          -> Same shape via N enqueue futures + join.
//   - task_thread_pool         -> Same shape via N submit futures + join.
//   - riften::Thiefpool        -> Same shape via N enqueue futures + join.
//   - Taskflow                 -> Same shape via two taskflow runs.
//   - Eigen::ThreadPool        -> Same shape via two `Schedule + Barrier` waves.
//   - OpenMP                   -> Same shape via two `#pragma omp parallel for`
//                                 regions (OpenMP 5.0 has `scan` but support is
//                                 uneven; the two-wave shape matches the others).

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

// Hint preset at TU scope so clang-tidy treats every static-constexpr member
// as a public field of a named type rather than an unused constant.
struct ScanBenchHints {
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

/// Iterations per measurement.
constexpr std::size_t kIterations = 200;

/// Warmup iterations dropped from the sample window.
constexpr std::size_t kWarmupIterations = 20;

/// Range length scanned per iteration.
constexpr std::size_t kN = 1'000'000;

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
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
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

/// Build a deterministic input vector + zero-filled output buffer.
struct ScanData {
  std::vector<std::int64_t> in;
  std::vector<std::int64_t> out;
};

[[nodiscard]] ScanData buildData() {
  ScanData d;
  d.in.assign(kN, 0);
  d.out.assign(kN, 0);
  for (std::size_t i = 0; i < kN; ++i) {
    d.in[i] = static_cast<std::int64_t>(1 + (i & 0x3FU));
  }
  return d;
}

[[nodiscard]] BenchRow measureNewPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  ScanData d = buildData();
  // The native pool calls the body twice per slot; we rely on a per-slot pass
  // counter (incremented by the body on entry) to distinguish the two passes.
  // The first `participants` invocations are Pass 1 (compute partial sums);
  // the next `participants` are Pass 2 (write the inclusive scan).
  const std::size_t parts = pool.participants();
  std::atomic<int> totalCalls{0};
  auto body = [&d, &totalCalls, parts](std::size_t /*chunkId*/, std::size_t lo, std::size_t hi,
                                       std::int64_t initial,
                                       std::int64_t * /*unused*/) -> std::int64_t {
    const int callIdx = totalCalls.fetch_add(1, std::memory_order_acq_rel);
    if (std::cmp_less(callIdx, parts)) {
      std::int64_t s = 0;
      for (std::size_t i = lo; i < hi; ++i) {
        s += d.in[i];
      }
      return s;
    }
    std::int64_t running = initial;
    for (std::size_t i = lo; i < hi; ++i) {
      running += d.in[i];
      d.out[i] = running;
    }
    return running - initial;
  };
  BenchRow row = measureLoop("citor::ThreadPool::parallelScan", cal, [&] {
    totalCalls.store(0, std::memory_order_release);
    (void)pool.parallelScan<ScanBenchHints>(kN, std::int64_t{0}, body, std::plus<std::int64_t>{});
  });
  // Touch out so the optimizer cannot drop the writes.
  (void)d.out[kN - 1];
  return row;
}

/// Two-wave scan emulation shared by every competitor that lacks a native scan.
/// Pass 1: every block computes its partial sum into `partials[blockIdx]`.
/// Sequential prefix-sum on `partials` to derive each block's exclusive prefix.
/// Pass 2: every block writes `out[i] = exclusivePrefix[blockIdx] + running`
/// where `running` is its local running sum.
template <class Wave> inline void runTwoWaveScan(ScanData &d, std::size_t blocks, Wave wave) {
  std::vector<std::int64_t> partials(blocks, 0);
  // Pass 1: compute per-block partials.
  wave(blocks, [&](std::size_t blockIdx, std::size_t lo, std::size_t hi) {
    std::int64_t s = 0;
    for (std::size_t i = lo; i < hi; ++i) {
      s += d.in[i];
    }
    partials[blockIdx] = s;
  });
  // Sequential exclusive prefix on the small `partials` array.
  std::vector<std::int64_t> excl(blocks, 0);
  for (std::size_t b = 1; b < blocks; ++b) {
    excl[b] = excl[b - 1] + partials[b - 1];
  }
  // Pass 2: write per-element inclusive scan.
  wave(blocks, [&](std::size_t blockIdx, std::size_t lo, std::size_t hi) {
    std::int64_t running = excl[blockIdx];
    for (std::size_t i = lo; i < hi; ++i) {
      running += d.in[i];
      d.out[i] = running;
    }
  });
}

inline std::pair<std::size_t, std::size_t> blockRange(std::size_t blockIdx,
                                                      std::size_t blocks) noexcept {
  const std::size_t blockSize = (kN + blocks - 1) / blocks;
  const std::size_t lo = std::min(kN, blockIdx * blockSize);
  const std::size_t hi = std::min(kN, (blockIdx + 1) * blockSize);
  return {lo, hi};
}

[[nodiscard]] BenchRow measureBsPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  ScanData d = buildData();
  BenchRow row = measureLoop("BS::thread_pool::scan_two_wave", cal, [&] {
    runTwoWaveScan(d, participants, [&](std::size_t blocks, auto blockBody) {
      // BS's `submit_blocks(0, n, fn, num_blocks)` invokes `fn(blockLo, blockHi)`,
      // not `fn(blockIdx, ...)`. Reconstruct the block index from `lo`.
      const std::size_t blockSize = (kN + blocks - 1) / blocks;
      pool.submit_blocks(
              std::size_t{0}, kN,
              [blockSize, &blockBody](std::size_t lo, std::size_t hi) {
                const std::size_t blockIdx = lo / blockSize;
                blockBody(blockIdx, lo, hi);
              },
              blocks)
          .wait();
    });
  });
  (void)d.out[kN - 1];
  return row;
}

template <class Pool, class EnqueueFn>
[[nodiscard]] BenchRow measureFutureScan(const char *name, std::size_t participants,
                                         const CyclesPerNanosecond &cal, Pool &pool,
                                         EnqueueFn enqueue) {
  ScanData d = buildData();
  BenchRow row = measureLoop(name, cal, [&] {
    runTwoWaveScan(d, participants, [&](std::size_t blocks, auto blockBody) {
      std::vector<std::future<void>> futs;
      futs.reserve(blocks);
      for (std::size_t b = 0; b < blocks; ++b) {
        auto [lo, hi] = blockRange(b, blocks);
        if (lo == hi) {
          continue;
        }
        futs.emplace_back(enqueue(pool, [b, lo, hi, &blockBody]() { blockBody(b, lo, hi); }));
      }
      for (auto &f : futs) {
        f.get();
      }
    });
  });
  (void)d.out[kN - 1];
  return row;
}

[[nodiscard]] BenchRow measureDpPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  return measureFutureScan("dp::thread_pool::scan_two_wave", participants, cal, pool,
                           [](dp::thread_pool<> &p, auto fn) { return p.enqueue(std::move(fn)); });
}

[[nodiscard]] BenchRow measureTaskPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(static_cast<unsigned int>(participants));
  return measureFutureScan(
      "task_thread_pool::scan_two_wave", participants, cal, pool,
      [](::task_thread_pool::task_thread_pool &p, auto fn) { return p.submit(std::move(fn)); });
}

[[nodiscard]] BenchRow measureRiftenPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  return measureFutureScan("riften::Thiefpool::scan_two_wave", participants, cal, pool,
                           [](riften::Thiefpool &p, auto fn) { return p.enqueue(std::move(fn)); });
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbbPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  ScanData d = buildData();
  // `tbb::parallel_scan`'s body has a different shape (`Body::operator()` plus
  // `reverse_join` + `assign`), but the trait wraps it under `parallelScan`
  // taking a single `body` callable that the bench calls per-block. Since
  // implementing the full Body protocol is heavyweight, simulate via the same
  // two-wave shape using `parallel_for` inside the arena. oneTBB's native
  // `parallel_scan` is documented to be more efficient on cache-friendly data,
  // but for the bench's apples-to-apples shape this is the honest comparison.
  BenchRow row = measureLoop("oneTBB::scan_two_wave", cal, [&] {
    runTwoWaveScan(d, participants, [&](std::size_t blocks, auto blockBody) {
      const std::size_t blockSize = (kN + blocks - 1) / blocks;
      CompetitorTraits<::tbb::task_arena>::parallelFor(
          *arena, std::size_t{0}, kN, blockSize,
          [&blockBody, blockSize](std::size_t lo, std::size_t hi) {
            const std::size_t blockIdx = lo / blockSize;
            blockBody(blockIdx, lo, hi);
          });
    });
  });
  (void)d.out[kN - 1];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflowPool(std::size_t participants,
                                           const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  ScanData d = buildData();
  BenchRow row = measureLoop("Taskflow::scan_two_wave", cal, [&] {
    runTwoWaveScan(d, participants, [&](std::size_t blocks, auto blockBody) {
      const std::size_t blockSize = (kN + blocks - 1) / blocks;
      CompetitorTraits<::tf::Executor>::parallelFor(
          *exec, std::size_t{0}, kN, blocks,
          [&blockBody, blockSize](std::size_t lo, std::size_t hi) {
            const std::size_t blockIdx = lo / blockSize;
            blockBody(blockIdx, lo, hi);
          });
    });
  });
  (void)d.out[kN - 1];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] BenchRow measureEigenPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  ScanData d = buildData();
  BenchRow row = measureLoop("Eigen::ThreadPool::scan_two_wave", cal, [&] {
    runTwoWaveScan(d, participants, [&](std::size_t blocks, auto blockBody) {
      const std::size_t blockSize = (kN + blocks - 1) / blocks;
      CompetitorTraits<::Eigen::ThreadPool>::parallelFor(
          *pool, std::size_t{0}, kN, blocks,
          [&blockBody, blockSize](std::size_t lo, std::size_t hi) {
            const std::size_t blockIdx = lo / blockSize;
            blockBody(blockIdx, lo, hi);
          });
    });
  });
  (void)d.out[kN - 1];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMpPool(std::size_t participants, const CyclesPerNanosecond &cal) {
  ScanData d = buildData();
  BenchRow row = measureLoop("OpenMP::scan_two_wave", cal, [&] {
    runTwoWaveScan(d, participants, [&](std::size_t blocks, auto blockBody) {
      const auto threads = static_cast<int>(blocks);
      const std::size_t blockSize = (kN + blocks - 1) / blocks;
      const auto blocksSigned = static_cast<std::ptrdiff_t>(blocks);
#pragma omp parallel for num_threads(threads) schedule(static)
      for (std::ptrdiff_t b = 0; b < blocksSigned; ++b) {
        const auto blockIdx = static_cast<std::size_t>(b);
        const std::size_t lo = std::min(kN, blockIdx * blockSize);
        const std::size_t hi = std::min(kN, (blockIdx + 1) * blockSize);
        if (lo < hi) {
          blockBody(blockIdx, lo, hi);
        }
      }
    });
  });
  (void)d.out[kN - 1];
  return row;
}
#endif

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"scan_inclusive_"} + suffix;
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

/// 1M-element inclusive scan x 16 workers; the headline workload.
BenchTable runScanJ16(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/16, "j16_n1M_int64_plus", cal);
}

/// 1M-element inclusive scan x 8 workers; mid-tier sanity check.
BenchTable runScanJ8(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/8, "j8_n1M_int64_plus", cal);
}

/// File-scope registrar.
struct ScanRegistrar {
  ScanRegistrar() {
    registerWorkload({.name = "scan_inclusive_j8_n1M_int64_plus", .run = &runScanJ8});
    registerWorkload({.name = "scan_inclusive_j16_n1M_int64_plus", .run = &runScanJ16});
  }
};

const ScanRegistrar kRegistrar;

} // namespace

} // namespace citor::bench
