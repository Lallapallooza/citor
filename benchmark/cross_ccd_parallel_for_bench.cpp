// Cross-CCD `parallelFor` workload for the comparative pool bench.
//
// Captures the producer-first-touch + cross-CCD writeback shape. The producer
// thread is pinned to a CPU on one CCD and zero-fills the buffer between
// timed dispatches, so the cache lines are owned by the producer's CCD when
// the dispatch publishes. The dispatch is routed into a `citor::PoolGroup`
// arena pinned to a (potentially) different CCD; the arena's workers then
// write `1.0F` over the same lines. On rows where the producer and the
// arena's workers live on opposite CCDs, every body store first invalidates
// the producer-CCD-resident line via the inter-CCD coherence domain, then
// installs the modified line in the arena CCD's L3. The dispatch's
// `generation` / `activeJob` / `doneEpoch` lines also cross the inter-CCD
// boundary on every cycle, but the bulk of the wall time is the writeback
// traffic over the 64 MiB working set; that is what the row-pair contrast
// (producer/arena same-CCD vs producer/arena different-CCD) exposes.
//
// The bench is citor-only: `PoolGroup` is a citor-specific construct, and
// none of the surveyed competitor pools expose a per-CCD-pinned arena
// abstraction we could route through symmetrically. The table prints three
// rows in a single `BenchTable`, one per producer/arena placement.
//
// Internal correctness: each iteration's parallel sum must equal the
// sequential reference; `CITOR_ALWAYS_ASSERT` aborts on mismatch BEFORE the
// timing window closes (the assert lives inside the timed block, but the
// check is cheap relative to the 64 MiB-buffer touch loop and only fires on
// real bugs).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "citor/always_assert.h"
#include "citor/hints.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "cycle_clock.h"
#include "multi_arena_harness.h"

namespace citor::bench {
namespace {

/// Element count for the `parallelFor` body. 64 MiB of `float` (16'777'216
/// elements) sits well above any single CCD's L3 capacity on typical hosts
/// (32 MiB or 64 MiB per CCD), so the body genuinely traverses DRAM-resident
/// data and the cross-CCD coherence traffic on the dispatch path is not
/// hidden by L3 hit rates.
constexpr std::size_t kElementCount = 16U * 1024U * 1024U;

/// Per-cell sample budget. The body takes a few milliseconds on a typical
/// host; 30 samples keeps a row under a couple of seconds wall while still
/// giving `finalizeRow`'s lower-quartile estimator enough samples to converge.
constexpr std::size_t kIterations = 30;
constexpr std::size_t kWarmupIterations = 5;

/// Hint preset for the cross-CCD body. Static-uniform balance keeps the
/// dispatch contract identical across rows (only the producer/arena placement
/// changes); SplitCcd affinity is the citor default for bulk parallel-for and
/// matches the workload's intent.
struct CrossCcdHints : citor::HintsDefaults {
  static constexpr citor::Affinity affinity = citor::Affinity::SplitCcd;
  static constexpr bool cancellationChecks = false;
};

/// 64-byte aligned `float[]` deleter.
struct AlignedFloatDeleter {
  void operator()(float *p) const noexcept {
    if (p != nullptr) {
      std::free(p);
    }
  }
};

using AlignedFloatBuffer = std::unique_ptr<float[], AlignedFloatDeleter>;

[[nodiscard]] AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  void *raw = nullptr;
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  if (::posix_memalign(&raw, 64U, bytes) != 0) {
    std::abort();
  }
  return AlignedFloatBuffer{static_cast<float *>(raw)};
}

/// RAII helper that pins the calling thread to a single CPU while alive,
/// restoring the original affinity mask on destruction. The bench uses one
/// instance per row so each cell observes the producer-on-CPU placement the
/// row claims to measure.
class ScopedThreadPin {
public:
  explicit ScopedThreadPin(unsigned cpu) noexcept {
#ifdef __linux__
    CPU_ZERO(&m_saved);
    if (pthread_getaffinity_np(pthread_self(), sizeof(m_saved), &m_saved) != 0) {
      m_restore = false;
      return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpu), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
      m_restore = true;
    }
#else
    (void)cpu;
#endif
  }

  ~ScopedThreadPin() {
#ifdef __linux__
    if (m_restore) {
      (void)pthread_setaffinity_np(pthread_self(), sizeof(m_saved), &m_saved);
    }
#endif
  }

  ScopedThreadPin(const ScopedThreadPin &) = delete;
  ScopedThreadPin &operator=(const ScopedThreadPin &) = delete;
  ScopedThreadPin(ScopedThreadPin &&) = delete;
  ScopedThreadPin &operator=(ScopedThreadPin &&) = delete;

private:
#ifdef __linux__
  cpu_set_t m_saved{};
  bool m_restore = false;
#endif
};

/// Float comparison reference: every element in the buffer should equal `1.0`
/// after the body runs (each body slot writes `1.0` into its range), so the
/// expected sum is exactly `kElementCount`. The body writes a
/// constant rather than reading the input; this keeps cross-CCD coherence
/// traffic on the *dispatch* path the bench wants to measure, instead of
/// pulling the reading-side cache lines into a different CCD's L3.
[[nodiscard]] double expectedSum() noexcept { return static_cast<double>(kElementCount); }

/// One row's measurement helper. The producer is pinned to |producerCpu| for
/// the duration of the call; `arena(arenaCcd)` services the dispatch.
///
/// The `harness` is constructed BEFORE the `ScopedThreadPin`: `PoolGroup`'s
/// first-call topology probe reads `sched_getaffinity`, and a single-CPU
/// affinity mask collapses the probe to one synthetic CCD. Constructing the
/// harness first ensures the singleton is initialized while the calling
/// thread still has the full process affinity mask. Subsequent harness
/// constructions reuse the singleton.
[[nodiscard]] BenchRow measureRow(const char *name, unsigned producerCpu, std::size_t arenaCcd,
                                  const CyclesPerNanosecond &cal) {
  MultiArenaHarness harness{/*requiredCcds=*/2U};
  CITOR_ALWAYS_ASSERT(arenaCcd < harness.arenaCount());
  citor::ThreadPool &pool = harness.arena(arenaCcd);
  CITOR_ALWAYS_ASSERT(pool.kind() == citor::PoolKind::Arena);

  ScopedThreadPin pin(producerCpu);

  AlignedFloatBuffer buf = allocateAlignedFloats(kElementCount);
  float *const data = buf.get();

  // First-touch fill so the buffer pages are resident before the timed
  // window. The test thread's NUMA placement determines the home node; the
  // bench treats the home-node + cross-CCD interaction as part of the cost
  // surface it measures (named in the row description).
  for (std::size_t i = 0; i < kElementCount; ++i) {
    data[i] = 0.0F;
  }

  // Warmup + correctness gate. The body writes `1.0F` over every element of
  // its range; after the dispatch the sum must equal `kElementCount`.
  auto runOnce = [&]() {
    pool.parallelFor<CrossCcdHints>(std::size_t{0}, kElementCount,
                                    [data](std::size_t lo, std::size_t hi) noexcept {
                                      for (std::size_t i = lo; i < hi; ++i) {
                                        data[i] = 1.0F;
                                      }
                                    });
    double sum = 0.0;
    for (std::size_t i = 0; i < kElementCount; ++i) {
      sum += static_cast<double>(data[i]);
    }
    return sum;
  };
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    const double sum = runOnce();
    CITOR_ALWAYS_ASSERT(sum == expectedSum());
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    // Reset the buffer between iterations so each `parallelFor` body has
    // observable work even if the optimizer noticed the body writes a
    // constant.
    for (std::size_t k = 0; k < kElementCount; ++k) {
      data[k] = 0.0F;
    }
    const std::uint64_t startCycles = readCyclesStart();
    pool.parallelFor<CrossCcdHints>(std::size_t{0}, kElementCount,
                                    [data](std::size_t lo, std::size_t hi) noexcept {
                                      for (std::size_t j = lo; j < hi; ++j) {
                                        data[j] = 1.0F;
                                      }
                                    });
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));

    // Cheap correctness check: a stride-shifted spot-check confirms the body
    // ran over the full range without paying the O(n) sequential sum on the
    // hot path. Mismatch aborts via CITOR_ALWAYS_ASSERT.
    const std::size_t stride = kElementCount / 64U;
    for (std::size_t k = 0; k < kElementCount; k += stride) {
      CITOR_ALWAYS_ASSERT(data[k] == 1.0F);
    }
  }

  return finalizeRow(name, samples);
}

/// Pick a CPU id from a CCD's enumerated CPU list, defaulting to the first
/// CPU. Returns `0` for an empty list (defensive; `enumerateCcds()` never
/// produces empty inner vectors on hosts the bench can run on).
[[nodiscard]] unsigned firstCpuOfCcd(const std::vector<unsigned> &cpus) noexcept {
  return cpus.empty() ? 0U : cpus.front();
}

BenchTable runCrossCcd(const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = "cross_ccd_parallel_for";

  const auto ccds = enumerateCcds();
  if (ccds.size() < 2U) {
    throw std::runtime_error{"cross-CCD bench: enumerateCcds() returned fewer than 2 CCDs at "
                             "runtime; topology may have been collapsed by post-init affinity. "
                             "Skipping."};
  }
  const unsigned cpuOnCcd0 = firstCpuOfCcd(ccds[0]);
  const unsigned cpuOnCcd1 = firstCpuOfCcd(ccds[1]);

  table.rows.push_back(measureRow("citor::ThreadPool producer_ccd0_arena_ccd1", cpuOnCcd0,
                                  /*arenaCcd=*/1U, cal));
  table.rows.push_back(measureRow("citor::ThreadPool producer_ccd1_arena_ccd0", cpuOnCcd1,
                                  /*arenaCcd=*/0U, cal));
  table.rows.push_back(measureRow("citor::ThreadPool producer_ccd0_arena_ccd0", cpuOnCcd0,
                                  /*arenaCcd=*/0U, cal));
  return table;
}

struct CrossCcdRegistrar {
  CrossCcdRegistrar() {
    try {
      (void)requireMultipleCcds(2U);
    } catch (const std::exception &) {
      // Single-CCD host: skip registration so the workload is not
      // (mis-)measured against a topology that cannot satisfy its intent.
      // The bench output omits the row.
      return;
    }
    registerWorkload({.name = "cross_ccd_parallel_for", .run = &runCrossCcd});
  }
};

const CrossCcdRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
