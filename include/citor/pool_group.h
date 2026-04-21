#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "citor/detail/topology.h"
#include "citor/thread_pool.h"

namespace citor {

/// Process-wide collection of `Arena`-kind `ThreadPool` instances, one per CCD.
///
/// Constructed lazily on the first call to `PoolGroup::global()` via a Meyers function-local
/// static; this side-steps the static initialization order fiasco that plagues classic process-
/// global singletons. Each arena is a normal `ThreadPool` whose workers are pinned to the CPUs of
/// one CCD (or shared-L3 cluster). The arenas have disjoint workers and no cross-arena stealing,
/// matching the N-pools (not oneTBB-style market) decision recorded in the parallel design.
///
/// Deadlock rule (enforced by the cross-arena guard in `ThreadPool`):
///   A thread participating in any `Arena` pool MUST NOT submit synchronous work to a different
///   `Arena` pool. Doing so would block the worker on a queue its own arena does not service. Each
///   primitive's dispatch path checks `shouldFallThroughCrossArena()` and runs the work inline on
///   the caller when the rule would be violated -- the cross-arena call still produces correct
///   results, but it does not parallelize on the target arena.
///
/// Thread-local participant token:
///   Every worker spawned by an `Arena` pool stores its arena index in `ThreadContext::arenaIndex`
///   for the duration of its body. `localArena()` reads the token and returns the calling thread's
///   owning arena, falling back to `arena(0)` when the calling thread is not a `PoolGroup` worker
///   (the producer, a user-spawned `std::thread`, etc.).
class PoolGroup {
public:
  /// Return the process-wide singleton, constructing it on first call.
  ///
  /// Construction is one-shot and thread-safe by virtue of the C++ function-local-static rule:
  /// the initialization runs under an implicit guard variable so concurrent first-callers do not
  /// race. After the first call, every invocation is a barrier-free pointer return.
  ///
  /// Reference to the singleton instance; valid for the rest of the process lifetime.
  static PoolGroup &global() noexcept {
    static PoolGroup instance;
    return instance;
  }

  /// Access the arena pinned to the CPUs of a single CCD.
  ///
  /// ccdIndex Zero-based CCD index; must be `< ccdCount()`.
  /// Reference to the arena's `ThreadPool`; valid for the rest of the process lifetime.
  [[nodiscard]] ThreadPool &arena(std::size_t ccdIndex) noexcept { return *m_arenas[ccdIndex]; }

  /// Number of CCD arenas owned by this group.
  ///
  /// Equal to the size of `detail::enumerateCcds()` at construction time. Always at least 1.
  ///
  /// Arena count.
  [[nodiscard]] std::size_t ccdCount() const noexcept { return m_arenas.size(); }

  /// Return the arena owning the calling thread.
  ///
  /// Reads the `ThreadContext::arenaIndex` participant token. When the calling thread is not a
  /// `PoolGroup` worker (the producer thread or any user-spawned `std::thread`), the function
  /// returns `arena(0)` so callers always get a valid arena to dispatch work to. The cross-arena
  /// deadlock guard in each primitive is what protects the deeper case where an `Arena` worker
  /// accidentally hands a different arena's reference around -- `localArena` is the safe default.
  ///
  /// Reference to the calling thread's owning arena, or `arena(0)` outside any arena.
  [[nodiscard]] ThreadPool &localArena() noexcept {
    const std::size_t hint = currentArenaHint();
    if (hint >= m_arenas.size()) {
      return *m_arenas[0];
    }
    return *m_arenas[hint];
  }

  /// Singleton: copy/move are deleted to keep the function-local-static contract.
  PoolGroup(const PoolGroup &) = delete;
  /// Singleton: copy/move are deleted to keep the function-local-static contract.
  PoolGroup &operator=(const PoolGroup &) = delete;
  /// Singleton: copy/move are deleted to keep the function-local-static contract.
  PoolGroup(PoolGroup &&) = delete;
  /// Singleton: copy/move are deleted to keep the function-local-static contract.
  PoolGroup &operator=(PoolGroup &&) = delete;

private:
  /// Construct one arena per CCD reported by the topology probe.
  ///
  /// Falls back to a single arena over the host's allowed CPU set when sysfs is unavailable; in
  /// that case the resulting `PoolGroup` has `ccdCount() == 1` and behaves like a single
  /// full-machine pool from the caller's point of view.
  PoolGroup() {
    const std::vector<std::vector<unsigned>> ccdCpus = detail::enumerateCcds();
    m_arenas.reserve(ccdCpus.size());
    std::uint32_t arenaIndex = 0;
    for (const auto &cpus : ccdCpus) {
      const std::vector<std::uint32_t> pins(cpus.begin(), cpus.end());
      const std::size_t participants = pins.empty() ? std::size_t{1} : pins.size();
      m_arenas.emplace_back(std::unique_ptr<ThreadPool>(
          new ThreadPool(ThreadPool::ArenaTag{}, participants, pins, arenaIndex)));
      ++arenaIndex;
    }
    if (m_arenas.empty()) {
      // Defensive fallback: enumerateCcds always returns at least one CCD, but if a future
      // platform port returns an empty list, spin up a single-thread arena so callers never see
      // an empty group.
      const std::vector<std::uint32_t> pins;
      m_arenas.emplace_back(std::unique_ptr<ThreadPool>(
          new ThreadPool(ThreadPool::ArenaTag{}, std::size_t{1}, pins, 0U)));
    }
  }

  /// Read the calling thread's arena participant token.
  ///
  /// Wraps `ThreadPool::currentArenaIndexHint`: when the calling thread is a worker on an
  /// `Arena` pool, the helper returns `ThreadContext::arenaIndex`. Otherwise it returns
  /// `kNoArenaHint` so `localArena()` can apply the safe default.
  [[nodiscard]] static std::size_t currentArenaHint() noexcept {
    const std::uint32_t index = ThreadPool::currentArenaIndexHint();
    if (index == kNoArenaSentinel) {
      return kNoArenaHint;
    }
    return static_cast<std::size_t>(index);
  }

  /// Sentinel returned when the calling thread is not a `PoolGroup` worker.
  static constexpr std::size_t kNoArenaHint = static_cast<std::size_t>(-1);

  /// 32-bit sentinel matching `ThreadPool::currentArenaIndexHint`'s "no arena" return.
  static constexpr std::uint32_t kNoArenaSentinel = static_cast<std::uint32_t>(-1);

  /// Owning storage for the per-CCD arenas.
  std::vector<std::unique_ptr<ThreadPool>> m_arenas;
};

} // namespace citor
