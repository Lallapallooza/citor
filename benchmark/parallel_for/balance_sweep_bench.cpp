// Balance enum sweep on citor under Pareto-body parallelFor.
//
// Claim: Balance enum has measurable cost on citor under heavy-tailed body
// cost. Two rows -- Balance::StaticUniform vs Balance::DynamicChunked -- run
// the same Pareto-body workload as the pareto_body_for bench so the
// comparison surfaces what the runtime hint is actually buying.
//
// j={8, 16}.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "citor/always_assert.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

// Dynamic-chunked hint preset (workers race on the relaxed nextBlock counter).
// The static-uniform counterpart in this bench is just `citor::HintsDefaults`
// (with cancellation polls disabled to match the dispatch-floor measurement
// shape).
struct BalanceSweepStaticHints : citor::HintsDefaults {
  static constexpr bool cancellationChecks = false;
};

struct BalanceSweepDynamicHints : citor::HintsDefaults {
  static constexpr citor::Balance balance = citor::Balance::DynamicChunked;
  static constexpr bool cancellationChecks = false;
};

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 30;
constexpr std::size_t kWarmupIterations = 3;
constexpr std::size_t kN = 1'000'000;

constexpr double kAlpha = 1.16;
constexpr double kParetoXmNs = 540.0;
constexpr double kSpinCapNs = 200'000.0;

[[nodiscard]] inline double paretoDrawNs(double u) noexcept {
  const double denom = std::pow(1.0 - u, 1.0 / kAlpha);
  if (denom <= 0.0) {
    return kSpinCapNs;
  }
  const double draw = kParetoXmNs / denom;
  if (draw > kSpinCapNs) {
    return kSpinCapNs;
  }
  return draw;
}

inline void spinForNs(double targetNs,
                      const CyclesPerNanosecond &cal) noexcept {
  if (targetNs <= 0.0) {
    return;
  }
  const std::uint64_t cycles =
      cal.value > 0.0 ? static_cast<std::uint64_t>(targetNs * cal.value) : 0ULL;
  const std::uint64_t start = readCyclesStart();
  while ((readCyclesEnd() - start) < cycles) {
    // Tight spin; empty body.
  }
}

struct ParetoData {
  std::vector<std::int64_t> costNs;
  std::int64_t totalCostNs = 0;
};

[[nodiscard]] ParetoData buildData() {
  ParetoData d;
  d.costNs.assign(kN, 0);
  // Same fixed seed as pareto_body_for so the two benches see the same draws.
  std::mt19937_64 rng(0xC1701F1A2E2DULL);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  std::int64_t total = 0;
  for (std::size_t i = 0; i < kN; ++i) {
    const double draw = paretoDrawNs(uni(rng));
    const auto cost = static_cast<std::int64_t>(draw);
    d.costNs[i] = cost;
    total += cost;
  }
  d.totalCostNs = total;
  return d;
}

inline void paretoBlockBody(const ParetoData &d, std::size_t lo, std::size_t hi,
                            const CyclesPerNanosecond &cal,
                            std::atomic<std::int64_t> &sink) noexcept {
  std::int64_t local = 0;
  for (std::size_t i = lo; i < hi; ++i) {
    spinForNs(static_cast<double>(d.costNs[i]), cal);
    local += d.costNs[i];
  }
  sink.fetch_add(local, std::memory_order_relaxed);
}

template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name,
                                   const CyclesPerNanosecond &cal, RunFn run,
                                   std::int64_t referenceTotal) {
  // Peer mismatches log and skip the row; aborting would discard the rest.
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    const std::int64_t v = run();
    if (v != referenceTotal) [[unlikely]] {
      std::fprintf(stderr, "[%s] balance mismatch: expected=%lld actual=%lld\n",
                   name, static_cast<long long>(referenceTotal),
                   static_cast<long long>(v));
      return skippedRow(name);
    }
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    if (value != referenceTotal) [[unlikely]] {
      std::fprintf(stderr, "[%s] balance mismatch: expected=%lld actual=%lld\n",
                   name, static_cast<long long>(referenceTotal),
                   static_cast<long long>(value));
      return skippedRow(name);
    }
  }
  return finalizeRow(name, samples);
}

[[nodiscard]] BenchRow measureStatic(std::size_t participants,
                                     const ParetoData &d,
                                     const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  return measureLoop(
      "citor::ThreadPool[Balance::StaticUniform]", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        pool.parallelFor<BalanceSweepStaticHints>(
            std::size_t{0}, kN,
            [&d, &cal, &sink](std::size_t lo, std::size_t hi) {
              paretoBlockBody(d, lo, hi, cal, sink);
            });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}

[[nodiscard]] BenchRow measureDynamic(std::size_t participants,
                                      const ParetoData &d,
                                      const CyclesPerNanosecond &cal) {
  ThreadPool pool(participants);
  return measureLoop(
      "citor::ThreadPool[Balance::DynamicChunked]", cal,
      [&] {
        std::atomic<std::int64_t> sink{0};
        pool.parallelFor<BalanceSweepDynamicHints>(
            std::size_t{0}, kN,
            [&d, &cal, &sink](std::size_t lo, std::size_t hi) {
              paretoBlockBody(d, lo, hi, cal, sink);
            });
        return sink.load(std::memory_order_relaxed);
      },
      d.totalCostNs);
}

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"balance_sweep_pareto_"} + suffix;
  const ParetoData d = buildData();
  table.rows.push_back(measureStatic(participants, d, cal));
  table.rows.push_back(measureDynamic(participants, d, cal));
  return table;
}

template <std::size_t JParticipants>
BenchTable runCell(const CyclesPerNanosecond &cal) {
  static_assert(JParticipants == 8 || JParticipants == 16 ||
                    JParticipants == 32 || JParticipants == 48,
                "unsupported j-value");
  constexpr const char *jSuffix = []() -> const char * {
    if constexpr (JParticipants == 8) {
      return "j8_n1M";
    } else if constexpr (JParticipants == 16) {
      return "j16_n1M";
    } else if constexpr (JParticipants == 32) {
      return "j32_n1M";
    } else {
      return "j48_n1M";
    }
  }();
  if (!hasEnoughPhysicalCores(JParticipants)) {
    throw std::runtime_error("needs " + std::to_string(JParticipants) +
                             " physical cores");
  }
  return buildTable(JParticipants, jSuffix, cal);
}

struct BalanceSweepRegistrar {
  BalanceSweepRegistrar() {
    registerWorkload(
        {.name = "balance_sweep_pareto_j8_n1M", .run = &runCell<8>});
    registerWorkload(
        {.name = "balance_sweep_pareto_j16_n1M", .run = &runCell<16>});
    registerWorkload(
        {.name = "balance_sweep_pareto_j32_n1M", .run = &runCell<32>});
    registerWorkload(
        {.name = "balance_sweep_pareto_j48_n1M", .run = &runCell<48>});
  }
};

const BalanceSweepRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
