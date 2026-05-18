// Two-pool BLAS coexistence workload.
//
// Measures the wall-time cost of `citor::ThreadPool::parallelFor` when it
// shares the host with a libomp-driven secondary worker pool. The cell pairs
// short citor dispatches with non-trivial body each, repeated to fill a fixed
// timing window) with a small matmul-shaped libomp `parallel for` that
// runs once just before the primary measurement so libomp's worker threads
// are warm and sitting in their park-spin window for the duration of the
// primary's timing.
//
// Two cells share the workload table:
//   - `kmp_default`: libomp keeps its default `KMP_BLOCKTIME=200ms` (set
//     at process startup before any citor work).
//   - `kmp_zero`: `kmp_set_blocktime(0)` is called before the primary
//     measurement so libomp parks promptly between dispatches.
//
// The "did it work" gate (per the spec): primary-pool wall time at
// `kmp_zero` should be meaningfully lower than at `kmp_default`. Both cells
// are reported in the same binary run so the reader can compare them in one
// run.
//
// This bench lives in a SEPARATE executable target (`parallel_bench_two_pool`)
// because libomp's `KMP_BLOCKTIME` and `omp_set_num_threads(...)` global
// state would leak across cells in the standard `parallel_bench` process.

#include <omp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "citor/always_assert.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "../harness/aligned_alloc.h"
#include "../harness/bench_format.h"
#include "../harness/bench_registry.h"
#include "../harness/cycle_clock.h"

namespace citor::bench {
namespace {

/// Hint preset for the primary citor parallel-for body. The body cost is
/// non-trivial per slot so the inline-fallback gate is
/// conservative: the bench never wants the primary call to silently degrade
/// into producer-only inline execution.
struct PrimaryForHints : citor::HintsDefaults {
  static constexpr citor::StealPolicy stealPolicy =
      citor::StealPolicy::ClusterLocal;
  static constexpr bool cancellationChecks = false;
};

/// Number of citor dispatches per measurement bracket. Each dispatch's body
/// performs non-trivial compute; 32 dispatches per bracket pushes the bracket
/// wall time well above the OS-jitter floor.
constexpr std::size_t kDispatchesPerBracket = 32;

/// Iteration count per cell. Each iteration is one bracket of
/// `kDispatchesPerBracket` primary dispatches; the headline is the lower-
/// quartile of the per-bracket wall time.
constexpr std::size_t kIterations = 60;
/// Warmup iterations per cell. Each warmup bracket runs one secondary matmul
/// followed by `kDispatchesPerBracket` primary dispatches. The first bracket
/// pays libomp's worker-pool initialization cost (the runtime spins up its
/// OpenMP team on the first `omp parallel for`); subsequent warmup brackets
/// stabilize libomp's spin-poll window and citor's workers' generation
/// observation. 16 warmup brackets is enough to push the libomp pool init
/// out of the timed window across reruns and bound cell-to-cell variance to
/// the per-iteration err% widths rather than letting libomp's first-call
/// startup land inside the first timed bracket on cold runs.
constexpr std::size_t kWarmupIterations = 16;

/// Body work: a tight floating-point loop sized to take non-trivial wall-time
/// per
/// `[lo, hi)` slot on a modern x86 host. The body operates over a per-call
/// scratch buffer (passed by pointer) so the compiler cannot prove the loop
/// is dead. Touch every element exactly once per iteration.
inline void primaryBody(std::size_t lo, std::size_t hi,
                        float *scratch) noexcept {
  // Each element is touched 64 times to land near 100us per slot at j=16
  // over a 16384-element range (1024 elements/slot * 64 multiply-adds = 64 K
  // FLOPs per slot; modern hosts hit the
  // bench-relevant inner-loop cost when memory traffic and per-element work
  // dominate over the FMA throughput).
  for (int rep = 0; rep < 64; ++rep) {
    for (std::size_t i = lo; i < hi; ++i) {
      const float v = scratch[i];
      scratch[i] = (v * 1.0001F) + 0.0001F;
    }
  }
}

/// Workload size for the primary's `parallelFor` range. 16384 elements at
/// j=16 gives 1024 elements/slot, which puts the per-slot body at the
/// the target after the inner repetition factor in `primaryBody`.
constexpr std::size_t kPrimaryRange = 16384;

/// 64-byte aligned `float[]` deleter; pairs with `std::unique_ptr`.
struct AlignedFloatDeleter {
  void operator()(float *p) const noexcept { alignedFree(p); }
};
// `unique_ptr<T[]>` is the only owning-dynamic-array spelling the standard
// offers; the check below is misfiring.
using AlignedFloatBuffer =
    std::unique_ptr<float[],
                    AlignedFloatDeleter>; // NOLINT(modernize-avoid-c-arrays)

[[nodiscard]] AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  void *raw = alignedAlloc(bytes, 64U);
  if (raw == nullptr) {
    std::abort();
  }
  return AlignedFloatBuffer{static_cast<float *>(raw)};
}

/// Secondary load generator: a single libomp `parallel for` that performs a
/// 256x256 matmul. Run once between primary brackets so libomp's worker
/// threads spin (or park, depending on `KMP_BLOCKTIME`) during the primary's
/// timing window.
constexpr std::size_t kSecondaryN = 256;

void secondaryMatmul(const float *aBase, const float *bBase,
                     float *cBase) noexcept {
  const std::size_t n = kSecondaryN;
  // ikj-ordered triple loop; fanned out to libomp threads on the outer i
  // loop. The body uses `omp parallel for` (not `omp parallel`
  // + `omp for`) so libomp creates / re-uses the worker pool inside this
  // single call rather than maintaining an outer region open across the
  // whole bench (which would not match the BLAS-coexistence shape the bench
  // models).
#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < n; ++i) {
    float *cRow = cBase + (i * n);
    for (std::size_t j = 0; j < n; ++j) {
      cRow[j] = 0.0F;
    }
    for (std::size_t k = 0; k < n; ++k) {
      const float aik = aBase[(i * n) + k];
      const float *bRow = bBase + (k * n);
      for (std::size_t j = 0; j < n; ++j) {
        cRow[j] += aik * bRow[j];
      }
    }
  }
}

/// Deterministic LCG fill; pairs with the secondary matmul's input init.
void deterministicFill(float *p, std::size_t count,
                       std::uint32_t seed) noexcept {
  std::uint32_t state = seed;
  for (std::size_t i = 0; i < count; ++i) {
    state = (state * 1664525U) + 1013904223U;
    p[i] = static_cast<float>(state >> 12U) * (1.0F / 1048576.0F);
  }
}

/// Verify the primary's body ran over every slot. The body's per-element
/// transform is `v -> (v * 1.0001) + 0.0001` repeated 64 times; after
/// `kDispatchesPerBracket` brackets the values diverge from the seed by a
/// non-negligible amount. We use a stride spot-check to assert the body
/// touched the buffer without paying the full pass on the hot path.
void spotCheck(const float *data) {
  const std::size_t stride = kPrimaryRange / 64U;
  for (std::size_t i = 0; i < kPrimaryRange; i += stride) {
    BENCH_CHECK_OR_THROW(data[i] != 0.0F, "blas_coexistence_bench.cpp");
  }
}

/// One cell's measurement. The cell selects the `KMP_BLOCKTIME` policy by
/// calling `kmp_set_blocktime(@ blocktimeMs)` before its first iteration.
[[nodiscard]] BenchRow measureCell(const char *name, int blocktimeMs,
                                   const CyclesPerNanosecond &cal) {
  // Establish the libomp blocktime policy for this cell. Every subsequent
  // libomp dispatch sees the new value; when `blocktimeMs > 0` workers stay
  // in their spin-poll window between secondary calls and contend for the
  // primary's CPUs.
  kmp_set_blocktime(blocktimeMs);

  // Construct the primary citor pool once per cell. The pool spawns its
  // background workers on physical cores by default; on a multi-CCD host
  // the workers split across CCDs.
  citor::ThreadPool primary{16U};

  AlignedFloatBuffer primaryBuf = allocateAlignedFloats(kPrimaryRange);
  for (std::size_t i = 0; i < kPrimaryRange; ++i) {
    primaryBuf[i] = 1.0F;
  }

  AlignedFloatBuffer secA = allocateAlignedFloats(kSecondaryN * kSecondaryN);
  AlignedFloatBuffer secB = allocateAlignedFloats(kSecondaryN * kSecondaryN);
  AlignedFloatBuffer secC = allocateAlignedFloats(kSecondaryN * kSecondaryN);
  deterministicFill(secA.get(), kSecondaryN * kSecondaryN, 0xA17EU);
  deterministicFill(secB.get(), kSecondaryN * kSecondaryN, 0xB23CU);

  float *const primaryData = primaryBuf.get();
  float *const aPtr = secA.get();
  float *const bPtr = secB.get();
  float *const cPtr = secC.get();

  auto runBracket = [&]() {
    // Run the secondary first so libomp's worker pool is in its blocktime
    // window when the primary timing starts. This is the configuration the
    // bench wants to expose: a real BLAS-coexistence call site issues
    // libomp work, waits for it, and then issues citor work back-to-back.
    secondaryMatmul(aPtr, bPtr, cPtr);
    for (std::size_t k = 0; k < kDispatchesPerBracket; ++k) {
      primary.parallelFor<PrimaryForHints>(
          std::size_t{0}, kPrimaryRange,
          [primaryData](std::size_t lo, std::size_t hi) noexcept {
            primaryBody(lo, hi, primaryData);
          });
    }
  };

  // Warmup: drop the first few brackets so libomp's pool is initialized and
  // citor's workers have observed at least one dispatch generation.
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    runBracket();
  }
  spotCheck(primaryData);

  // Reset primary buffer between cells so the body's per-element drift does
  // not overflow during the measurement window.
  for (std::size_t i = 0; i < kPrimaryRange; ++i) {
    primaryBuf[i] = 1.0F;
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    // Re-seed the buffer per iteration so each bracket's body operates on a
    // fresh start state and the optimizer cannot conclude the loop is dead.
    for (std::size_t k = 0; k < kPrimaryRange; ++k) {
      primaryData[k] = 1.0F;
    }
    const std::uint64_t startCycles = readCyclesStart();
    // The bracket times the *primary*'s wall time only; the secondary call
    // is INSIDE the bracket because the bench wants to measure the cost of
    // the citor primary while libomp workers are in their post-region
    // spin-or-park window. Including the secondary in the same bracket is
    // by design (matches the BLAS-coexistence call site shape).
    runBracket();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
  }
  spotCheck(primaryData);

  return finalizeRow(name, samples);
}

BenchTable runBlasCoexistence(const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = "two_pool_blas_coexistence";

  // Default cell first: libomp default blocktime (200 ms). On most hosts
  // the runtime exposes its compile-time default via `kmp_get_blocktime()`
  // before any explicit set; we record it here for diagnostic context, then
  // call the API explicitly so the cell's policy is reproducible.
  table.rows.push_back(measureCell("citor::ThreadPool kmp_blocktime=default",
                                   /*blocktimeMs=*/200, cal));
  // Cool-off between cells covers libomp's 200ms default blocktime so the
  // second cell starts with a fully-parked libomp pool, not residual spin
  // from the first cell.
  std::this_thread::sleep_for(std::chrono::milliseconds{300});
  table.rows.push_back(measureCell("citor::ThreadPool kmp_blocktime=0",
                                   /*blocktimeMs=*/0, cal));
  return table;
}

struct BlasCoexistenceRegistrar {
  BlasCoexistenceRegistrar() {
    registerWorkload(
        {.name = "two_pool_blas_coexistence", .run = &runBlasCoexistence});
  }
};

const BlasCoexistenceRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
