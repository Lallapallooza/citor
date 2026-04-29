// 2D 5-point Jacobi heat-diffusion workload for the comparative pool bench.
//
// `u_new[i,j] = 0.25 * (u[i-1,j] + u[i+1,j] + u[i,j-1] + u[i,j+1])` over an
// `N x N` grid (interior nodes only; boundary held at the initial value).
// `T = 200` timesteps; each timestep is one `parallelFor` over rows. The
// outer loop chains the timesteps; this is back-to-back parallelFor per
// timestep, NOT runPlex (the workload's name reflects the algorithmic
// shape, not the citor primitive driving it).
//
// All 8 competitor pools render natively via `CompetitorTraits<P>::parallelFor`.
// citor uses a Heat-tuned hint set (chunk = 0, auto-pick rows-per-block
// from participants) so the row partition matches what the future-pool
// shims produce; without this match, citor's empty-fan-out hint of
// chunk=1 would produce one block per row, paying per-block dispatch
// 256x more times than the competitor partition over a 2048-row grid.
//
// Internal correctness check: a sequential reference run on the same seed
// is run BEFORE the timing window; the parallel run's final-grid checksum
// must match the reference checksum bit-identically. The body is
// associative-commutative (every cell's result depends on its 4 neighbors
// from the previous timestep, not from any other cell at the current
// timestep), so the row-block decomposition is data-race-free without
// further synchronization.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "citor/always_assert.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 12;
constexpr std::size_t kWarmupIterations = 2;
constexpr std::size_t kTimesteps = 200;

/// Hint preset for the citor parallelFor call inside Heat. `chunk = 0` lets
/// the engine derive rows-per-block from `participants` (one block per
/// participant), matching the partition the future-pool shims use. Without
/// this override the bench would inherit `EmptyFanoutHints::chunk = 1` from
/// the citor competitor trait (designed for the dispatch-floor benches),
/// paying per-block dispatch hundreds of times more often than peers.
struct HeatHints {
  static constexpr citor::Balance balance = citor::Balance::StaticUniform;
  static constexpr citor::Determinism determinism = citor::Determinism::FixedBlockOrder;
  static constexpr citor::Affinity affinity = citor::Affinity::None;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  static constexpr citor::Partition partition = citor::Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool tlsRequired = false;
  static constexpr bool allowProducer = true;
  static constexpr bool allowWorkerSteal = false;
  static constexpr bool allowNestedParallelism = false;
  static constexpr bool fpDeterministicTree = true;
  static constexpr bool cancellationChecks = false;
  static constexpr bool pipelineSameChunk = false;
};

/// 64-byte aligned `double[]` deleter; pairs with `std::unique_ptr`.
struct AlignedDoubleDeleter {
  void operator()(double *p) const noexcept {
    if (p != nullptr) {
      std::free(p);
    }
  }
};

using AlignedDoubleBuffer = std::unique_ptr<double[], AlignedDoubleDeleter>;

AlignedDoubleBuffer allocateAlignedDoubles(std::size_t count) {
  void *raw = nullptr;
  const std::size_t bytes = ((count * sizeof(double) + 63U) / 64U) * 64U;
  if (::posix_memalign(&raw, 64U, bytes) != 0) {
    std::abort();
  }
  return AlignedDoubleBuffer{static_cast<double *>(raw)};
}

/// Deterministic LCG-seeded fill. Same seed -> identical initial grid for
/// every (pool, n) cell so the checksum invariant is well-defined.
void deterministicFill(double *p, std::size_t count, std::uint32_t seed) noexcept {
  std::uint32_t state = seed;
  for (std::size_t i = 0; i < count; ++i) {
    state = state * 1664525U + 1013904223U;
    p[i] = static_cast<double>(state >> 12U) * (1.0 / 1048576.0);
  }
}

/// Sequential 5-point Jacobi reference; used to compute the correctness
/// checksum the parallel run must match before any timing iteration.
[[nodiscard]] double seqJacobi(std::size_t n, std::size_t timesteps, std::uint32_t seed) {
  const std::size_t elems = n * n;
  AlignedDoubleBuffer u = allocateAlignedDoubles(elems);
  AlignedDoubleBuffer uNext = allocateAlignedDoubles(elems);
  deterministicFill(u.get(), elems, seed);
  // Boundary: leave the initial values in place; only interior is updated.
  std::copy(u.get(), u.get() + elems, uNext.get());

  for (std::size_t t = 0; t < timesteps; ++t) {
    for (std::size_t i = 1; i < n - 1U; ++i) {
      for (std::size_t j = 1; j < n - 1U; ++j) {
        uNext.get()[i * n + j] = 0.25 * (u.get()[(i - 1U) * n + j] + u.get()[(i + 1U) * n + j] +
                                         u.get()[i * n + (j - 1U)] + u.get()[i * n + (j + 1U)]);
      }
    }
    std::swap(u, uNext);
  }
  // Checksum: sum of interior cells.
  double sum = 0.0;
  for (std::size_t i = 1; i < n - 1U; ++i) {
    for (std::size_t j = 1; j < n - 1U; ++j) {
      sum += u.get()[i * n + j];
    }
  }
  return sum;
}

/// Per-timestep row-block body. `rowFirst..rowLast` are interior row
/// indices (both clipped to `[1, n-1)` upstream).
inline void jacobiRowBlock(std::size_t rowFirst, std::size_t rowLast, std::size_t n,
                           const double *uIn, double *uOut) noexcept {
  for (std::size_t i = rowFirst; i < rowLast; ++i) {
    for (std::size_t j = 1; j < n - 1U; ++j) {
      uOut[i * n + j] = 0.25 * (uIn[(i - 1U) * n + j] + uIn[(i + 1U) * n + j] +
                                uIn[i * n + (j - 1U)] + uIn[i * n + (j + 1U)]);
    }
  }
}

/// Per-pool sample function. Uses the trait's `parallelFor` for citor + the
/// 4 native-primitive competitors (oneTBB, Taskflow, Eigen, OpenMP) and the
/// 4 future-pool shims (BS, dp, task, riften) configured in
/// `competitor_traits.h`. Riften's `parallelFor` shim takes a participant
/// count argument so its overload signature differs; we wrap that case.
template <class PoolT>
[[nodiscard]] BenchRow measureHeat(std::size_t participants, std::size_t n,
                                   const CyclesPerNanosecond &cal, double referenceChecksum) {
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  const std::size_t elems = n * n;
  AlignedDoubleBuffer u = allocateAlignedDoubles(elems);
  AlignedDoubleBuffer uNext = allocateAlignedDoubles(elems);

  constexpr std::uint32_t kSeed = 0xC1701U;

  auto runOnce = [&]() {
    deterministicFill(u.get(), elems, kSeed);
    std::copy(u.get(), u.get() + elems, uNext.get());
    for (std::size_t t = 0; t < kTimesteps; ++t) {
      const double *uIn = u.get();
      double *uOut = uNext.get();
      const auto body = [uIn, uOut, n](std::size_t lo, std::size_t hi) noexcept {
        // Interior rows only; clip [lo, hi) to [1, n-1).
        const std::size_t rLo = std::max<std::size_t>(lo, 1U);
        const std::size_t rHi = hi >= n - 1U ? n - 1U : hi;
        if (rLo < rHi) {
          jacobiRowBlock(rLo, rHi, n, uIn, uOut);
        }
      };
      // Dispatch over [1, n-1) rows. citor uses HeatHints (chunk = 0 so the
      // engine partitions one block per participant) instead of going
      // through the Traits adapter, which is wired with EmptyFanoutHints
      // (chunk = 1) for the dispatch-floor benches. All other pools go
      // through their trait's `parallelFor`, whose shim partitions into
      // one block per participant.
      if constexpr (std::is_same_v<PoolT, citor::ThreadPool>) {
        pool->template parallelFor<HeatHints>(std::size_t{1}, n - 1U, body);
      } else {
        Traits::parallelFor(*pool, std::size_t{1}, n - 1U, participants, body);
      }
      std::swap(u, uNext);
    }
    // Checksum.
    double sum = 0.0;
    for (std::size_t i = 1; i < n - 1U; ++i) {
      for (std::size_t j = 1; j < n - 1U; ++j) {
        sum += u.get()[i * n + j];
      }
    }
    return sum;
  };

  // Correctness gate: warmup-and-validate before the timing window opens.
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    const double sum = runOnce();
    // Bit-identical comparison would require the row partition to be
    // associative-commutative-identical to the sequential traversal; for
    // this 5-point kernel each cell update is fully independent within a
    // timestep so the result IS bit-identical to the sequential run.
    CITOR_ALWAYS_ASSERT(sum == referenceChecksum);
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const double sum = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(sum == referenceChecksum);
  }

  return finalizeRow(Traits::name, samples);
}

struct HeatCell {
  std::size_t n;
  const char *suffix;
};

constexpr std::array<HeatCell, 2> kCells{{
    {.n = 2048, .suffix = "n2048"},
    {.n = 4096, .suffix = "n4096"},
}};

BenchTable buildTable(std::size_t participants, HeatCell cell, const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload =
      std::string{"parallelfor_heat_j"} + std::to_string(participants) + "_" + cell.suffix;

  // Sequential reference checksum is computed once per cell and reused
  // across pool rows.
  const double reference = seqJacobi(cell.n, kTimesteps, /*seed=*/0xC1701U);

  table.rows.push_back(measureHeat<citor::ThreadPool>(participants, cell.n, cal, reference));
  table.rows.push_back(measureHeat<BS::light_thread_pool>(participants, cell.n, cal, reference));
  table.rows.push_back(measureHeat<dp::thread_pool<>>(participants, cell.n, cal, reference));
  table.rows.push_back(
      measureHeat<::task_thread_pool::task_thread_pool>(participants, cell.n, cal, reference));
  table.rows.push_back(measureHeat<riften::Thiefpool>(participants, cell.n, cal, reference));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureHeat<::tbb::task_arena>(participants, cell.n, cal, reference));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureHeat<::tf::Executor>(participants, cell.n, cal, reference));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureHeat<::Eigen::ThreadPool>(participants, cell.n, cal, reference));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureHeat<OpenMpRunner>(participants, cell.n, cal, reference));
#endif
  return table;
}

template <std::size_t CellIdx, std::size_t Participants>
BenchTable runHeatCell(const CyclesPerNanosecond &cal) {
  constexpr HeatCell cell = kCells[CellIdx];
  return buildTable(Participants, cell, cal);
}

struct HeatRegistrar {
  HeatRegistrar() {
    registerWorkload({.name = "parallelfor_heat_j8_n2048", .run = &runHeatCell<0, 8>});
    registerWorkload({.name = "parallelfor_heat_j16_n2048", .run = &runHeatCell<0, 16>});
    registerWorkload({.name = "parallelfor_heat_j8_n4096", .run = &runHeatCell<1, 8>});
    registerWorkload({.name = "parallelfor_heat_j16_n4096", .run = &runHeatCell<1, 16>});
  }
};

const HeatRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
