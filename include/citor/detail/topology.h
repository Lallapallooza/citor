#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace citor::detail {

/// Logical view of the host's CPU topology used for affinity decisions.
///
/// Constructed once via `detectTopology()` at pool startup; never queried on the hot path. The pool
/// uses `physicalCores` for one-worker-per-physical-core pinning, `ccdGroups` and `ccdOfCpu` for
/// CCD-aware victim selection, and the counts for sizing the workers vector when no explicit
/// participant count is supplied. On hosts where sysfs is unavailable the constructor falls back to
/// `std::thread::hardware_concurrency()` and treats every logical CPU as its own physical core
/// inside one synthetic CCD.
struct Topology {
  /// Process-affinity-filtered list of one logical CPU per physical core (the pinning targets).
  std::vector<std::uint32_t> physicalCores;

  /// Lists of physical-core CPU ids grouped by shared L3 cache (CCD on Zen, cluster on big.LITTLE).
  std::vector<std::vector<std::uint32_t>> ccdGroups;

  /// CCD index for each logical CPU id; `ccdOfCpu[id]` indexes `ccdGroups`.
  std::vector<std::uint32_t> ccdOfCpu;

  /// Total logical CPU count reported by the OS.
  std::uint32_t logicalCount = 0;

  /// Number of physical cores in the process affinity mask.
  std::uint32_t physicalCount = 0;

  /// Number of distinct CCDs (or shared-L3 groups).
  std::uint32_t ccdCount = 0;
};

/// Read a comma-separated CPU list (e.g. `0-7,16-23`) from a sysfs file.
///
/// Used for both `thread_siblings_list` (SMT detection) and `cache/index3/shared_cpu_list` (CCD
/// detection). Returns an empty vector when the file is absent so the caller can fall back to a
/// conservative default; missing sysfs entries are common in containers and CI runners.
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
        const auto first = static_cast<std::uint32_t>(std::stoul(token.substr(0, dash)));
        const auto last = static_cast<std::uint32_t>(std::stoul(token.substr(dash + 1)));
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

/// Probe the host's CPU topology via sysfs and the process affinity mask.
///
/// The detection sequence:
/// 1. `sched_getaffinity(0, ...)` reads which logical CPUs the process is allowed to use.
/// 2. For each allowed CPU, `topology/thread_siblings_list` selects one logical per physical core.
/// 3. For each chosen physical CPU, `cache/index3/shared_cpu_list` groups them by shared L3.
/// 4. When sysfs is absent the function falls back to `hardware_concurrency()` and a single CCD.
///
/// The returned `Topology` is the source of truth for affinity decisions for the pool's lifetime.
///
/// Populated `Topology`; never throws even if sysfs is unavailable.
inline Topology detectTopology() {
  Topology topo;
  topo.logicalCount = std::thread::hardware_concurrency();
  if (topo.logicalCount == 0) {
    topo.logicalCount = 1;
  }
  topo.ccdOfCpu.assign(topo.logicalCount, 0U);

  std::vector<std::uint32_t> allowed;
  allowed.reserve(topo.logicalCount);

#ifdef __linux__
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
    for (std::uint32_t cpu = 0; cpu < topo.logicalCount; ++cpu) {
      if (CPU_ISSET(static_cast<int>(cpu), &mask)) {
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

  // Pick one logical CPU per physical core (skip SMT siblings).
  std::vector<bool> consumed(topo.logicalCount, false);
  for (const std::uint32_t cpu : allowed) {
    if (cpu >= consumed.size() || consumed[cpu]) {
      continue;
    }
    consumed[cpu] = true;
    topo.physicalCores.push_back(cpu);

    const std::string siblingsPath =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list";
    const std::vector<std::uint32_t> siblings = readCpuList(siblingsPath);
    for (const std::uint32_t sib : siblings) {
      if (sib < consumed.size()) {
        consumed[sib] = true;
      }
    }
  }

  if (topo.physicalCores.empty()) {
    // Fallback: every allowed logical CPU is its own physical core.
    topo.physicalCores = allowed;
  }

  topo.physicalCount = static_cast<std::uint32_t>(topo.physicalCores.size());

  // Group physical cores by shared L3 (CCD on Zen, cluster on heterogeneous SoCs).
  std::vector<bool> assigned(topo.logicalCount, false);
  for (const std::uint32_t cpu : topo.physicalCores) {
    if (cpu < assigned.size() && assigned[cpu]) {
      continue;
    }
    const std::string l3Path =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index3/shared_cpu_list";
    std::vector<std::uint32_t> shared = readCpuList(l3Path);
    if (shared.empty()) {
      // Either sysfs is absent or this CPU has no L3; emit a singleton group.
      shared.push_back(cpu);
    }

    std::vector<std::uint32_t> physicalInGroup;
    physicalInGroup.reserve(shared.size());
    for (const std::uint32_t sharedCpu : shared) {
      const auto it = std::find(topo.physicalCores.begin(), topo.physicalCores.end(), sharedCpu);
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

  // Reorder `physicalCores` so each CCD's members appear in descending CPU id order. The
  // standalone-pool slot-0 CPU (the caller thread's current CPU) is later rotated to the
  // front by `reserveProducerCpuFirst`; until that rotation, putting the highest-CPU member
  // of each CCD first means slots 1..N-1 of the same-CCD subset land on the higher-numbered
  // sibling pairs. On Linux those pairs typically have lighter IRQ steering than the BSP-
  // adjacent siblings (CPU 0+16 / 1+17 are the conventional irqbalance defaults), so a
  // hot-spinning worker on the high-CPU end of the CCD sees fewer SMT-cross-issue collisions
  // from kthreads on its sibling.
  std::vector<std::uint32_t> reordered;
  reordered.reserve(topo.physicalCores.size());
  for (const auto &group : topo.ccdGroups) {
    for (auto it = group.rbegin(); it != group.rend(); ++it) {
      reordered.push_back(*it);
    }
  }
  if (reordered.size() == topo.physicalCores.size()) {
    topo.physicalCores = std::move(reordered);
  }
  return topo;
}

/// Enumerate the CPU ids belonging to each CCD (or shared-L3 cluster).
///
/// Returns one inner vector per CCD; the outer vector's index is the CCD id. The CPU ids are
/// physical-core representatives (one logical per sibling set) inside the process affinity mask,
/// matching `Topology::ccdGroups` exactly. Used by the `PoolGroup` constructor to size and pin one
/// arena per CCD.
///
/// Mock fallback: when sysfs is unavailable or the process is restricted to a single CPU, the
/// function returns a single CCD containing every allowed CPU. Callers should treat the empty
/// outer vector as a host with no usable CPUs (never observed in practice).
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

/// Pin the calling thread to a single CPU id.
///
/// Called exactly once per worker, immediately after creation. The hot path never invokes
/// `pthread_setaffinity_np`. Failures (e.g. CPU not in the process mask) are silently ignored;
/// the pool tolerates the fallback to OS scheduling rather than aborting startup.
///
/// cpuId Logical CPU id to pin to.
inline void bindAffinityOnce(std::uint32_t cpuId) noexcept {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(cpuId), &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)cpuId;
#endif
}

} // namespace citor::detail
