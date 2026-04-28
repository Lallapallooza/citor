#pragma once

// Shared multi-arena harness for cross-CCD benches that exercise
// `citor::PoolGroup`. The header re-exports `enumerateCcds`, exposes a
// `MultiArenaHarness` adapter that hands out per-CCD `ThreadPool` references
// owned by `PoolGroup::global()`, and a runtime probe that aborts with a
// diagnostic when the host has fewer than the requested number of CCDs.
//
// Consumed by `cross_ccd_parallel_for_bench.cpp` (and any future cross-arena
// bench TU). Single-CCD hosts must skip registration of these workloads;
// `requireMultipleCcds()` is the explicit fail-fast probe TUs call from their
// registrar to avoid silently mismatching the bench's intent.

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "citor/detail/topology.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"

namespace citor::bench {

/// Re-export of `citor::detail::enumerateCcds()` so cross-CCD bench TUs
///        can probe the host's topology without reaching into `detail/`.
///
/// The inner vectors are CPU id lists; the outer vector's index is the CCD id.
/// On a host where sysfs is unavailable, `enumerateCcds()` returns a single
/// synthetic CCD covering every allowed CPU, so the result is never empty.
[[nodiscard]] inline std::vector<std::vector<unsigned>> enumerateCcds() {
  return citor::detail::enumerateCcds();
}

/// Runtime probe used by cross-CCD bench TUs from their registrar.
///
/// Throws `std::runtime_error` when `enumerateCcds().size() < required`. The
/// registrar is expected to catch the exception and skip `registerWorkload`,
/// so a single-CCD host quietly omits the cross-CCD rows from the bench output
/// instead of running them on a topology that can not satisfy the workload's
/// intent. The error message names the requested and observed CCD counts so
/// diagnostics on a constrained host (a single CCD due to taskset) are
/// unambiguous.
///
/// required Minimum CCD count the calling TU needs.
/// The CCD count when the probe succeeds.
[[nodiscard]] inline std::size_t requireMultipleCcds(std::size_t required = 2U) {
  const std::size_t observed = enumerateCcds().size();
  if (observed < required) {
    throw std::runtime_error{"cross-CCD bench requires at least " + std::to_string(required) +
                             " CCDs; host reports " + std::to_string(observed)};
  }
  return observed;
}

/// Per-CCD `ThreadPool` arena adapter backed by `PoolGroup::global()`.
///
/// Construction does not own any pools; the underlying arenas live for the
/// process lifetime as part of the singleton. The harness only forwards
/// accessors so a bench TU can dispatch into a specific CCD's arena and the
/// caller can verify each arena's `kind() == PoolKind::Arena`.
///
/// The class deliberately exposes no copy: a moved-from harness would carry a
/// stale arena-count view. Bench TUs should construct one harness per cell.
class MultiArenaHarness {
public:
  /// Acquire a view over the process-wide `PoolGroup::global()` arenas.
  ///
  /// The constructor verifies that every arena reports `PoolKind::Arena`; a
  /// mismatch indicates the singleton has been mis-initialized and the bench
  /// cannot trust later cross-CCD assertions, so the constructor aborts via
  /// the bench's standard `CITOR_ALWAYS_ASSERT` mechanism.
  MultiArenaHarness() : m_group(&citor::PoolGroup::global()) {
    for (std::size_t ccd = 0; ccd < m_group->ccdCount(); ++ccd) {
      if (m_group->arena(ccd).kind() != citor::PoolKind::Arena) {
        std::cerr << "[multi_arena_harness] arena " << ccd
                  << " kind() != PoolKind::Arena; PoolGroup state is corrupt\n";
        std::abort();
      }
    }
  }

  MultiArenaHarness(const MultiArenaHarness &) = delete;
  MultiArenaHarness &operator=(const MultiArenaHarness &) = delete;
  MultiArenaHarness(MultiArenaHarness &&) = delete;
  MultiArenaHarness &operator=(MultiArenaHarness &&) = delete;
  ~MultiArenaHarness() = default;

  /// Number of arenas owned by the underlying `PoolGroup::global()`.
  [[nodiscard]] std::size_t arenaCount() const noexcept { return m_group->ccdCount(); }

  /// Access the per-CCD arena at |ccd|.
  ///
  /// ccd Zero-based CCD index; must be `< arenaCount()`.
  /// Reference to the arena's `ThreadPool`.
  [[nodiscard]] citor::ThreadPool &arena(std::size_t ccd) noexcept { return m_group->arena(ccd); }

  /// Total participants summed across every arena. Used by bench TUs
  ///        that want to size workloads relative to the cross-CCD aggregate.
  [[nodiscard]] std::size_t totalParticipants() const noexcept {
    std::size_t sum = 0;
    for (std::size_t ccd = 0; ccd < m_group->ccdCount(); ++ccd) {
      sum += m_group->arena(ccd).participants();
    }
    return sum;
  }

private:
  citor::PoolGroup *m_group;
};

} // namespace citor::bench
