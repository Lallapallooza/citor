#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#if defined(__linux__)
#include <sys/utsname.h>
#endif

#include "bench_format.h"
#include "cycle_clock.h"
#include "harness.h"

namespace citor::bench {

/// Single JSON document written by `parallel_bench --export <path>`.
///
/// The format is Google-Benchmark-shaped:
/// one document per bench invocation with three top-level keys --
/// `schema_version`, `context`, and `samples`. The `context` block carries host
/// / build / governor / TSC provenance written once per run. Each entry in
/// `samples` is one timed iteration: `(workload, pool, rep, cycles, ns)`.
///
/// Cycle counts are emitted as decimal strings to dodge the JSON int64
/// precision trap (`__rdtsc()` is `uint64_t`; JSON has no native int64 and
/// common parsers silently round through `double`). Nanoseconds are emitted as
/// native JSON numbers because the realistic range (<= a few seconds per
/// iteration) is comfortably inside `2^53`.
///
/// The exporter never inspects measurement timing; it consumes whatever
/// `finalizeRow` already populated in `BenchRow::rawSamplesNs`. When the
/// `--export` flag is absent, `rawSampleExportEnabled()` is false, the per-row
/// sample snapshot is never copied, and this header's writer is never called.
struct ExportContext {
  std::string tool;
  std::string citorVersion;
  std::string citorCommit;
  bool citorDirty = false;
  std::string datetimeUtc;
  std::string hostname;
  std::string kernel;
  std::string cpuModel;
  unsigned cpuLogical = 0;
  std::string compiler;
  std::string compilerVersion;
  std::string buildType;
  bool avx2 = false;
  double tscCyclesPerNs = 0.0;
  std::string tasksetCpus;
  std::vector<GateResult> checklist;
};

namespace export_detail {

/// Read the first non-empty line of a file; return empty string on failure.
[[nodiscard]] inline std::string readFirstLineOrEmpty(const std::string &path) {
  std::ifstream f(path);
  std::string line;
  if (f.is_open()) {
    std::getline(f, line);
  }
  return line;
}

/// Read the value of a `KEY=VALUE` line whose key matches |key|. Strips
/// surrounding double quotes from the value. Returns empty string when the
/// file or the key is missing.
[[nodiscard]] inline std::string readKeyValue(const std::string &path, const std::string &key) {
  std::ifstream f(path);
  std::string line;
  while (f.is_open() && std::getline(f, line)) {
    if (line.size() > key.size() + 1U && line.compare(0U, key.size(), key) == 0 &&
        line[key.size()] == '=') {
      std::string value = line.substr(key.size() + 1U);
      if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
        value = value.substr(1U, value.size() - 2U);
      }
      return value;
    }
  }
  return std::string{};
}

/// Run a shell command, return its stdout's first line trimmed; empty on
/// failure. Used for `git describe` / `git rev-parse` / `git status`. The
/// command is fixed at compile time -- no user-supplied substrings reach
/// `popen`.
[[nodiscard]] inline std::string runOneLine(const char *cmd) {
  std::string out;
#if defined(__linux__)
  // NOLINTNEXTLINE(cert-env33-c) -- fixed-string command; no user input.
  FILE *pipe = ::popen(cmd, "r");
  if (pipe == nullptr) {
    return out;
  }
  std::array<char, 256> buf{};
  if (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out.assign(buf.data());
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
      out.pop_back();
    }
  }
  (void)::pclose(pipe);
#else
  (void)cmd;
#endif
  return out;
}

[[nodiscard]] inline std::string isoUtcNow() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto t = clock::to_time_t(now);
  std::tm tmv{};
#if defined(__linux__) || defined(__APPLE__)
  gmtime_r(&t, &tmv);
#else
  tmv = *std::gmtime(&t);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tmv, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

[[nodiscard]] inline std::string detectCompiler() {
#if defined(__clang__)
  return "clang";
#elif defined(__GNUC__)
  return "gcc";
#elif defined(_MSC_VER)
  return "msvc";
#else
  return "unknown";
#endif
}

[[nodiscard]] inline std::string detectCompilerVersion() {
#if defined(__clang_version__)
  return std::string{__clang_version__};
#elif defined(__VERSION__)
  return std::string{__VERSION__};
#else
  return std::string{};
#endif
}

[[nodiscard]] inline std::string detectBuildType() {
#if defined(NDEBUG)
  return "Release";
#else
  return "Debug";
#endif
}

[[nodiscard]] inline std::string readTasksetCpus() {
#if defined(__linux__)
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
    return std::string{};
  }
  std::vector<int> cpus;
  for (int i = 0; i < CPU_SETSIZE; ++i) {
    if (CPU_ISSET(i, &mask)) {
      cpus.push_back(i);
    }
  }
  if (cpus.empty()) {
    return std::string{};
  }
  // Compress to comma-separated ranges (e.g. "0-7,16-23").
  std::ostringstream oss;
  std::size_t i = 0;
  while (i < cpus.size()) {
    std::size_t j = i;
    while (j + 1U < cpus.size() && cpus[j + 1U] == cpus[j] + 1) {
      ++j;
    }
    if (i != 0U) {
      oss << ',';
    }
    if (i == j) {
      oss << cpus[i];
    } else {
      oss << cpus[i] << '-' << cpus[j];
    }
    i = j + 1U;
  }
  return oss.str();
#else
  return std::string{};
#endif
}

[[nodiscard]] inline std::string readCpuModel() {
#if defined(__linux__)
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (f.is_open() && std::getline(f, line)) {
    constexpr std::string_view kPrefix = "model name";
    if (line.compare(0U, kPrefix.size(), kPrefix) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos) {
        std::string value = line.substr(colon + 1U);
        while (!value.empty() && value.front() == ' ') {
          value.erase(value.begin());
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\n')) {
          value.pop_back();
        }
        return value;
      }
    }
  }
#endif
  return std::string{};
}

[[nodiscard]] inline std::string readKernel() {
#if defined(__linux__)
  utsname u{};
  if (uname(&u) == 0) {
    std::string s{u.sysname};
    s += ' ';
    s += u.release;
    return s;
  }
#endif
  return std::string{};
}

[[nodiscard]] inline std::string readHostname() {
#if defined(__linux__)
  std::array<char, 256> buf{};
  if (gethostname(buf.data(), buf.size() - 1U) == 0) {
    return std::string{buf.data()};
  }
#endif
  return std::string{};
}

inline void escapeJsonInto(std::string_view in, std::string &out) {
  out.push_back('"');
  for (const char c : in) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20U) {
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
        out += esc;
      } else {
        out.push_back(c);
      }
    }
  }
  out.push_back('"');
}

[[nodiscard]] inline std::string escapeJson(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 2U);
  escapeJsonInto(in, out);
  return out;
}

inline void writeNumber(std::ostream &out, double v) {
  if (!std::isfinite(v)) {
    // JSON has no representation for NaN/Inf. Substitute null so downstream
    // parsers don't crash; the row should be inspected if this fires.
    out << "null";
    return;
  }
  out << std::setprecision(17) << v;
}

[[nodiscard]] inline std::string statusLabel(GateStatus s) {
  switch (s) {
  case GateStatus::Pass:
    return "PASS";
  case GateStatus::Fail:
    return "FAIL";
  case GateStatus::Unknown:
    break;
  }
  return "UNKNOWN";
}

} // namespace export_detail

/// Probe and assemble the per-run provenance block.
///
/// cal Cycles-per-nanosecond calibration measured by `bench_main.cpp`.
///            Stashed verbatim into the JSON `context.tsc_cycles_per_ns` so
///            downstream tools can reconstruct the wall-clock conversion.
/// Populated `ExportContext`. Fields that fail to probe are left empty;
///         the writer emits empty strings as JSON `""`.
[[nodiscard]] inline ExportContext probeContext(const CyclesPerNanosecond &cal) {
  ExportContext c;
  c.tool = "citor::parallel_bench";
  c.citorVersion = export_detail::runOneLine("git describe --always --dirty 2>/dev/null");
  c.citorCommit = export_detail::runOneLine("git rev-parse HEAD 2>/dev/null");
  c.citorDirty = !export_detail::runOneLine("git status --porcelain 2>/dev/null").empty();
  c.datetimeUtc = export_detail::isoUtcNow();
  c.hostname = export_detail::readHostname();
  c.kernel = export_detail::readKernel();
  c.cpuModel = export_detail::readCpuModel();
  c.cpuLogical = std::thread::hardware_concurrency();
  c.compiler = export_detail::detectCompiler();
  c.compilerVersion = export_detail::detectCompilerVersion();
  c.buildType = export_detail::detectBuildType();
#ifdef CITOR_USE_AVX2
  c.avx2 = true;
#endif
  c.tscCyclesPerNs = cal.value;
  c.tasksetCpus = export_detail::readTasksetCpus();
  c.checklist = {probeGovernor(),  probeBoost(),    probeSmt(),
                 probeAslr(),      probeTsanOff(), probeLibompBlocktime()};
  return c;
}

/// Write the JSON document to |path| using the recommended schema.
///
/// Returns true on success, false on stream-open / write failure. The function
/// never throws -- a write error is reported to stderr and the bench continues.
///
/// path        Destination file path. Truncated and overwritten.
/// context     Provenance block from `probeContext`.
/// tables      All tables produced by the bench loop. Rows whose
///                    `rawSamplesNs` is empty contribute zero records (e.g.
///                    skipped engines or workloads that never collected
///                    per-iteration samples).
/// pretty      When true, emit indented JSON. Default false (compact).
inline bool writeJsonExport(const std::string &path, const ExportContext &context,
                            const std::vector<BenchTable> &tables, bool pretty = false) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    std::cerr << "parallel_bench: failed to open --export path '" << path << "' for write\n";
    return false;
  }
  const std::string nl = pretty ? "\n" : "";
  const std::string indent1 = pretty ? "  " : "";
  const std::string indent2 = pretty ? "    " : "";
  const std::string indent3 = pretty ? "      " : "";
  const std::string sep = pretty ? ", " : ",";

  out << '{' << nl;
  out << indent1 << "\"schema_version\": 1," << nl;
  out << indent1 << "\"context\": {" << nl;
  out << indent2 << "\"tool\": " << export_detail::escapeJson(context.tool) << ',' << nl;
  out << indent2 << "\"citor_version\": " << export_detail::escapeJson(context.citorVersion) << ','
      << nl;
  out << indent2 << "\"citor_commit\": " << export_detail::escapeJson(context.citorCommit) << ','
      << nl;
  out << indent2 << "\"citor_dirty\": " << (context.citorDirty ? "true" : "false") << ',' << nl;
  out << indent2 << "\"datetime_utc\": " << export_detail::escapeJson(context.datetimeUtc) << ','
      << nl;
  out << indent2 << "\"hostname\": " << export_detail::escapeJson(context.hostname) << ',' << nl;
  out << indent2 << "\"kernel\": " << export_detail::escapeJson(context.kernel) << ',' << nl;
  out << indent2 << "\"cpu_model\": " << export_detail::escapeJson(context.cpuModel) << ',' << nl;
  out << indent2 << "\"cpu_logical\": " << context.cpuLogical << ',' << nl;
  out << indent2 << "\"compiler\": " << export_detail::escapeJson(context.compiler) << ',' << nl;
  out << indent2 << "\"compiler_version\": " << export_detail::escapeJson(context.compilerVersion)
      << ',' << nl;
  out << indent2 << "\"build_type\": " << export_detail::escapeJson(context.buildType) << ',' << nl;
  out << indent2 << "\"avx2\": " << (context.avx2 ? "true" : "false") << ',' << nl;
  out << indent2 << "\"tsc_cycles_per_ns\": ";
  export_detail::writeNumber(out, context.tscCyclesPerNs);
  out << ',' << nl;
  out << indent2 << "\"taskset_cpus\": " << export_detail::escapeJson(context.tasksetCpus) << ','
      << nl;
  out << indent2 << "\"checklist\": [" << nl;
  for (std::size_t i = 0; i < context.checklist.size(); ++i) {
    const auto &g = context.checklist[i];
    out << indent3 << "{\"name\": " << export_detail::escapeJson(g.name)
        << ", \"status\": " << export_detail::escapeJson(export_detail::statusLabel(g.status))
        << ", \"detail\": " << export_detail::escapeJson(g.detail) << '}';
    if (i + 1U < context.checklist.size()) {
      out << ',';
    }
    out << nl;
  }
  out << indent2 << "]" << nl;
  out << indent1 << "}," << nl;

  out << indent1 << "\"samples\": [" << nl;
  bool firstRecord = true;
  // Compute total cycle delta from ns + calibration. We don't have the raw
  // cycle count after `cyclesToNs` conversion; the deterministic re-derivation
  // is `cycles = round(ns * cyclesPerNs)`. This is the same conversion the
  // bench's measurement loop uses in reverse and preserves the user's ability
  // to recover cycle stamps from the JSON without storing them twice.
  const double cyclesPerNs = context.tscCyclesPerNs;
  for (const auto &table : tables) {
    for (const auto &row : table.rows) {
      if (row.rawSamplesNs.empty()) {
        continue;
      }
      for (std::size_t rep = 0; rep < row.rawSamplesNs.size(); ++rep) {
        if (!firstRecord) {
          out << ',' << nl;
        }
        firstRecord = false;
        const double ns = row.rawSamplesNs[rep];
        const double cyclesD = ns * cyclesPerNs;
        // Cast to uint64_t; emit as decimal string.
        const auto cycles = static_cast<std::uint64_t>(cyclesD < 0.0 ? 0.0 : cyclesD);
        out << indent2 << "{\"workload\": " << export_detail::escapeJson(table.workload) << sep
            << "\"pool\": " << export_detail::escapeJson(row.name) << sep << "\"rep\": " << rep
            << sep << "\"cycles\": \"" << cycles << "\"" << sep << "\"ns\": ";
        export_detail::writeNumber(out, ns);
        out << '}';
      }
    }
  }
  if (!firstRecord) {
    out << nl;
  }
  out << indent1 << "]" << nl;
  out << '}' << nl;
  return out.good();
}

} // namespace citor::bench
