#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "citor/detail/coherence_probe.h"

namespace citor {

class ThreadPool;

/// Serialise a pool's one-time coherence probe to a portable blob. The blob
/// embeds the probe's worker cpuset, which is the key `importCoherenceProbe`
/// seeds under, so a short-lived process can persist it and replay it on the
/// next run to let a matching pool skip the live probe. Returns an empty
/// vector when the pool has no valid probe (single-worker and arena pools
/// never run it).
std::vector<std::byte> exportCoherenceProbe(const ThreadPool &pool);

/// Seed the process-wide probe cache from a blob produced by
/// @ref citor::exportCoherenceProbe. The next `ThreadPool` whose worker cpuset
/// matches the blob's embedded key returns the seeded probe instead of running
/// the live calibration; a cpuset that does not match is a harmless miss that
/// re-probes. Returns false, with no effect and without throwing, on a magic
/// or version mismatch, truncation, or a structural inconsistency.
inline bool importCoherenceProbe(std::span<const std::byte> bytes) {
  detail::CoherenceProbe probe;
  if (!detail::deserializeCoherenceProbe(bytes, probe)) {
    return false;
  }
  detail::seedCoherenceProbeCache(probe.matrix.cpus, probe);
  return true;
}

} // namespace citor
