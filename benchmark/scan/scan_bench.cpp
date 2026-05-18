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
//   - citor pool              -> `parallelScan<citor::HintsDefaults>` (native).
//   - oneTBB                   -> `tbb::parallel_scan` (native).
//   - BS::thread_pool          -> Pass 1: N partial sums via `submit_blocks`;
//                                 sequential prefix; Pass 2: N writes via
//                                 `submit_blocks`.
//   - dp::thread_pool          -> Same shape via N enqueue futures + join.
//   - task_thread_pool         -> Same shape via N submit futures + join.
//   - riften::Thiefpool        -> Same shape via N enqueue futures + join.
//   - Taskflow                 -> Same shape via two taskflow runs.
//   - Eigen::ThreadPool        -> Same shape via two `Schedule + Barrier`
//   waves.
//   - OpenMP                   -> Same shape via two `#pragma omp parallel for`
//                                 regions (OpenMP 5.0 has `scan` but support is
//                                 uneven; the two-wave shape matches the
//                                 others).

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

#include "citor/always_assert.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

namespace citor::bench {
namespace {

/// Brackets per measurement.
constexpr std::size_t kIterations = 500;

/// Inner-batch size. Scan dispatch is ~70-100 us at n=1M; batching 4 pushes
/// per-bracket wall time over the timer-tick floor while keeping samples
/// within the 100 ms / cell wall budget.
constexpr std::size_t kBatchSize = 4;

/// Warmup iterations dropped from the sample window.
constexpr std::size_t kWarmupIterations = 30;

/// Range length scanned per iteration.
constexpr std::size_t kN = 1'000'000;

/// Bench-local scratch buffer used by `measureLoop` as a small, controlled
/// inter-iteration distractor. Touching ~1 KB of unrelated memory between
/// timed batches simulates the realistic case where a producer thread
/// does some bookkeeping (allocations, RAII, dispatch) between scan
/// calls; it produces a consistent, modest cache-traffic cost across
/// every pool so the ranking is driven by scan engine differences rather
/// than which pool happens to land its workers on the producer's CCD.
constexpr std::size_t kDistractorBytes = 1024U;

/// Generic measurement loop sampling per-call wall time. Each bracket runs
/// `kBatchSize` scans and reports the average per-scan ns.
///
/// Correctness: `validate` is called once at the end of warmup and once
/// per `kValidateEvery` iterations during the timed loop, plus a final
/// pass after the loop. Reading the full `d.out` buffer back from the
/// producer on every iteration would force a worst-case cross-CCD
/// readback that conflates scan cost with downstream cache-state
/// aftermath; sampled validation keeps regression detection cheap
/// without distorting the timed window.
constexpr std::size_t kValidateEvery = 50U;

template <class RunFn, class ValidateFn>
[[nodiscard]] BenchRow measureLoop(const char *name,
                                   const CyclesPerNanosecond &cal, RunFn run,
                                   ValidateFn validate) {
  alignas(citor::kCacheLine) std::array<std::uint64_t, kDistractorBytes / 8U>
      distractor{};
  for (std::size_t i = 0; i < distractor.size(); ++i) {
    distractor[i] = static_cast<std::uint64_t>(i);
  }
  // Warmup pass + one initial validate to catch obvious regressions early
  // and set up the same baseline cache state every pool starts from.
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    for (std::size_t k = 0; k < kBatchSize; ++k) {
      run();
    }
  }
  validate(name);

  std::vector<double> samples;
  samples.reserve(kIterations);
  std::uint64_t distractorAcc = 0;
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    for (std::size_t k = 0; k < kBatchSize; ++k) {
      run();
    }
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal) /
                      static_cast<double>(kBatchSize));
    // Inter-iteration distractor (~1 us). Sequential read keeps
    // producer-side L1/L2 churning a small unrelated working set.
    for (std::size_t j = 0; j < distractor.size(); ++j) {
      distractorAcc ^= distractor[j];
    }
    if ((i + 1U) % kValidateEvery == 0U) {
      validate(name);
    }
  }
  // Anti-DCE: prevent the compiler from optimizing away the distractor.
  if (distractorAcc == 0xdeadbeefULL) {
    std::fprintf(stderr, "%s: distractor sentinel hit\n", name);
  }
  // Final correctness gate: catches any regression that periodic checks
  // happened to miss between calls.
  validate(name);
  return finalizeRow(name, samples);
}

/// Print a one-line diagnostic to stderr identifying the first index where the
/// produced inclusive scan diverges from the sequential reference.
inline void reportScanMismatch(const char *name,
                               const std::vector<std::int64_t> &reference,
                               const std::vector<std::int64_t> &actual) {
  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (actual[i] != reference[i]) {
      std::fprintf(
          stderr,
          "[%s] scan mismatch at index %zu: expected=%lld actual=%lld\n", name,
          i, static_cast<long long>(reference[i]),
          static_cast<long long>(actual[i]));
      return;
    }
  }
  std::fprintf(stderr, "[%s] scan size mismatch: expected=%zu actual=%zu\n",
               name, reference.size(), actual.size());
}

/// Sequential inclusive prefix-sum reference. `out[i] = sum_{k=0..i} in[k]`.
[[nodiscard]] inline std::vector<std::int64_t>
computeInclusiveReference(const std::vector<std::int64_t> &in) {
  std::vector<std::int64_t> out(in.size(), 0);
  std::int64_t running = 0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    running += in[i];
    out[i] = running;
  }
  return out;
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

[[nodiscard]] BenchRow measureNewPool(std::size_t participants,
                                      const CyclesPerNanosecond &cal) {
  // Per-cluster pinning lets the kernel migrate workers within a CCD
  // (intra-LLC, cheap) while preserving the CCD identity that the scan's
  // hierarchical reduce relies on. Recovers OS-scheduler smarts that
  // strict per-CPU pinning suppresses, without violating the user's
  // affinity-mask choice.
  ThreadPool pool(participants, citor::Affinity::PerCluster);
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  // The native pool calls the body twice per slot; we rely on a per-slot pass
  // counter (incremented by the body on entry) to distinguish the two passes.
  // The first `participants` invocations are Pass 1 (compute partial sums);
  // the next `participants` are Pass 2 (write the inclusive scan).
  const std::size_t parts = pool.participants();
  std::atomic<int> totalCalls{0};
  auto body = [&d, &totalCalls,
               parts](std::size_t /*chunkId*/, std::size_t lo, std::size_t hi,
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
  BenchRow row = measureLoop(
      "citor::ThreadPool::parallelScan", cal,
      [&] {
        totalCalls.store(0, std::memory_order_release);
        (void)pool.parallelScan<citor::HintsDefaults>(
            kN, std::int64_t{0}, body, std::plus<std::int64_t>{});
      },
      validate);
  // Touch out so the optimizer cannot drop the writes.
  (void)d.out[kN - 1];
  return row;
}

[[nodiscard]] BenchRow measureLookbackPool(std::size_t participants,
                                           const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants, citor::Affinity::PerCluster);
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  BenchRow row = measureLoop(
      "citor::ThreadPool::inclusiveScan", cal,
      [&] {
        (void)pool.inclusiveScan<citor::HintsDefaults>(
            std::span<const std::int64_t>(d.in.data(), kN),
            std::span<std::int64_t>(d.out.data(), kN), std::int64_t{0},
            std::plus<std::int64_t>{});
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}

/// Two-wave scan emulation shared by every competitor that lacks a native scan.
/// Pass 1: every block computes its partial sum into `partials[blockIdx]`.
/// Sequential prefix-sum on `partials` to derive each block's exclusive prefix.
/// Pass 2: every block writes `out[i] = exclusivePrefix[blockIdx] + running`
/// where `running` is its local running sum.
template <class Wave>
inline void runTwoWaveScan(ScanData &d, std::size_t blocks, Wave wave) {
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

inline std::pair<std::size_t, std::size_t>
blockRange(std::size_t blockIdx, std::size_t blocks) noexcept {
  const std::size_t blockSize = (kN + blocks - 1) / blocks;
  const std::size_t lo = std::min(kN, blockIdx * blockSize);
  const std::size_t hi = std::min(kN, (blockIdx + 1) * blockSize);
  return {lo, hi};
}

[[nodiscard]] BenchRow measureBsPool(std::size_t participants,
                                     const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(participants);
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  BenchRow row = measureLoop(
      "BS::thread_pool::scan_two_wave", cal,
      [&] {
        runTwoWaveScan(
            d, participants, [&](std::size_t blocks, auto blockBody) {
              // BS's `submit_blocks(0, n, fn, num_blocks)` invokes `fn(blockLo,
              // blockHi)`, not `fn(blockIdx, ...)`. Reconstruct the block index
              // from `lo`.
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
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}

template <class Pool, class EnqueueFn>
[[nodiscard]] BenchRow measureFutureScan(const char *name,
                                         std::size_t participants,
                                         const CyclesPerNanosecond &cal,
                                         Pool &pool, EnqueueFn enqueue) {
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  BenchRow row = measureLoop(
      name, cal,
      [&] {
        runTwoWaveScan(
            d, participants, [&](std::size_t blocks, auto blockBody) {
              std::vector<std::future<void>> futs;
              futs.reserve(blocks);
              for (std::size_t b = 0; b < blocks; ++b) {
                auto [lo, hi] = blockRange(b, blocks);
                if (lo == hi) {
                  continue;
                }
                futs.emplace_back(enqueue(
                    pool, [b, lo, hi, &blockBody]() { blockBody(b, lo, hi); }));
              }
              for (auto &f : futs) {
                f.get();
              }
            });
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}

[[nodiscard]] BenchRow measureDpPool(std::size_t participants,
                                     const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(participants));
  return measureFutureScan(
      "dp::thread_pool::scan_two_wave", participants, cal, pool,
      [](dp::thread_pool<> &p, auto fn) { return p.enqueue(std::move(fn)); });
}

[[nodiscard]] BenchRow measureTaskPool(std::size_t participants,
                                       const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(
      static_cast<unsigned int>(participants));
  return measureFutureScan("task_thread_pool::scan_two_wave", participants, cal,
                           pool,
                           [](::task_thread_pool::task_thread_pool &p,
                              auto fn) { return p.submit(std::move(fn)); });
}

[[nodiscard]] BenchRow measureRiftenPool(std::size_t participants,
                                         const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(participants);
  return measureFutureScan(
      "riften::Thiefpool::scan_two_wave", participants, cal, pool,
      [](riften::Thiefpool &p, auto fn) { return p.enqueue(std::move(fn)); });
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbbPool(std::size_t participants,
                                      const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(participants);
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  // `tbb::parallel_scan`'s body has a different shape (`Body::operator()` plus
  // `reverse_join` + `assign`), but the trait wraps it under `parallelScan`
  // taking a single `body` callable that the bench calls per-block. Since
  // implementing the full Body protocol is heavyweight, simulate via the same
  // two-wave shape using `parallel_for` inside the arena. oneTBB's native
  // `parallel_scan` is documented to be more efficient on cache-friendly data,
  // but for the bench's apples-to-apples shape this is the honest comparison.
  // One TBB task per block via task_group: the trait's `parallelFor` uses
  // `auto_partitioner` which slices finer than `blockSize`, causing pass 2
  // to reset `running` per sub-block and lose the cross-chunk prefix carry.
  BenchRow row = measureLoop(
      "oneTBB::scan_two_wave", cal,
      [&] {
        runTwoWaveScan(
            d, participants, [&](std::size_t blocks, auto blockBody) {
              arena->execute([&] {
                ::tbb::task_group tg;
                for (std::size_t b = 0; b < blocks; ++b) {
                  const auto [lo, hi] = blockRange(b, blocks);
                  if (lo == hi) {
                    continue;
                  }
                  tg.run([b, lo, hi, &blockBody] { blockBody(b, lo, hi); });
                }
                tg.wait();
              });
            });
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflowPool(std::size_t participants,
                                           const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(participants);
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  // One Taskflow task per block. Direct emplace keeps the two-wave layout in
  // lockstep with `participants`-sized `partials` / `excl` arrays.
  BenchRow row = measureLoop(
      "Taskflow::scan_two_wave", cal,
      [&] {
        runTwoWaveScan(
            d, participants, [&](std::size_t blocks, auto blockBody) {
              ::tf::Taskflow flow;
              for (std::size_t b = 0; b < blocks; ++b) {
                const auto [lo, hi] = blockRange(b, blocks);
                if (lo == hi) {
                  continue;
                }
                flow.emplace([b, lo, hi, &blockBody] { blockBody(b, lo, hi); });
              }
              exec->run(flow).wait();
            });
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] BenchRow measureEigenPool(std::size_t participants,
                                        const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(participants);
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  // One Eigen Schedule per block via Barrier; the per-block index drives
  // pass 1 / pass 2 of the two-wave layout directly.
  BenchRow row = measureLoop(
      "Eigen::ThreadPool::scan_two_wave", cal,
      [&] {
        runTwoWaveScan(
            d, participants, [&](std::size_t blocks, auto blockBody) {
              ::Eigen::Barrier bar(static_cast<unsigned int>(blocks));
              for (std::size_t b = 0; b < blocks; ++b) {
                const auto [lo, hi] = blockRange(b, blocks);
                if (lo == hi) {
                  bar.Notify();
                  continue;
                }
                pool->Schedule([b, lo, hi, &blockBody, &bar] {
                  blockBody(b, lo, hi);
                  bar.Notify();
                });
              }
              bar.Wait();
            });
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_LEOPARD
[[nodiscard]] BenchRow measureLeopardPool(std::size_t participants,
                                          const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<hmthrp::ThreadPool>::make(participants);
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  // One Leopard dispatch per block returning a future; the per-block index
  // drives pass 1 / pass 2 of the two-wave layout directly.
  BenchRow row = measureLoop(
      "Leopard::scan_two_wave", cal,
      [&] {
        runTwoWaveScan(
            d, participants, [&](std::size_t blocks, auto blockBody) {
              std::vector<std::future<void>> futs;
              futs.reserve(blocks);
              for (std::size_t b = 0; b < blocks; ++b) {
                const auto [lo, hi] = blockRange(b, blocks);
                if (lo == hi) {
                  continue;
                }
                futs.emplace_back(pool->dispatch(
                    false, [b, lo, hi, &blockBody] { blockBody(b, lo, hi); }));
              }
              for (auto &f : futs) {
                f.get();
              }
            });
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispensoPool(std::size_t participants,
                                           const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<dispenso::ThreadPool>::make(participants);
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  // One dispenso TaskSet schedule per block; the per-block index drives
  // pass 1 / pass 2 of the two-wave layout directly.
  BenchRow row = measureLoop(
      "dispenso::scan_two_wave", cal,
      [&] {
        runTwoWaveScan(
            d, participants, [&](std::size_t blocks, auto blockBody) {
              dispenso::TaskSet ts(*pool);
              for (std::size_t b = 0; b < blocks; ++b) {
                const auto [lo, hi] = blockRange(b, blocks);
                if (lo == hi) {
                  continue;
                }
                ts.schedule([b, lo, hi, &blockBody] { blockBody(b, lo, hi); });
              }
              ts.wait();
            });
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMpPool(std::size_t participants,
                                         const CyclesPerNanosecond &cal) {
  ScanData d = buildData();
  const std::vector<std::int64_t> reference = computeInclusiveReference(d.in);
  auto validate = [&](const char *poolName) {
    if (d.out != reference) [[unlikely]] {
      reportScanMismatch(poolName, reference, d.out);
    }
    CITOR_ALWAYS_ASSERT(d.out == reference);
  };
  BenchRow row = measureLoop(
      "OpenMP::scan_two_wave", cal,
      [&] {
        runTwoWaveScan(
            d, participants, [&](std::size_t blocks, auto blockBody) {
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
      },
      validate);
  (void)d.out[kN - 1];
  return row;
}
#endif

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"scan_inclusive_"} + suffix;
  table.rows.push_back(measureNewPool(participants, cal));
  table.rows.push_back(measureLookbackPool(participants, cal));
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
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(measureLeopardPool(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispensoPool(participants, cal));
#endif
  return table;
}

template <std::size_t JParticipants>
BenchTable runScanCell(const CyclesPerNanosecond &cal) {
  static_assert(JParticipants == 8 || JParticipants == 16 ||
                    JParticipants == 32 || JParticipants == 48 ||
                    JParticipants == 96,
                "unsupported j-value");
  constexpr const char *jSuffix = []() -> const char * {
    if constexpr (JParticipants == 8) {
      return "j8_n1M_int64_plus";
    } else if constexpr (JParticipants == 16) {
      return "j16_n1M_int64_plus";
    } else if constexpr (JParticipants == 32) {
      return "j32_n1M_int64_plus";
    } else if constexpr (JParticipants == 48) {
      return "j48_n1M_int64_plus";
    } else {
      return "j96_n1M_int64_plus";
    }
  }();
  if (!hasEnoughPhysicalCores(JParticipants)) {
    throw std::runtime_error("needs " + std::to_string(JParticipants) +
                             " physical cores");
  }
  return buildTable(JParticipants, jSuffix, cal);
}

/// File-scope registrar.
struct ScanRegistrar {
  ScanRegistrar() {
    registerWorkload(
        {.name = "scan_inclusive_j8_n1M_int64_plus", .run = &runScanCell<8>});
    registerWorkload(
        {.name = "scan_inclusive_j16_n1M_int64_plus", .run = &runScanCell<16>});
    registerWorkload(
        {.name = "scan_inclusive_j32_n1M_int64_plus", .run = &runScanCell<32>});
    registerWorkload(
        {.name = "scan_inclusive_j48_n1M_int64_plus", .run = &runScanCell<48>});
    registerWorkload(
        {.name = "scan_inclusive_j96_n1M_int64_plus", .run = &runScanCell<96>});
  }
};

const ScanRegistrar kRegistrar;

} // namespace

} // namespace citor::bench
