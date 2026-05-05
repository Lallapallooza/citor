#pragma once

// Multi-arena harness for cross-CCD benches that exercise citor::PoolGroup.
// Re-exports enumerateCcds, exposes a MultiArenaHarness adapter that owns a
// scoped per-cell PoolGroup, and a runtime probe that aborts when the host
// has fewer CCDs than required. Single-CCD hosts must skip registration of
// these workloads.
//
// The harness owns a value-typed PoolGroup so each bench cell's arena
// workers go away with the harness; otherwise the process-wide
// PoolGroup::global() singleton would leave its (~16 worker) arena fleet
// alive for the rest of the bench, and any later cell that builds its own
// ThreadPool(16) would run with 32 workers pinned to 16 CPUs and pay
// 5-8x slowdown on recursive-task workloads (skynet, strassen DaC, etc.).

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "citor/detail/topology.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"

namespace citor::bench {

// Re-export of detail::enumerateCcds() so cross-CCD bench TUs can probe the
// host's topology without reaching into detail/. Inner vectors are CPU id
// lists; outer index is the CCD id. On a host without sysfs the result is one
// synthetic CCD covering every allowed CPU.
[[nodiscard]] inline std::vector<std::vector<unsigned>> enumerateCcds() {
  return citor::detail::enumerateCcds();
}

// Runtime probe used by cross-CCD bench TUs from their registrar. Throws
// std::runtime_error when the host has fewer CCDs than |required|; the
// registrar catches and skips registerWorkload.
[[nodiscard]] inline std::size_t
requireMultipleCcds(std::size_t required = 2U) {
  const std::size_t observed = enumerateCcds().size();
  if (observed < required) {
    throw std::runtime_error{"cross-CCD bench requires at least " +
                             std::to_string(required) + " CCDs; host reports " +
                             std::to_string(observed)};
  }
  return observed;
}

// Per-CCD ThreadPool arena adapter that owns a scoped PoolGroup. The
// underlying arenas are constructed when the harness is constructed and
// joined when it goes out of scope; bench TUs should construct exactly one
// harness per cell so the arena workers do not outlive the cell.
class MultiArenaHarness {
public:
  // Construct the per-cell PoolGroup. Verifies every arena reports
  // PoolKind::Arena and, when |requiredCcds| > 0, that the topology probe
  // enumerated at least that many CCDs. The latter guards the
  // construction-order trap: if the harness is constructed AFTER the
  // producer is pinned to a single-CCD subset, the probe collapses to one
  // synthetic CCD and the harness would silently report same-CCD numbers.
  // Cross-CCD bench TUs MUST pass requiredCcds >= 2.
  explicit MultiArenaHarness(std::size_t requiredCcds = 0U) {
    if (requiredCcds > 0U && m_group.ccdCount() < requiredCcds) {
      throw std::runtime_error{
          "MultiArenaHarness constructed with degraded topology; constructed "
          "under affinity mask that collapsed PoolGroup to fewer CCDs than "
          "required (need " +
          std::to_string(requiredCcds) + ", got " +
          std::to_string(m_group.ccdCount()) +
          "). Construct harness BEFORE pinning the producer thread."};
    }
    for (std::size_t ccd = 0; ccd < m_group.ccdCount(); ++ccd) {
      if (m_group.arena(ccd).kind() != citor::PoolKind::Arena) {
        throw std::runtime_error{
            "MultiArenaHarness: arena " + std::to_string(ccd) +
            " kind() != PoolKind::Arena; PoolGroup state is corrupt"};
      }
    }
  }

  MultiArenaHarness(const MultiArenaHarness &) = delete;
  MultiArenaHarness &operator=(const MultiArenaHarness &) = delete;
  MultiArenaHarness(MultiArenaHarness &&) = delete;
  MultiArenaHarness &operator=(MultiArenaHarness &&) = delete;
  ~MultiArenaHarness() = default;

  // Number of arenas owned by this harness's PoolGroup.
  [[nodiscard]] std::size_t arenaCount() const noexcept {
    return m_group.ccdCount();
  }

  // Access the per-CCD arena at |ccd|; |ccd| must be < arenaCount().
  [[nodiscard]] citor::ThreadPool &arena(std::size_t ccd) noexcept {
    return m_group.arena(ccd);
  }

  // Total participants summed across every arena.
  [[nodiscard]] std::size_t totalParticipants() const noexcept {
    std::size_t sum = 0;
    for (std::size_t ccd = 0; ccd < m_group.ccdCount(); ++ccd) {
      sum += m_group.arena(ccd).participants();
    }
    return sum;
  }

private:
  citor::PoolGroup m_group;
};

} // namespace citor::bench
