// Cold-dispatch tail-latency workload for the comparative pool bench.
//
// Replaces submitDetached open-loop as the v1 tail-latency vehicle: the
// public submitDetached primitive is `std::thread`-per-call (deferred), so
// the v1 narrative claim "we publish p99 because we claim sub-microsecond
// dispatch" rides on `parallelFor` cold-dispatch p99 instead.
//
// The cell sleeps `kCoolOff` between iterations so every pool's workers
// park before the next dispatch. Each iteration submits a single trivial
// closure spanning `[0, participants)`; the cycle bracket covers the
// dispatch + wake + body + join sequence. The measurement is closed-loop
// p25/p50/p99 with explicit 30ms cool-off between iterations; not
// coordinated-omission-corrected, by design -- the closed-loop sampler
// controls inter-arrival explicitly via cool-off, so the recorded
// histogram represents real per-iteration dispatch latency rather than
// CO-synthesized phantom samples.
//
// Output: each row carries `tailNs = {p25, p50, p99}` extracted from the
// per-row HdrHistogram. The columns render only when the bench is invoked
// with `--with-tail-percentiles`; otherwise the output shape is byte-
// identical to `cold_path_bench.cpp` for parsers that key off the existing
// columns.
//
// Internal correctness check: every iteration increments a sink atomic by
// the slot count; the final sink count must equal `kCells * iterations` (the
// dispatch correctness gate, run BEFORE the timing window).

#include <hdr/hdr_histogram.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "citor/always_assert.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"
#include "harness.h"

namespace citor::bench {
namespace {

/// Per-row sample budget. The cool-off sleep dominates wall time. 300
/// samples gives the bottom-quartile MAD ~75 draws while keeping per-row
/// wall time around ~10 s.
constexpr std::size_t kIterations = 300;

/// Warmup iterations dropped from the timing window. Covers OpenMP's lazy
/// thread-pool re-fill after a cold gap and citor's first-touch wake page.
constexpr std::size_t kWarmupIterations = 30;

/// Sleep between iterations. Long enough for every pool's spin-then-park
/// budget to expire; matches `cold_path_bench.cpp`'s 30 ms.
constexpr std::chrono::milliseconds kCoolOff{30};

/// HdrHistogram trackable maximum (1 second). Cold dispatch is in the
/// micro-to-millisecond range; a 1 s ceiling leaves headroom even for
/// pathological tails (page-fault on the wake closure, NUMA scan).
constexpr std::int64_t kHdrMaxNs = 1'000'000'000LL;

/// Significant-figures budget for the histogram. 3 digits gives ~0.1 %
/// reporting precision, sufficient for p25/p50/p99 in the comparison table.
constexpr int kHdrSigDigits = 3;

/// RAII wrapper around HdrHistogram_c so the per-row histogram releases its
/// backing arena even on exceptional unwind. Allocation failure aborts via
/// `CITOR_ALWAYS_ASSERT` -- a histogram allocation failure means the cell
/// cannot publish corrected p99, which is the row's only deliverable.
class HdrHistogramHandle {
public:
  HdrHistogramHandle() {
    const int rc = ::hdr_init(/*lowest=*/1, kHdrMaxNs, kHdrSigDigits, &m_hist);
    CITOR_ALWAYS_ASSERT(rc == 0 && m_hist != nullptr);
  }
  HdrHistogramHandle(const HdrHistogramHandle &) = delete;
  HdrHistogramHandle &operator=(const HdrHistogramHandle &) = delete;
  HdrHistogramHandle(HdrHistogramHandle &&) = delete;
  HdrHistogramHandle &operator=(HdrHistogramHandle &&) = delete;
  ~HdrHistogramHandle() {
    if (m_hist != nullptr) {
      ::hdr_close(m_hist);
    }
  }

  [[nodiscard]] ::hdr_histogram *get() const noexcept { return m_hist; }

private:
  ::hdr_histogram *m_hist = nullptr;
};

/// Sample one pool's cold-dispatch latency over `kIterations` rounds,
///        feeding each sample into both the median sample vector AND the
///        per-row HdrHistogram.
///
/// Each iteration sleeps `kCoolOff` to let workers park, then dispatches an
/// empty closure over `[0, participants)`. The cycle delta covers the
/// dispatch + wake + body + join sequence; the sleep is outside the bracket.
///
/// The HdrHistogram is fed via `hdr_record_value`. The measurement is
/// closed-loop and the producer controls inter-arrival explicitly via the
/// `kCoolOff` sleep; coordinated-omission correction would synthesize
/// phantom samples that misrepresent what each iteration actually observed,
/// so it is not applied here.
///
/// PoolT   Concrete pool type (the trait's specialization key).
/// participants Total worker count to construct the pool with.
/// cal     Calibration constant for converting cycles to ns.
/// A populated `BenchRow` with tailNs filled when the global
///                 `tailPercentilesEnabled()` flag is set.
template <class PoolT>
[[nodiscard]] BenchRow measureColdDispatch(std::size_t participants,
                                           const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  // Sink + body: the body increments the sink by the slot count it covers.
  // Final sink == sum(participants) over the warmup + sample iterations,
  // which is the bench's correctness gate.
  std::atomic<std::uint64_t> sink{0};
  const auto body = [&sink](std::size_t lo, std::size_t hi) noexcept {
    sink.fetch_add(hi - lo, std::memory_order_relaxed);
  };

  // Warmup. Discarded from the cycle samples; runs to ensure each pool's
  // workers have been spawned at least once before the timing window opens.
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    std::this_thread::sleep_for(kCoolOff);
    Traits::submitBlocksAndWait(*pool, 0, participants, body);
  }

  // Correctness gate: the warmup alone must have driven the sink to
  // `kWarmupIterations * participants`. If the body never ran on some
  // iterations the gate fires before the timing window opens.
  CITOR_ALWAYS_ASSERT(sink.load(std::memory_order_relaxed) ==
                      static_cast<std::uint64_t>(kWarmupIterations) *
                          static_cast<std::uint64_t>(participants));

  HdrHistogramHandle hist;

  // Timing window. Samples are pushed into both the sample vector (for the
  // headline p25 + err%) and the HdrHistogram (for the per-row tail). The
  // measurement is closed-loop and inter-arrival is controlled by the
  // explicit `kCoolOff` sleep; samples are recorded as-is.
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    std::this_thread::sleep_for(kCoolOff);
    const std::uint64_t startCycles = readCyclesStart();
    Traits::submitBlocksAndWait(*pool, 0, participants, body);
    const std::uint64_t endCycles = readCyclesEnd();
    const double ns = cyclesToNs(endCycles - startCycles, cal);
    samples.push_back(ns);
    // Clamp to the histogram's tracking range. A sample above kHdrMaxNs is
    // a multi-second stall and is recorded at the ceiling so the p99 still
    // reflects "outside the trackable range" rather than silently dropping.
    const std::int64_t recorded = std::min<std::int64_t>(static_cast<std::int64_t>(ns), kHdrMaxNs);
    (void)::hdr_record_value(hist.get(), recorded);
  }

  // Final correctness gate. Each iteration (warmup + timing) increments the
  // sink by `participants`.
  CITOR_ALWAYS_ASSERT(sink.load(std::memory_order_relaxed) ==
                      static_cast<std::uint64_t>(kWarmupIterations + kIterations) *
                          static_cast<std::uint64_t>(participants));

  BenchRow row = finalizeRow(Traits::name, samples);
  if (tailPercentilesEnabled()) {
    const std::int64_t p25Ns = ::hdr_value_at_percentile(hist.get(), 25.0);
    const std::int64_t p50Ns = ::hdr_value_at_percentile(hist.get(), 50.0);
    const std::int64_t p99Ns = ::hdr_value_at_percentile(hist.get(), 99.0);
    row.tailNs = std::array<double, 3>{
        static_cast<double>(p25Ns),
        static_cast<double>(p50Ns),
        static_cast<double>(p99Ns),
    };
  }
  return row;
}

/// Build a cold-dispatch comparison table for a single `j` value.
BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"parallel_for_cold_dispatch_"} + suffix;

  table.rows.push_back(measureColdDispatch<citor::ThreadPool>(participants, cal));
  table.rows.push_back(measureColdDispatch<BS::light_thread_pool>(participants, cal));
  table.rows.push_back(measureColdDispatch<dp::thread_pool<>>(participants, cal));
  table.rows.push_back(
      measureColdDispatch<::task_thread_pool::task_thread_pool>(participants, cal));
  table.rows.push_back(measureColdDispatch<riften::Thiefpool>(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureColdDispatch<::tbb::task_arena>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureColdDispatch<::tf::Executor>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureColdDispatch<::Eigen::ThreadPool>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureColdDispatch<OpenMpRunner>(participants, cal));
#endif

  return table;
}

BenchTable runColdDispatchJ16(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/16, "j16", cal);
}

BenchTable runColdDispatchJ8(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/8, "j8", cal);
}

struct ColdDispatchRegistrar {
  ColdDispatchRegistrar() {
    registerWorkload({.name = "parallel_for_cold_dispatch_j8", .run = &runColdDispatchJ8});
    registerWorkload({.name = "parallel_for_cold_dispatch_j16", .run = &runColdDispatchJ16});
  }
};

const ColdDispatchRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
