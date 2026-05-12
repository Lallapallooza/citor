// 2D 5-point Jacobi heat-diffusion workload for the comparative pool bench.
//
// `u_new[i,j] = 0.25 * (u[i-1,j] + u[i+1,j] + u[i,j-1] + u[i,j+1])` over an
// `N x N` grid (interior nodes only; boundary held at the initial value).
// `T = 200` timesteps; each timestep is one `parallelFor` over rows. The
// chain across timesteps is the implicit outer loop -- reusing the
// `parallelChain` primitive would require all stages to share the same
// dispatch shape, which the heat kernel does not (the row-block partition
// is constant across stages but the per-stage dispatch is also natural via
// the outer loop and avoids a chain-rendezvous round per timestep that
// some competitor adapters cannot honor).
//
// All 8 competitor pools render natively via
// `CompetitorTraits<P>::parallelFor`.
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

#include "aligned_alloc.h"
#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 12;
constexpr std::size_t kWarmupIterations = 2;
constexpr std::size_t kTimesteps = 200;

/// 64-byte aligned `double[]` deleter; pairs with `std::unique_ptr`.
struct AlignedDoubleDeleter {
  void operator()(double *p) const noexcept { alignedFree(p); }
};

using AlignedDoubleBuffer = std::unique_ptr<double, AlignedDoubleDeleter>;

AlignedDoubleBuffer allocateAlignedDoubles(std::size_t count) {
  const std::size_t bytes = ((count * sizeof(double) + 63U) / 64U) * 64U;
  void *raw = alignedAlloc(bytes, 64U);
  if (raw == nullptr) {
    std::abort();
  }
  return AlignedDoubleBuffer{static_cast<double *>(raw)};
}

/// Deterministic LCG-seeded fill. Same seed -> identical initial grid for
/// every (pool, n) cell so the checksum invariant is well-defined.
void deterministicFill(double *p, std::size_t count,
                       std::uint32_t seed) noexcept {
  std::uint32_t state = seed;
  for (std::size_t i = 0; i < count; ++i) {
    state = (state * 1664525U) + 1013904223U;
    p[i] = static_cast<double>(state >> 12U) * (1.0 / 1048576.0);
  }
}

/// Sequential 5-point Jacobi reference; used to compute the correctness
/// checksum the parallel run must match before any timing iteration.
[[nodiscard]] double seqJacobi(std::size_t n, std::size_t timesteps,
                               std::uint32_t seed) {
  const std::size_t elems = n * n;
  AlignedDoubleBuffer u = allocateAlignedDoubles(elems);
  AlignedDoubleBuffer uNext = allocateAlignedDoubles(elems);
  deterministicFill(u.get(), elems, seed);
  // Boundary: leave the initial values in place; only interior is updated.
  std::copy(u.get(), u.get() + elems, uNext.get());

  for (std::size_t t = 0; t < timesteps; ++t) {
    for (std::size_t i = 1; i < n - 1U; ++i) {
      for (std::size_t j = 1; j < n - 1U; ++j) {
        uNext.get()[(i * n) + j] =
            0.25 * (u.get()[((i - 1U) * n) + j] + u.get()[((i + 1U) * n) + j] +
                    u.get()[(i * n) + (j - 1U)] + u.get()[(i * n) + (j + 1U)]);
      }
    }
    std::swap(u, uNext);
  }
  // Checksum: sum of interior cells.
  double sum = 0.0;
  for (std::size_t i = 1; i < n - 1U; ++i) {
    for (std::size_t j = 1; j < n - 1U; ++j) {
      sum += u.get()[(i * n) + j];
    }
  }
  return sum;
}

/// Per-timestep row-block body. `rowFirst..rowLast` are interior row
/// indices (both clipped to `[1, n-1)` upstream).
inline void jacobiRowBlock(std::size_t rowFirst, std::size_t rowLast,
                           std::size_t n, const double *uIn,
                           double *uOut) noexcept {
  for (std::size_t i = rowFirst; i < rowLast; ++i) {
    for (std::size_t j = 1; j < n - 1U; ++j) {
      uOut[(i * n) + j] =
          0.25 * (uIn[((i - 1U) * n) + j] + uIn[((i + 1U) * n) + j] +
                  uIn[(i * n) + (j - 1U)] + uIn[(i * n) + (j + 1U)]);
    }
  }
}

/// Per-pool sample function. Uses the trait's `parallelFor` for citor + the
/// 4 native-primitive competitors (oneTBB, Taskflow, Eigen, OpenMP) and the
/// 4 future-pool shims (BS, dp, task, riften) configured in
/// `competitor_traits.h`. Riften's `parallelFor` shim takes a participant
/// count argument so its overload signature differs; we wrap that case.
template <class PoolT, class Dispatch>
[[nodiscard]] BenchRow
measureHeatWith(const char *displayName, std::size_t participants,
                std::size_t n, const CyclesPerNanosecond &cal,
                double referenceChecksum, Dispatch dispatch) {
  if (!engineEnabled(displayName)) {
    BenchRow row{};
    row.name = displayName;
    row.skipped = true;
    return row;
  }
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
      const auto body = [uIn, uOut, n](std::size_t lo,
                                       std::size_t hi) noexcept {
        const std::size_t rLo = std::max<std::size_t>(lo, 1U);
        const std::size_t rHi = hi >= n - 1U ? n - 1U : hi;
        if (rLo < rHi) {
          jacobiRowBlock(rLo, rHi, n, uIn, uOut);
        }
      };
      dispatch(*pool, std::size_t{1}, n - 1U, participants, body);
      std::swap(u, uNext);
    }
    double sum = 0.0;
    for (std::size_t i = 1; i < n - 1U; ++i) {
      for (std::size_t j = 1; j < n - 1U; ++j) {
        sum += u.get()[(i * n) + j];
      }
    }
    return sum;
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    const double sum = runOnce();
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

  return finalizeRow(displayName, samples);
}

template <class PoolT>
[[nodiscard]] BenchRow measureHeat(std::size_t participants, std::size_t n,
                                   const CyclesPerNanosecond &cal,
                                   double referenceChecksum) {
  using Traits = CompetitorTraits<PoolT>;
  return measureHeatWith<PoolT>(
      Traits::name, participants, n, cal, referenceChecksum,
      [](PoolT &pool, std::size_t first, std::size_t last, std::size_t p,
         auto fn) { Traits::parallelFor(pool, first, last, p, fn); });
}

template <class HintsT>
[[nodiscard]] BenchRow
measureCitorHeatWithHint(const char *displayName, std::size_t participants,
                         std::size_t n, const CyclesPerNanosecond &cal,
                         double referenceChecksum) {
  return measureHeatWith<citor::ThreadPool>(
      displayName, participants, n, cal, referenceChecksum,
      [](citor::ThreadPool &pool, std::size_t first, std::size_t last,
         std::size_t /*p*/,
         auto fn) { pool.parallelFor<HintsT>(first, last, fn); });
}

struct HeatCell {
  std::size_t n;
  const char *suffix;
};

constexpr std::array<HeatCell, 2> kCells{{
    {.n = 2048, .suffix = "n2048"},
    {.n = 4096, .suffix = "n4096"},
}};

BenchTable buildTable(std::size_t participants, HeatCell cell,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"runplex_heat_j"} +
                   std::to_string(participants) + "_" + cell.suffix;

  // Sequential reference checksum is computed once per cell and reused
  // across pool rows.
  const double reference = seqJacobi(cell.n, kTimesteps, /*seed=*/0xC1701U);

  table.rows.push_back(measureCitorHeatWithHint<citor::StaticHints>(
      "citor::ThreadPool[Static]", participants, cell.n, cal, reference));
  table.rows.push_back(measureCitorHeatWithHint<citor::DynamicHints>(
      "citor::ThreadPool[Dynamic]", participants, cell.n, cal, reference));
  table.rows.push_back(
      measureHeat<BS::light_thread_pool>(participants, cell.n, cal, reference));
  table.rows.push_back(
      measureHeat<dp::thread_pool<>>(participants, cell.n, cal, reference));
  table.rows.push_back(measureHeat<::task_thread_pool::task_thread_pool>(
      participants, cell.n, cal, reference));
  table.rows.push_back(
      measureHeat<riften::Thiefpool>(participants, cell.n, cal, reference));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(
      measureHeat<::tbb::task_arena>(participants, cell.n, cal, reference));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(
      measureHeat<::tf::Executor>(participants, cell.n, cal, reference));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(
      measureHeat<::Eigen::ThreadPool>(participants, cell.n, cal, reference));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(
      measureHeat<OpenMpRunner>(participants, cell.n, cal, reference));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(
      measureHeat<hmthrp::ThreadPool>(participants, cell.n, cal, reference));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(
      measureHeat<dispenso::ThreadPool>(participants, cell.n, cal, reference));
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
    registerWorkload(
        {.name = "runplex_heat_j8_n2048", .run = &runHeatCell<0, 8>});
    registerWorkload(
        {.name = "runplex_heat_j16_n2048", .run = &runHeatCell<0, 16>});
    registerWorkload(
        {.name = "runplex_heat_j8_n4096", .run = &runHeatCell<1, 8>});
    registerWorkload(
        {.name = "runplex_heat_j16_n4096", .run = &runHeatCell<1, 16>});
  }
};

const HeatRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
