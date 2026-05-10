// StealPolicy sweep on citor under memory-irregular parallelFor.
//
// Two rows -- StealPolicy::Global vs StealPolicy::ClusterLocal -- run a
// strided memory access pattern designed to exercise cross-CCD cache
// coherence: a 64MB buffer is swept with a 2KB stride, so each iteration
// touches a fresh cache line outside the per-core L2 working set.
// Cluster-local steal probes keep load-balancing traffic inside the
// shared L3 most of the time and reach across the inter-cluster fabric
// only when the local cluster's deques drain; the contrast against the
// global probe shows the per-row coherence-traffic difference.
//
// j=16 only -- steal-policy differences are most visible at full
// machine width.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "citor/always_assert.h"
#include "citor/hints.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 30;
constexpr std::size_t kWarmupIterations = 3;

/// 64 MB buffer; 2 KB stride. 64MB / 2KB = 32768 stride positions per pass.
/// 64 MB exceeds typical L3-per-CCD on a multi-CCD chip, forcing cross-CCD
/// traffic when threads on different CCDs touch overlapping or adjacent
/// stride regions.
constexpr std::size_t kBufferBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kStrideBytes = 2ULL * 1024ULL;
constexpr std::size_t kN = kBufferBytes / kStrideBytes;

/// Sentinel pattern written / accumulated at every stride position.
constexpr std::uint64_t kPattern = 0xC11705C0DE5A11A5ULL;

/// Per-iteration cost: read the 8-byte word at stride offset `i`, XOR a
/// per-iteration constant into it, and write the new value back. The
/// read-modify-write pattern forces the cache line to MODIFIED state under
/// MESI, which is what produces cross-CCD coherence traffic when adjacent
/// iterations land on different CCDs. The XOR constant depends only on `i`
/// (not on prior iterations) so the post-state of every word is independent
/// of the partition: parallel and sequential evaluations leave the buffer in
/// the same byte-for-byte state.
inline void stridedBody(std::uint64_t *buffer, std::size_t lo, std::size_t hi,
                        std::atomic<std::uint64_t> &sink) noexcept {
  std::uint64_t localXor = 0;
  const std::size_t wordsPerStride = kStrideBytes / sizeof(std::uint64_t);
  for (std::size_t i = lo; i < hi; ++i) {
    const std::size_t idx = i * wordsPerStride;
    const std::uint64_t mix =
        kPattern ^ (static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL);
    const std::uint64_t v = buffer[idx];
    const std::uint64_t next = v ^ mix;
    buffer[idx] = next;
    localXor ^= next;
  }
  sink.fetch_xor(localXor, std::memory_order_relaxed);
}

/// Reset buffer to a deterministic pre-state so every measurement iteration
/// starts from the same byte pattern. Without the reset the buffer's contents
/// drift across iterations and the correctness gate's reference value moves
/// with them.
void resetBuffer(std::uint64_t *buffer, std::size_t words) noexcept {
  for (std::size_t i = 0; i < words; ++i) {
    buffer[i] = static_cast<std::uint64_t>(i) * 0x517C0DE51F1A2E2DULL;
  }
}

/// Compute the sequential reference for the strided body so every parallel
/// row's final XOR-sink is checked against a known constant. Because the
/// per-iteration mix is index-only, partitioning does not change the XOR
/// fold, so this single sequential pass is the correctness oracle for every
/// parallel partition.
[[nodiscard]] std::uint64_t
computeReferenceXor(std::vector<std::uint64_t> &buffer) {
  resetBuffer(buffer.data(), buffer.size());
  std::atomic<std::uint64_t> sink{0};
  stridedBody(buffer.data(), std::size_t{0}, kN, sink);
  return sink.load(std::memory_order_relaxed);
}

template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name,
                                   const CyclesPerNanosecond &cal, RunFn run,
                                   std::uint64_t referenceXor) {
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    const std::uint64_t v = run();
    CITOR_ALWAYS_ASSERT(v == referenceXor);
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::uint64_t value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(value == referenceXor);
  }
  return finalizeRow(name, samples);
}

[[nodiscard]] BenchRow measureWithAffinity(const char *name,
                                           std::size_t participants,
                                           citor::StealPolicy stealPolicy,
                                           const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  std::vector<std::uint64_t> buffer(kBufferBytes / sizeof(std::uint64_t), 0ULL);

  // Reference: run the body sequentially once to produce the canonical XOR
  // sink. The parallel runs reset the buffer to the same pre-state, so the
  // sequential result is the comparison oracle for every iteration.
  const std::uint64_t referenceXor = computeReferenceXor(buffer);

  citor::Hints hints;
  hints.stealPolicy = stealPolicy;
  hints.cancellationChecks = false;

  return measureLoop(
      name, cal,
      [&] {
        resetBuffer(buffer.data(), buffer.size());
        std::atomic<std::uint64_t> sink{0};
        pool.parallelForRuntime(
            std::size_t{0}, kN,
            [buf = buffer.data(), &sink](std::size_t lo, std::size_t hi) {
              stridedBody(buf, lo, hi, sink);
            },
            hints);
        return sink.load(std::memory_order_relaxed);
      },
      referenceXor);
}

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"affinity_sweep_strided_"} + suffix;
  table.rows.push_back(
      measureWithAffinity("citor::ThreadPool[StealPolicy::Global]",
                          participants, citor::StealPolicy::Global, cal));
  table.rows.push_back(
      measureWithAffinity("citor::ThreadPool[StealPolicy::ClusterLocal]",
                          participants, citor::StealPolicy::ClusterLocal, cal));
  return table;
}

BenchTable runAffinityJ16(const CyclesPerNanosecond &cal) {
  return buildTable(16, "j16_64MB_2KBstride", cal);
}

struct AffinitySweepRegistrar {
  AffinitySweepRegistrar() {
    registerWorkload({.name = "affinity_sweep_strided_j16_64MB_2KBstride",
                      .run = &runAffinityJ16});
  }
};

const AffinitySweepRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
