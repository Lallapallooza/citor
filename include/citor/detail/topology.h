#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace citor::detail {

#if defined(_WIN32)

/// Walk every `SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX` record for
/// `rel` and pass each to `f`. Returns `true` only when the entire
/// buffer was walked. Centralises the probe-size / allocate / fetch /
/// variable-stride scan that `RelationProcessorCore` and `RelationCache` share.
template <class F>
inline bool walkLogicalProcessorInfoEx(LOGICAL_PROCESSOR_RELATIONSHIP rel,
                                       F &&f) {
  DWORD length = 0;
  // First call is expected to fail with ERROR_INSUFFICIENT_BUFFER and set the
  // required size in `length`. Any other failure shape (rel not supported,
  // truncated query) means we cannot probe topology and the caller should fall
  // back.
  if (::GetLogicalProcessorInformationEx(rel, nullptr, &length) != FALSE) {
    return false;
  }
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return false;
  }
  std::vector<unsigned char> buffer(length);
  if (::GetLogicalProcessorInformationEx(
          rel,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
              buffer.data()),
          &length) == FALSE) {
    return false;
  }
  unsigned char *p = buffer.data();
  unsigned char *const end = p + length;
  while (p < end) {
    auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
    if (info->Size == 0U || p + info->Size > end) {
      return false;
    }
    f(*info);
    p += info->Size;
  }
  return true;
}

/// Lift a `GROUP_AFFINITY` into the flat logical-CPU-id space. Single
/// processor group only; multi-group hosts (>64 logical CPUs) require
/// `SetThreadGroupAffinity`, which the pool does not yet emit.
inline std::vector<std::uint32_t>
expandGroupAffinity(const GROUP_AFFINITY &ga) {
  std::vector<std::uint32_t> result;
  KAFFINITY mask = ga.Mask;
  const std::uint32_t base = static_cast<std::uint32_t>(ga.Group) * 64U;
  for (std::uint32_t bit = 0; mask != 0U; ++bit, mask >>= 1) {
    if ((mask & 1U) != 0U) {
      result.push_back(base + bit);
    }
  }
  return result;
}

#endif // _WIN32

/// Logical view of the host's CPU topology used for affinity decisions.
///
/// Constructed once via `detectTopology()` at pool startup; never queried on
/// the hot path. The pool uses `physicalCores` for one-worker-per-physical-core
/// pinning, `ccdGroups` and `ccdOfCpu` for CCD-aware victim selection, and the
/// counts for sizing the workers vector when no explicit participant count is
/// supplied. On hosts where sysfs is unavailable the constructor falls back to
/// `std::thread::hardware_concurrency()` and treats every logical CPU as its
/// own physical core inside one synthetic CCD.
struct Topology {
  /// Process-affinity-filtered list of one logical CPU per physical core (the
  /// pinning targets).
  std::vector<std::uint32_t> physicalCores;

  /// Lists of physical-core CPU ids grouped by shared L3 cache (CCD on Zen,
  /// cluster on big.LITTLE).
  std::vector<std::vector<std::uint32_t>> ccdGroups;

  /// CCD index for each logical CPU id; `ccdOfCpu[id]` indexes `ccdGroups`.
  std::vector<std::uint32_t> ccdOfCpu;

  /// L3 cache size in KiB for each CCD; `l3KibOfCcd[i]` corresponds to
  /// `ccdGroups[i]`. Read from
  /// `/sys/devices/system/cpu/cpuN/cache/index3/size` once per probe; zero when
  /// sysfs is absent.
  std::vector<std::uint64_t> l3KibOfCcd;

  /// Per-core L2 cache size in KiB, sampled from
  /// `cache/index2/size` of the first physical-core CPU id once per probe.
  /// Per-core L2 is uniform on every supported microarchitecture today,
  /// so one sample suffices; heterogeneous chips will need a per-cluster
  /// probe later. Used by primitives that pick tile sizes from the
  /// runtime cache hierarchy instead of hardcoded constants. Zero when
  /// sysfs is absent; primitives fall back to a conservative default.
  std::uint64_t l2KibPerCore = 0;

  /// Index into `ccdGroups` of the preferred CCD for small pools that fit in a
  /// single L3. Picks the largest L3 (= 3D V-Cache CCD on V-Cache parts, where
  /// one CCD has a stacked SRAM die); breaks ties by lowest index so the choice
  /// is deterministic across runs.
  std::uint32_t preferredCcd = 0;

  /// Total logical CPU count reported by the OS.
  std::uint32_t logicalCount = 0;

  /// Number of physical cores in the process affinity mask.
  std::uint32_t physicalCount = 0;

  /// Number of distinct CCDs (or shared-L3 groups).
  std::uint32_t ccdCount = 0;

  /// SMT sibling of each logical CPU id, or `UINT32_MAX` when the core
  /// is single-threaded or the OS did not report siblings. SMT4 silicon
  /// records the first non-self entry deterministically. The placement
  /// rule reads this to route slot 1 onto the producer's SMT sibling.
  std::vector<std::uint32_t> smtSiblingOfCpu;
};

/// Read a sysfs cache size string like "32768K" or "96M" and convert to KiB.
/// Returns 0 on parse failure or missing file so the caller can fall back to
/// "unknown size".
inline std::uint64_t readCacheSizeKib(const std::string &path) noexcept {
  std::ifstream in(path);
  if (!in.is_open()) {
    return 0U;
  }
  std::string token;
  if (!(in >> token) || token.empty()) {
    return 0U;
  }
  std::uint64_t value = 0U;
  std::size_t i = 0;
  while (i < token.size() &&
         std::isdigit(static_cast<unsigned char>(token[i])) != 0) {
    value = (value * 10U) + static_cast<std::uint64_t>(token[i] - '0');
    ++i;
  }
  if (i == 0U) {
    return 0U;
  }
  if (i < token.size()) {
    const char unit = token[i];
    if (unit == 'M' || unit == 'm') {
      value *= 1024U;
    } else if (unit == 'G' || unit == 'g') {
      value *= std::uint64_t{1024} * 1024U;
    }
    // 'K'/'k' or unknown unit: leave value as-is (sysfs convention is KiB by
    // default).
  }
  return value;
}

/// Read a comma-separated CPU list (e.g. `0-7,16-23`) from a sysfs file.
///
/// Used for both `thread_siblings_list` (SMT detection) and
/// `cache/index3/shared_cpu_list` (CCD detection). Returns an empty vector when
/// the file is absent so the caller can fall back to a conservative default;
/// missing sysfs entries are common in containers and CI runners.
///
/// path Absolute path to a sysfs file containing the cpu-list-format string.
/// Sorted, deduplicated list of CPU ids; empty when the file cannot be read.
inline std::vector<std::uint32_t> readCpuList(const std::string &path) {
  std::vector<std::uint32_t> result;
  std::ifstream in(path);
  if (!in.is_open()) {
    return result;
  }
  std::string line;
  if (!std::getline(in, line)) {
    return result;
  }
  std::stringstream ss(line);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) {
      continue;
    }
    const auto dash = token.find('-');
    if (dash == std::string::npos) {
      try {
        result.push_back(static_cast<std::uint32_t>(std::stoul(token)));
      } catch (const std::exception &) {
        // Malformed token; skip rather than throwing from a topology probe.
        continue;
      }
    } else {
      try {
        const auto first =
            static_cast<std::uint32_t>(std::stoul(token.substr(0, dash)));
        const auto last =
            static_cast<std::uint32_t>(std::stoul(token.substr(dash + 1)));
        for (std::uint32_t cpu = first; cpu <= last; ++cpu) {
          result.push_back(cpu);
        }
      } catch (const std::exception &) {
        // Malformed range; skip the token.
        continue;
      }
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

#if defined(_WIN32)
/// Windows-side topology probe. Uses `GetLogicalProcessorInformationEx` for
/// SMT-sibling discovery (`RelationProcessorCore`), shared-L3 groups
/// (`RelationCache` filtered to `Level == 3`), and cache sizes; uses
/// `GetProcessAffinityMask` for the allowed-CPU set. Falls back to a single
/// synthetic CCD covering every allowed CPU when the OS rejects the query
/// (e.g. inside a container that masked off the API).
inline Topology detectTopologyWindows() {
  Topology topo;
  topo.logicalCount = std::thread::hardware_concurrency();
  if (topo.logicalCount == 0U) {
    topo.logicalCount = 1U;
  }
  topo.ccdOfCpu.assign(topo.logicalCount, 0U);
  topo.smtSiblingOfCpu.assign(topo.logicalCount, UINT32_MAX);

  std::vector<std::uint32_t> allowed;
  allowed.reserve(topo.logicalCount);
  {
    DWORD_PTR procMask = 0;
    DWORD_PTR sysMask = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &procMask, &sysMask) !=
        FALSE) {
      const std::uint32_t bits =
          static_cast<std::uint32_t>(sizeof(DWORD_PTR) * 8U);
      const std::uint32_t scanLimit =
          topo.logicalCount < bits ? topo.logicalCount : bits;
      for (std::uint32_t cpu = 0; cpu < scanLimit; ++cpu) {
        if ((procMask & (static_cast<DWORD_PTR>(1) << cpu)) != 0U) {
          allowed.push_back(cpu);
        }
      }
    }
  }
  if (allowed.empty()) {
    allowed.reserve(topo.logicalCount);
    for (std::uint32_t cpu = 0; cpu < topo.logicalCount; ++cpu) {
      allowed.push_back(cpu);
    }
  }

  // Build a CPU -> SMT-sibling list and pick one CPU per physical core.
  // `RelationProcessorCore` records have `GroupMask[0].Mask` set to the
  // logical-CPU bitset of the physical core's siblings.
  std::vector<std::vector<std::uint32_t>> smtSiblings(topo.logicalCount);
  (void)walkLogicalProcessorInfoEx(
      RelationProcessorCore,
      [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX &info) {
        if (info.Relationship != RelationProcessorCore) {
          return;
        }
        const auto &core = info.Processor;
        if (core.GroupCount == 0U) {
          return;
        }
        const std::vector<std::uint32_t> cpus =
            expandGroupAffinity(core.GroupMask[0]);
        for (const std::uint32_t cpu : cpus) {
          if (cpu < smtSiblings.size()) {
            smtSiblings[cpu] = cpus;
          }
        }
      });
  // Record the "other" sibling per CPU. On SMT4 silicon (>2 reported
  // siblings) we pick the first non-self entry deterministically.
  for (std::uint32_t cpu = 0; cpu < topo.logicalCount; ++cpu) {
    const auto &sibs = smtSiblings[cpu];
    for (const std::uint32_t s : sibs) {
      if (s != cpu) {
        topo.smtSiblingOfCpu[cpu] = s;
        break;
      }
    }
  }

  std::vector<bool> consumed(topo.logicalCount, false);
  for (const std::uint32_t cpu : allowed) {
    if (cpu >= consumed.size() || consumed[cpu]) {
      continue;
    }
    consumed[cpu] = true;
    topo.physicalCores.push_back(cpu);
    if (cpu < smtSiblings.size()) {
      for (const std::uint32_t sib : smtSiblings[cpu]) {
        if (sib < consumed.size()) {
          consumed[sib] = true;
        }
      }
    }
  }
  if (topo.physicalCores.empty()) {
    topo.physicalCores = allowed;
  }
  topo.physicalCount = static_cast<std::uint32_t>(topo.physicalCores.size());

  // Per-CPU L3 shared-CPU list + L3 size from `RelationCache` filtered to
  // `Level == 3`. Per-core L2 sampled the same way at `Level == 2`. Caches
  // whose `GroupCount > 1` (split across processor groups) are skipped;
  // pools spanning multiple groups would need `SetThreadGroupAffinity`.
  std::vector<std::vector<std::uint32_t>> l3SharedByCpu(topo.logicalCount);
  std::vector<std::uint64_t> l3SizeKibByCpu(topo.logicalCount, 0U);
  std::vector<std::uint64_t> l2SizeKibByCpu(topo.logicalCount, 0U);
  (void)walkLogicalProcessorInfoEx(
      RelationCache, [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX &info) {
        if (info.Relationship != RelationCache) {
          return;
        }
        const auto &cache = info.Cache;
        if (cache.Level != 2 && cache.Level != 3) {
          return;
        }
        if (cache.GroupCount == 0U) {
          return;
        }
        const std::vector<std::uint32_t> cpus =
            expandGroupAffinity(cache.GroupMasks[0]);
        const std::uint64_t sizeKib =
            static_cast<std::uint64_t>(cache.CacheSize) / 1024U;
        if (cache.Level == 3) {
          for (const std::uint32_t cpu : cpus) {
            if (cpu < l3SharedByCpu.size()) {
              l3SharedByCpu[cpu] = cpus;
              l3SizeKibByCpu[cpu] = sizeKib;
            }
          }
        } else { // Level == 2
          for (const std::uint32_t cpu : cpus) {
            if (cpu < l2SizeKibByCpu.size()) {
              l2SizeKibByCpu[cpu] = sizeKib;
            }
          }
        }
      });

  // Group physical cores by shared L3.
  std::vector<bool> assigned(topo.logicalCount, false);
  for (const std::uint32_t cpu : topo.physicalCores) {
    if (cpu < assigned.size() && assigned[cpu]) {
      continue;
    }
    std::vector<std::uint32_t> shared = cpu < l3SharedByCpu.size()
                                            ? l3SharedByCpu[cpu]
                                            : std::vector<std::uint32_t>{};
    if (shared.empty()) {
      shared.push_back(cpu);
    }
    std::vector<std::uint32_t> physicalInGroup;
    physicalInGroup.reserve(shared.size());
    for (const std::uint32_t sharedCpu : shared) {
      const auto it = std::find(topo.physicalCores.begin(),
                                topo.physicalCores.end(), sharedCpu);
      if (it != topo.physicalCores.end()) {
        physicalInGroup.push_back(sharedCpu);
        if (sharedCpu < assigned.size()) {
          assigned[sharedCpu] = true;
        }
      }
    }
    if (physicalInGroup.empty()) {
      physicalInGroup.push_back(cpu);
      if (cpu < assigned.size()) {
        assigned[cpu] = true;
      }
    }
    const auto ccdIndex = static_cast<std::uint32_t>(topo.ccdGroups.size());
    for (const std::uint32_t physCpu : physicalInGroup) {
      if (physCpu < topo.ccdOfCpu.size()) {
        topo.ccdOfCpu[physCpu] = ccdIndex;
      }
    }
    topo.ccdGroups.push_back(std::move(physicalInGroup));
  }
  topo.ccdCount = static_cast<std::uint32_t>(topo.ccdGroups.size());

  // Per-CCD L3 size from the per-CPU map.
  topo.l3KibOfCcd.assign(topo.ccdGroups.size(), 0U);
  for (std::size_t ccd = 0; ccd < topo.ccdGroups.size(); ++ccd) {
    if (topo.ccdGroups[ccd].empty()) {
      continue;
    }
    const std::uint32_t rep = topo.ccdGroups[ccd].front();
    if (rep < l3SizeKibByCpu.size()) {
      topo.l3KibOfCcd[ccd] = l3SizeKibByCpu[rep];
    }
  }
  if (!topo.physicalCores.empty()) {
    const std::uint32_t first = topo.physicalCores.front();
    if (first < l2SizeKibByCpu.size()) {
      topo.l2KibPerCore = l2SizeKibByCpu[first];
    }
  }
  std::uint64_t bestKib = 0U;
  std::uint32_t bestIdx = 0;
  for (std::size_t ccd = 0; ccd < topo.l3KibOfCcd.size(); ++ccd) {
    if (topo.l3KibOfCcd[ccd] > bestKib) {
      bestKib = topo.l3KibOfCcd[ccd];
      bestIdx = static_cast<std::uint32_t>(ccd);
    }
  }
  topo.preferredCcd = bestIdx;

  // Reorder each CCD so SMT-capable cores appear first, descending CPU
  // id otherwise. Hybrid Intel parts expose E-cores at higher CPU ids;
  // pure-descending order would place the producer on an E-core, where
  // the slot-1 SMT-sibling routing rule has nothing to attach to. AMD
  // parts have SMT on every core, so the bias is a no-op.
  std::vector<std::uint32_t> reordered;
  reordered.reserve(topo.physicalCores.size());
  for (const auto &group : topo.ccdGroups) {
    std::vector<std::uint32_t> withSibling;
    std::vector<std::uint32_t> withoutSibling;
    withSibling.reserve(group.size());
    withoutSibling.reserve(group.size());
    for (std::size_t i = group.size(); i > 0U; --i) {
      const std::uint32_t cpu = group[i - 1U];
      const bool hasSibling = cpu < topo.smtSiblingOfCpu.size() &&
                              topo.smtSiblingOfCpu[cpu] != UINT32_MAX;
      if (hasSibling) {
        withSibling.push_back(cpu);
      } else {
        withoutSibling.push_back(cpu);
      }
    }
    reordered.insert(reordered.end(), withSibling.begin(), withSibling.end());
    reordered.insert(reordered.end(), withoutSibling.begin(),
                     withoutSibling.end());
  }
  if (reordered.size() == topo.physicalCores.size()) {
    topo.physicalCores = std::move(reordered);
  }
  return topo;
}
#endif // _WIN32

/// Probe the host's CPU topology via sysfs and the process affinity mask.
///
/// The detection sequence:
/// 1. `sched_getaffinity(0, ...)` reads which logical CPUs the process is
/// allowed to use.
/// 2. For each allowed CPU, `topology/thread_siblings_list` selects one logical
/// per physical core.
/// 3. For each chosen physical CPU, `cache/index3/shared_cpu_list` groups them
/// by shared L3.
/// 4. When sysfs is absent the function falls back to `hardware_concurrency()`
/// and a single CCD.
///
/// The returned `Topology` is the source of truth for affinity decisions for
/// the pool's lifetime.
///
/// Populated `Topology`; never throws even if sysfs is unavailable.
inline Topology detectTopology() {
#if defined(_WIN32)
  return detectTopologyWindows();
#else
  Topology topo;
  topo.logicalCount = std::thread::hardware_concurrency();
  if (topo.logicalCount == 0) {
    topo.logicalCount = 1;
  }
  topo.ccdOfCpu.assign(topo.logicalCount, 0U);
  topo.smtSiblingOfCpu.assign(topo.logicalCount, UINT32_MAX);

  std::vector<std::uint32_t> allowed;
  allowed.reserve(topo.logicalCount);

#ifdef __linux__
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
    /// Cap the scan at `CPU_SETSIZE` so the fixed-size `cpu_set_t` is never
    /// indexed past its bit range on hosts with more than `CPU_SETSIZE`
    /// logical CPUs. Pools never need more than `physicalCores` workers so
    /// the cap only affects affinity reporting, not scheduling.
    const auto cpuMax = static_cast<std::uint32_t>(CPU_SETSIZE);
    const std::uint32_t scanLimit =
        topo.logicalCount < cpuMax ? topo.logicalCount : cpuMax;
    for (std::uint32_t cpu = 0; cpu < scanLimit; ++cpu) {
      if (CPU_ISSET(static_cast<std::size_t>(cpu), &mask)) {
        allowed.push_back(cpu);
      }
    }
  }
#endif

  if (allowed.empty()) {
    allowed.reserve(topo.logicalCount);
    for (std::uint32_t cpu = 0; cpu < topo.logicalCount; ++cpu) {
      allowed.push_back(cpu);
    }
  }

  // Pick one logical CPU per physical core (skip SMT siblings). Record
  // per-CPU sibling mapping while walking sysfs so callers can route
  // slot 1 to the producer's SMT sibling without re-reading sysfs.
  std::vector<bool> consumed(topo.logicalCount, false);
  for (const std::uint32_t cpu : allowed) {
    if (cpu >= consumed.size() || consumed[cpu]) {
      continue;
    }
    consumed[cpu] = true;
    topo.physicalCores.push_back(cpu);

    const std::string siblingsPath = "/sys/devices/system/cpu/cpu" +
                                     std::to_string(cpu) +
                                     "/topology/thread_siblings_list";
    const std::vector<std::uint32_t> siblings = readCpuList(siblingsPath);
    for (const std::uint32_t sib : siblings) {
      if (sib < consumed.size()) {
        consumed[sib] = true;
      }
    }
    // Stamp both directions of each sibling pair on first sighting; the
    // SMT4 fallback in `smtSiblingOfCpu`'s doc applies if a core reports
    // more than two siblings.
    for (const std::uint32_t sib : siblings) {
      if (sib >= topo.smtSiblingOfCpu.size() || sib == cpu) {
        continue;
      }
      if (topo.smtSiblingOfCpu[cpu] == UINT32_MAX) {
        topo.smtSiblingOfCpu[cpu] = sib;
      }
      if (topo.smtSiblingOfCpu[sib] == UINT32_MAX) {
        topo.smtSiblingOfCpu[sib] = cpu;
      }
    }
  }

  if (topo.physicalCores.empty()) {
    // Fallback: every allowed logical CPU is its own physical core.
    topo.physicalCores = allowed;
  }

  topo.physicalCount = static_cast<std::uint32_t>(topo.physicalCores.size());

  // Group physical cores by shared L3 (CCD on Zen, cluster on heterogeneous
  // SoCs).
  std::vector<bool> assigned(topo.logicalCount, false);
  for (const std::uint32_t cpu : topo.physicalCores) {
    if (cpu < assigned.size() && assigned[cpu]) {
      continue;
    }
    const std::string l3Path = "/sys/devices/system/cpu/cpu" +
                               std::to_string(cpu) +
                               "/cache/index3/shared_cpu_list";
    std::vector<std::uint32_t> shared = readCpuList(l3Path);
    if (shared.empty()) {
      // Either sysfs is absent or this CPU has no L3; emit a singleton group.
      shared.push_back(cpu);
    }

    std::vector<std::uint32_t> physicalInGroup;
    physicalInGroup.reserve(shared.size());
    for (const std::uint32_t sharedCpu : shared) {
      const auto it = std::find(topo.physicalCores.begin(),
                                topo.physicalCores.end(), sharedCpu);
      if (it != topo.physicalCores.end()) {
        physicalInGroup.push_back(sharedCpu);
        if (sharedCpu < assigned.size()) {
          assigned[sharedCpu] = true;
        }
      }
    }
    if (physicalInGroup.empty()) {
      physicalInGroup.push_back(cpu);
      if (cpu < assigned.size()) {
        assigned[cpu] = true;
      }
    }
    const auto ccdIndex = static_cast<std::uint32_t>(topo.ccdGroups.size());
    for (const std::uint32_t physCpu : physicalInGroup) {
      if (physCpu < topo.ccdOfCpu.size()) {
        topo.ccdOfCpu[physCpu] = ccdIndex;
      }
    }
    topo.ccdGroups.push_back(std::move(physicalInGroup));
  }

  topo.ccdCount = static_cast<std::uint32_t>(topo.ccdGroups.size());

  // Per-CCD L3 size + preferred-CCD selection. V-Cache parts have one CCD with
  // a stacked SRAM die (96 MiB on 9950X3D's CCD0 vs 32 MiB on the regular CCD);
  // for workloads whose working set exceeds the smaller L3 but fits the larger,
  // landing on the V-Cache CCD is a 5-10x speedup. We pick the largest-L3 CCD
  // as the default placement target; tie-break by lowest index so symmetric
  // Zens (no X3D) still get a deterministic choice across runs.
  topo.l3KibOfCcd.assign(topo.ccdGroups.size(), 0U);
  for (std::size_t ccd = 0; ccd < topo.ccdGroups.size(); ++ccd) {
    if (topo.ccdGroups[ccd].empty()) {
      continue;
    }
    const std::uint32_t rep = topo.ccdGroups[ccd].front();
    topo.l3KibOfCcd[ccd] =
        readCacheSizeKib("/sys/devices/system/cpu/cpu" + std::to_string(rep) +
                         "/cache/index3/size");
  }
  // Per-core L2: probe one representative CPU. Per-core L2 is
  // architecture-uniform on every CPU we currently target (Zen, P-cores
  // on Alder Lake, Apple firestorm/icestorm), so a single sample is
  // sufficient. Future heterogeneous parts will need a per-cluster
  // probe.
  if (!topo.physicalCores.empty()) {
    topo.l2KibPerCore = readCacheSizeKib(
        "/sys/devices/system/cpu/cpu" +
        std::to_string(topo.physicalCores.front()) + "/cache/index2/size");
  }
  std::uint64_t bestKib = 0U;
  std::uint32_t bestIdx = 0;
  for (std::size_t ccd = 0; ccd < topo.l3KibOfCcd.size(); ++ccd) {
    if (topo.l3KibOfCcd[ccd] > bestKib) {
      bestKib = topo.l3KibOfCcd[ccd];
      bestIdx = static_cast<std::uint32_t>(ccd);
    }
  }
  topo.preferredCcd = bestIdx;

  // Reorder each CCD so SMT-capable cores appear first, descending CPU
  // id otherwise. Hybrid Intel parts expose E-cores at higher CPU ids;
  // pure-descending order would place the producer on an E-core, where
  // the slot-1 SMT-sibling routing rule has nothing to attach to. The
  // descending tail within each bucket preserves the irqbalance bias
  // that puts kthreads on the low-numbered siblings.
  std::vector<std::uint32_t> reordered;
  reordered.reserve(topo.physicalCores.size());
  for (const auto &group : topo.ccdGroups) {
    std::vector<std::uint32_t> withSibling;
    std::vector<std::uint32_t> withoutSibling;
    withSibling.reserve(group.size());
    withoutSibling.reserve(group.size());
    for (std::size_t i = group.size(); i > 0U; --i) {
      const std::uint32_t cpu = group[i - 1U];
      const bool hasSibling = cpu < topo.smtSiblingOfCpu.size() &&
                              topo.smtSiblingOfCpu[cpu] != UINT32_MAX;
      if (hasSibling) {
        withSibling.push_back(cpu);
      } else {
        withoutSibling.push_back(cpu);
      }
    }
    reordered.insert(reordered.end(), withSibling.begin(), withSibling.end());
    reordered.insert(reordered.end(), withoutSibling.begin(),
                     withoutSibling.end());
  }
  if (reordered.size() == topo.physicalCores.size()) {
    topo.physicalCores = std::move(reordered);
  }
  return topo;
#endif // _WIN32
}

/// Enumerate the CPU ids belonging to each CCD (or shared-L3 cluster).
///
/// Returns one inner vector per CCD; the outer vector's index is the CCD id.
/// The CPU ids are physical-core representatives (one logical per sibling set)
/// inside the process affinity mask, matching `Topology::ccdGroups` exactly.
/// Used by the `PoolGroup` constructor to size and pin one arena per CCD.
///
/// Mock fallback: when sysfs is unavailable or the process is restricted to a
/// single CPU, the function returns a single CCD containing every allowed CPU.
/// Callers should treat the empty outer vector as a host with no usable CPUs
/// (never observed in practice).
///
/// Outer vector indexed by CCD id; inner vectors are CPU id lists per CCD.
inline std::vector<std::vector<unsigned>> enumerateCcds() {
  const Topology topo = detectTopology();
  std::vector<std::vector<unsigned>> result;
  result.reserve(topo.ccdGroups.size());
  for (const auto &group : topo.ccdGroups) {
    std::vector<unsigned> cpus;
    cpus.reserve(group.size());
    for (const std::uint32_t cpu : group) {
      cpus.push_back(static_cast<unsigned>(cpu));
    }
    result.push_back(std::move(cpus));
  }
  if (result.empty()) {
    std::vector<unsigned> all;
    all.reserve(topo.physicalCores.size());
    for (const std::uint32_t cpu : topo.physicalCores) {
      all.push_back(static_cast<unsigned>(cpu));
    }
    if (all.empty()) {
      const unsigned hwc = std::thread::hardware_concurrency();
      const unsigned bound = hwc > 0U ? hwc : 1U;
      all.reserve(bound);
      for (unsigned cpu = 0; cpu < bound; ++cpu) {
        all.push_back(cpu);
      }
    }
    result.push_back(std::move(all));
  }
  return result;
}

/// Reorder `|cpuPins|` so the producer's reserved CPU sits at index 0.
/// Two exceptions to the default physical-cores-first layout:
///   * `participants == 2`: slot 1 lands on the producer's SMT sibling
///     so the handshake stays L1-resident.
///   * `participants > pins.size()`: SMT siblings of existing pins are
///     appended as overflow, producer-CCD first.
/// Standalone pools also get a producer-CCD-first reorder. Arena pools
/// skip it because `PoolGroup` already filtered `cpuPins` to one CCD.
inline std::vector<std::uint32_t>
reserveProducerCpuFirst(const std::vector<std::uint32_t> &cpuPins,
                        std::size_t participants, bool standalone,
                        const Topology &topo) {
  std::vector<std::uint32_t> pins = cpuPins;
  if (pins.size() <= 1U) {
    return pins;
  }
  if (standalone) {
    const std::uint32_t targetCcd = topo.preferredCcd;
    if (targetCcd < topo.ccdGroups.size()) {
      std::vector<std::uint32_t> reordered;
      reordered.reserve(pins.size());
      for (const std::uint32_t cpu : pins) {
        if (cpu < topo.ccdOfCpu.size() && topo.ccdOfCpu[cpu] == targetCcd) {
          reordered.push_back(cpu);
        }
      }
      for (const std::uint32_t cpu : pins) {
        if (cpu >= topo.ccdOfCpu.size() || topo.ccdOfCpu[cpu] != targetCcd) {
          reordered.push_back(cpu);
        }
      }
      if (reordered.size() == pins.size()) {
        pins = std::move(reordered);
      }
    }
  }

  // Exception A: single-worker pool. Slot 1 = producer's SMT sibling.
  if (participants == 2U && !topo.smtSiblingOfCpu.empty()) {
    const std::uint32_t prodCpu = pins[0];
    if (prodCpu < topo.smtSiblingOfCpu.size()) {
      const std::uint32_t sib = topo.smtSiblingOfCpu[prodCpu];
      if (sib != UINT32_MAX && sib != prodCpu) {
        const auto sibIt = std::find(pins.begin(), pins.end(), sib);
        if (sibIt == pins.end()) {
          pins.insert(pins.begin() + 1, sib);
        } else if (sibIt != pins.begin() + 1) {
          std::iter_swap(pins.begin() + 1, sibIt);
        }
      }
    }
    return pins;
  }

  // Exception B: oversubscribed pool. Append SMT siblings as overflow,
  // producer-CCD siblings first.
  if (participants > pins.size() && !topo.smtSiblingOfCpu.empty()) {
    const std::uint32_t targetCcd = topo.preferredCcd;
    const std::size_t baseCount = pins.size();
    for (std::size_t pass = 0; pass < 2U; ++pass) {
      for (std::size_t i = 0; i < baseCount; ++i) {
        const std::uint32_t cpu = pins[i];
        if (cpu >= topo.smtSiblingOfCpu.size()) {
          continue;
        }
        const std::uint32_t sib = topo.smtSiblingOfCpu[cpu];
        if (sib == UINT32_MAX || sib == cpu) {
          continue;
        }
        const bool preferredCcdMatch = sib < topo.ccdOfCpu.size() &&
                                       targetCcd < topo.ccdGroups.size() &&
                                       topo.ccdOfCpu[sib] == targetCcd;
        if (pass == 0U && !preferredCcdMatch) {
          continue;
        }
        if (pass == 1U && preferredCcdMatch) {
          continue;
        }
        const bool already =
            std::find(pins.begin(), pins.end(), sib) != pins.end();
        if (!already) {
          pins.push_back(sib);
        }
      }
    }
  }
  return pins;
}

/// Pin the calling thread to a single CPU id.
///
/// Called exactly once per worker, immediately after creation. The hot path
/// never invokes `pthread_setaffinity_np`. Failures (e.g. CPU not in the
/// process mask) are silently ignored; the pool tolerates the fallback to OS
/// scheduling rather than aborting startup.
///
/// cpuId Logical CPU id to pin to.
inline void bindAffinityOnce(std::uint32_t cpuId) noexcept {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpuId), &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#elif defined(_WIN32)
  // Single-CPU mask within the current processor group. Hosts with >64
  // logical CPUs would need `SetThreadGroupAffinity`; pools that large are
  // out of the supported envelope. Failure (e.g. CPU outside the process
  // affinity mask) is intentionally non-fatal.
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << (cpuId & 63U);
  (void)::SetThreadAffinityMask(::GetCurrentThread(), mask);
#else
  (void)cpuId;
#endif
}

} // namespace citor::detail
