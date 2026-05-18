// Cold-dispatch tail-latency workload. Producer sleeps kCoolOff between
// iters so workers park; each iteration dispatches one trivial closure over
// [0, participants). Closed-loop p25/p50/p99: inter-arrival is controlled
// explicitly via cool-off, so coordinated-omission correction is not applied.
// Each row carries tailNs = {p25, p50, p99} extracted from a per-row
// HdrHistogram; columns render only with --with-tail-percentiles.

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

// Per-row sample budget. Cool-off sleep dominates wall time.
constexpr std::size_t kIterations = 300;

// Warmup iters dropped from the timing window.
constexpr std::size_t kWarmupIterations = 30;

// Sleep between iterations.
constexpr std::chrono::milliseconds kCoolOff{30};

// HdrHistogram trackable maximum (1 s ceiling for pathological tails).
constexpr std::int64_t kHdrMaxNs = 1'000'000'000LL;

// Significant-figures budget for the histogram (~0.1 % reporting precision).
constexpr int kHdrSigDigits = 3;

// RAII wrapper around HdrHistogram_c. Allocation failure aborts via
// BENCH_CHECK_OR_THROW.
class HdrHistogramHandle {
public:
  HdrHistogramHandle() {
    const int rc = ::hdr_init(/*lowest_discernible_value=*/1, kHdrMaxNs,
                              kHdrSigDigits, &m_hist);
    BENCH_CHECK_OR_THROW(rc == 0, "parallel_for_cold_dispatch_bench.cpp");
    BENCH_CHECK_OR_THROW(m_hist != nullptr,
                         "parallel_for_cold_dispatch_bench.cpp");
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

// Sample one pool's cold-dispatch latency, feeding each sample into both the
// sample vector (for headline p25 + err%) and the HdrHistogram (for tail).
// tailNs is filled when tailPercentilesEnabled() is set.
template <class PoolT, class Dispatch>
[[nodiscard]] BenchRow
measureColdDispatchWith(const char *displayName, std::size_t participants,
                        const CyclesPerNanosecond &cal, Dispatch dispatch) {
  if (!engineEnabled(displayName)) {
    BenchRow row{};
    row.name = displayName;
    row.skipped = true;
    return row;
  }
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  // body increments sink by the slot count it covers; the final sink count
  // is the bench's correctness gate.
  std::atomic<std::uint64_t> sink{0};
  const auto body = [&sink](std::size_t lo, std::size_t hi) noexcept {
    sink.fetch_add(hi - lo, std::memory_order_relaxed);
  };

  // Warmup ensures each pool's workers have spawned before the timing window.
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    std::this_thread::sleep_for(kCoolOff);
    dispatch(*pool, 0, participants, body);
  }

  // Correctness gate: warmup alone must have driven sink to
  // kWarmupIterations * participants. Fires before the timing window opens.
  BENCH_CHECK_OR_THROW(sink.load(std::memory_order_relaxed) ==
                           static_cast<std::uint64_t>(kWarmupIterations) *
                               static_cast<std::uint64_t>(participants),
                       "parallel_for_cold_dispatch_bench.cpp");

  const HdrHistogramHandle hist;

  // Timing window. Drives the same `dispatch` shim the warmup used so the
  // row label's `HintsT` (StaticHints / DynamicHints for citor, Traits for
  // peers) is what we actually measure; the prior `Traits::submitBlocksAndWait`
  // wired citor through `parallelFor<DynamicHints>` regardless of the row,
  // collapsing Static and Dynamic onto the same number.
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    std::this_thread::sleep_for(kCoolOff);
    const std::uint64_t startCycles = readCyclesStart();
    dispatch(*pool, 0, participants, body);
    const std::uint64_t endCycles = readCyclesEnd();
    const double ns = cyclesToNs(endCycles - startCycles, cal);
    samples.push_back(ns);
    // Clamp to the histogram's tracking range; a sample above kHdrMaxNs is a
    // multi-second stall and is recorded at the ceiling rather than dropped.
    const std::int64_t recorded =
        std::min<std::int64_t>(static_cast<std::int64_t>(ns), kHdrMaxNs);
    (void)::hdr_record_value(hist.get(), recorded);
  }

  // Final correctness gate.
  BENCH_CHECK_OR_THROW(
      sink.load(std::memory_order_relaxed) ==
          static_cast<std::uint64_t>(kWarmupIterations + kIterations) *
              static_cast<std::uint64_t>(participants),
      "parallel_for_cold_dispatch_bench.cpp");

  BenchRow row = finalizeRow(displayName, samples);
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

// Drives a peer pool through its submitBlocksAndWait shim.
template <class PoolT>
[[nodiscard]] BenchRow measureColdDispatch(std::size_t participants,
                                           const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  return measureColdDispatchWith<PoolT>(
      Traits::name, participants, cal,
      [](PoolT &pool, std::size_t first, std::size_t last, auto fn) {
        Traits::submitBlocksAndWait(pool, first, last, fn);
      });
}

// Citor's row driven by an explicit hint type so Static-vs-Dynamic balance
// shows up side-by-side. The hint must inherit DynamicHints so per-block cost
// is identical across the two rows; only the balance differs.
template <class HintsT>
[[nodiscard]] BenchRow
measureCitorColdDispatchWithHint(const char *displayName,
                                 std::size_t participants,
                                 const CyclesPerNanosecond &cal) {
  return measureColdDispatchWith<citor::ThreadPool>(
      displayName, participants, cal,
      [](citor::ThreadPool &pool, std::size_t first, std::size_t last,
         auto fn) { pool.parallelFor<HintsT>(first, last, fn); });
}

// Build a cold-dispatch comparison table for a single j value.
BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"parallel_for_cold_dispatch_"} + suffix;

  table.rows.push_back(measureCitorColdDispatchWithHint<citor::StaticHints>(
      "citor::ThreadPool[Static]", participants, cal));
  table.rows.push_back(measureCitorColdDispatchWithHint<citor::DynamicHints>(
      "citor::ThreadPool[Dynamic]", participants, cal));
  table.rows.push_back(
      measureColdDispatch<BS::light_thread_pool>(participants, cal));
  table.rows.push_back(
      measureColdDispatch<dp::thread_pool<>>(participants, cal));
  table.rows.push_back(
      measureColdDispatch<::task_thread_pool::task_thread_pool>(participants,
                                                                cal));
  table.rows.push_back(
      measureColdDispatch<riften::Thiefpool>(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(
      measureColdDispatch<::tbb::task_arena>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureColdDispatch<::tf::Executor>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(
      measureColdDispatch<::Eigen::ThreadPool>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureColdDispatch<OpenMpRunner>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(
      measureColdDispatch<hmthrp::ThreadPool>(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(
      measureColdDispatch<dispenso::ThreadPool>(participants, cal));
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

struct ColdDispatchRegistrar {
  ColdDispatchRegistrar() {
    registerWorkload(
        {.name = "parallel_for_cold_dispatch_j8", .run = &runCell<8>});
    registerWorkload(
        {.name = "parallel_for_cold_dispatch_j16", .run = &runCell<16>});
    registerWorkload(
        {.name = "parallel_for_cold_dispatch_j32", .run = &runCell<32>});
    registerWorkload(
        {.name = "parallel_for_cold_dispatch_j48", .run = &runCell<48>});
  }
};

const ColdDispatchRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
