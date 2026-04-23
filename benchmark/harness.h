#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

#include "cycle_clock.h"

namespace citor::bench {

/// Sample budget shared by every cell that uses time-budget collection.
///
/// The harness used to fix the iteration count per cell, which over-sampled the
/// 1 ms-body cells and under-sampled the 1 us-body cells. Cells with a body cost
/// around the bimodal-noise floor (THP defrag, NUMA scan, IRQ migration) need
/// thousands of samples to converge a stable headline median; cells whose body
/// is itself in the millisecond range only need a few dozen. A wall-time budget
/// adapts both extremes in one knob.
struct SampleBudget {
  /// Wall-time spent collecting samples per (pool, cell) pair, in nanoseconds.
  /// 100 ms is the smallest budget that consistently dropped the bench's err%
  /// out of bimodal cliffs on this host's microarchitecture.
  std::uint64_t budgetNs = 100'000'000ULL;

  /// Hard cap on iteration count to bound short-body cells. Without the cap a
  /// 50 ns body would run ~2 M iters per pool per cell.
  std::size_t maxIters = 50'000;

  /// Minimum iter count so even sub-microsecond bodies have enough samples to
  /// compute a stable lower-quartile.
  std::size_t minIters = 32;

  /// Warmup iters dropped from the sample window. Sized to give the pool's
  /// spin-then-park budget, the page table, and the host's first-touch faults
  /// time to settle. A separate warmup count from the sample budget so very
  /// short cells can warm up cheaply while long cells skip the warmup early.
  std::size_t warmupIters = 64;

  /// Warmup cap, in nanoseconds, so warmup never dominates a long-body cell.
  std::uint64_t warmupBudgetNs = 20'000'000ULL;
};

/// Default sample budget used by every cell that does not override it.
inline constexpr SampleBudget kDefaultSampleBudget{};

/// Run |invoke| until either the wall-time budget or the iter cap is
///        exhausted, returning the per-iteration timings (in ns).
///
/// The caller supplies |invoke| as a noexcept callable that performs one
/// iteration's measurement bracket internally and returns the cycle delta. The
/// helper converts cycles to ns via |cal| and applies a warmup phase first.
///
/// Invoke   Callable returning a `std::uint64_t` cycle delta per call.
/// cal      Calibration constant for converting cycles to ns.
/// budget   Sample budget driving warmup and measurement loops.
/// invoke   Per-iteration measurement function.
/// Vector of per-iter ns measurements, sized to whatever the budget
///         produced. Always at least `budget.minIters` and at most
///         `budget.maxIters` entries.
template <class Invoke>
[[nodiscard]] std::vector<double> runUntilBudget(const CyclesPerNanosecond &cal,
                                                 const SampleBudget &budget, Invoke &&invoke) {
  // Warmup: fixed iter count or budget, whichever expires first. Discarded
  // samples are not retained.
  const auto warmupStart = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < budget.warmupIters; ++i) {
    (void)invoke();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - warmupStart).count();
    if (static_cast<std::uint64_t>(elapsedNs) >= budget.warmupBudgetNs) {
      break;
    }
  }

  std::vector<double> samples;
  samples.reserve(budget.minIters);
  const auto sampleStart = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < budget.maxIters; ++i) {
    const std::uint64_t deltaCycles = invoke();
    samples.push_back(cyclesToNs(deltaCycles, cal));
    if (samples.size() >= budget.minIters) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsedNs =
          std::chrono::duration_cast<std::chrono::nanoseconds>(now - sampleStart).count();
      if (static_cast<std::uint64_t>(elapsedNs) >= budget.budgetNs) {
        break;
      }
    }
  }
  return samples;
}

} // namespace citor::bench

namespace citor::bench {

/// Honest-bench checklist gate result. `Pass` means the gate is observed to
/// hold; `Fail` means it is observed not to hold; `Unknown` means the host
/// does not expose the data needed to decide.
enum class GateStatus : std::uint8_t { Pass, Fail, Unknown };

/// One row of the honest-bench checklist printed at the top of every run.
struct GateResult {
  std::string_view name;
  GateStatus status;
  std::string detail;
};

[[nodiscard]] inline std::string readFirstLine(const std::string &path) {
  std::ifstream f(path);
  std::string line;
  if (f.is_open()) {
    std::getline(f, line);
  }
  return line;
}

/// Probe `/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`. Pass when
/// the governor is "performance"; Fail otherwise; Unknown when the file is
/// not readable (e.g. virtualized host without cpufreq exposed).
[[nodiscard]] inline GateResult probeGovernor() {
#if defined(__linux__)
  const std::string g = readFirstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  if (g.empty()) {
    return {.name = "governor=performance",
            .status = GateStatus::Unknown,
            .detail = "cpufreq scaling_governor not readable"};
  }
  if (g == "performance") {
    return {.name = "governor=performance", .status = GateStatus::Pass, .detail = g};
  }
  return {.name = "governor=performance", .status = GateStatus::Fail, .detail = g};
#else
  return {.name = "governor=performance",
          .status = GateStatus::Unknown,
          .detail = "non-linux host"};
#endif
}

/// Probe `/sys/devices/system/cpu/cpufreq/boost` (AMD) or
/// `/sys/devices/system/cpu/intel_pstate/no_turbo` (Intel pstate). Pass when
/// boost is observed off.
[[nodiscard]] inline GateResult probeBoost() {
#if defined(__linux__)
  const std::string amdBoost = readFirstLine("/sys/devices/system/cpu/cpufreq/boost");
  if (!amdBoost.empty()) {
    if (amdBoost == "0") {
      return {.name = "boost=off", .status = GateStatus::Pass, .detail = "amd boost=0"};
    }
    return {.name = "boost=off", .status = GateStatus::Fail, .detail = "amd boost=" + amdBoost};
  }
  const std::string intelNoTurbo =
      readFirstLine("/sys/devices/system/cpu/intel_pstate/no_turbo");
  if (!intelNoTurbo.empty()) {
    if (intelNoTurbo == "1") {
      return {.name = "boost=off", .status = GateStatus::Pass, .detail = "intel no_turbo=1"};
    }
    return {.name = "boost=off",
            .status = GateStatus::Fail,
            .detail = "intel no_turbo=" + intelNoTurbo};
  }
  return {.name = "boost=off", .status = GateStatus::Unknown, .detail = "boost knob not exposed"};
#else
  return {.name = "boost=off", .status = GateStatus::Unknown, .detail = "non-linux host"};
#endif
}

/// Probe `/sys/devices/system/cpu/smt/active`. Pass when SMT is observed off.
[[nodiscard]] inline GateResult probeSmt() {
#if defined(__linux__)
  const std::string smt = readFirstLine("/sys/devices/system/cpu/smt/active");
  if (smt.empty()) {
    return {.name = "smt=off", .status = GateStatus::Unknown, .detail = "smt/active not readable"};
  }
  if (smt == "0") {
    return {.name = "smt=off", .status = GateStatus::Pass, .detail = "smt/active=0"};
  }
  return {.name = "smt=off", .status = GateStatus::Fail, .detail = "smt/active=" + smt};
#else
  return {.name = "smt=off", .status = GateStatus::Unknown, .detail = "non-linux host"};
#endif
}

/// Probe `/proc/sys/kernel/randomize_va_space`. Pass when ASLR is observed
/// off. ASLR introduces page-layout noise that has been measured larger than
/// many compiler optimizations on small workloads (Mytkowicz et al. ASPLOS
/// 2009).
[[nodiscard]] inline GateResult probeAslr() {
#if defined(__linux__)
  const std::string aslr = readFirstLine("/proc/sys/kernel/randomize_va_space");
  if (aslr.empty()) {
    return {.name = "aslr=off",
            .status = GateStatus::Unknown,
            .detail = "randomize_va_space not readable"};
  }
  if (aslr == "0") {
    return {.name = "aslr=off", .status = GateStatus::Pass, .detail = "randomize_va_space=0"};
  }
  return {.name = "aslr=off", .status = GateStatus::Fail, .detail = "randomize_va_space=" + aslr};
#else
  return {.name = "aslr=off", .status = GateStatus::Unknown, .detail = "non-linux host"};
#endif
}

/// Detect whether the host is running under TSan (via `__has_feature` on
/// clang or via the `__SANITIZE_THREAD__` macro on gcc). TSan's instrumentation
/// makes published numbers meaningless; the gate fails when sanitization is
/// active.
[[nodiscard]] inline GateResult probeTsanOff() {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
  return {.name = "tsan=off", .status = GateStatus::Fail, .detail = "ThreadSanitizer active"};
#endif
#endif
#if defined(__SANITIZE_THREAD__)
  return {.name = "tsan=off", .status = GateStatus::Fail, .detail = "ThreadSanitizer active"};
#else
  return {.name = "tsan=off", .status = GateStatus::Pass, .detail = "no sanitizer"};
#endif
}

/// Read `VmHWM` (peak resident set size) from `/proc/self/status`, in KiB.
/// Returns 0 when the field cannot be read.
[[nodiscard]] inline std::uint64_t readPeakRssKb() {
#if defined(__linux__)
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    constexpr std::string_view kPrefix = "VmHWM:";
    if (line.starts_with(kPrefix)) {
      // Format: "VmHWM:\t  12345 kB"
      std::uint64_t kb = 0;
      for (char c : line) {
        if (c >= '0' && c <= '9') {
          kb = (kb * 10U) + static_cast<unsigned>(c - '0');
        } else if (kb > 0U) {
          break;
        }
      }
      return kb;
    }
  }
  return 0U;
#else
  return 0U;
#endif
}

/// Read `getrusage(RUSAGE_SELF)`'s system + user time, in microseconds. Used
/// by idle-burn benches to confirm that an "idle" pool consumes no CPU.
struct RusageSample {
  std::uint64_t userUs = 0;
  std::uint64_t systemUs = 0;
};

[[nodiscard]] inline RusageSample readRusage() {
  RusageSample s;
#if defined(__linux__)
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    s.userUs = (static_cast<std::uint64_t>(ru.ru_utime.tv_sec) * 1'000'000ULL) +
               static_cast<std::uint64_t>(ru.ru_utime.tv_usec);
    s.systemUs = (static_cast<std::uint64_t>(ru.ru_stime.tv_sec) * 1'000'000ULL) +
                 static_cast<std::uint64_t>(ru.ru_stime.tv_usec);
  }
#endif
  return s;
}

/// Format and print the honest-bench checklist banner at the top of a run.
inline void printChecklist(std::ostream &out) {
  const std::array<GateResult, 5> gates{{
      probeGovernor(),
      probeBoost(),
      probeSmt(),
      probeAslr(),
      probeTsanOff(),
  }};
  out << "[CHECKLIST] honest-bench gates:\n";
  for (const GateResult &g : gates) {
    const char *label = "UNKNOWN";
    if (g.status == GateStatus::Pass) {
      label = "PASS";
    } else if (g.status == GateStatus::Fail) {
      label = "FAIL";
    }
    out << "[CHECKLIST] " << label << "  " << g.name;
    if (!g.detail.empty()) {
      out << "  (" << g.detail << ")";
    }
    out << '\n';
  }
}

} // namespace citor::bench
