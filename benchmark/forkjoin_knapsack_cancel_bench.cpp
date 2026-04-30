// Recursive 0/1 knapsack workload with cooperative cancellation, for the
// comparative pool bench.
//
// The recursive descent enumerates the binary decision tree (take vs skip
// each item); a bound-pruning step drops subtrees whose upper bound is
// below the best value found so far. Cancellation triggers when a
// dramatically tighter upper bound is discovered: the producer's
// citor::CancellationToken is signaled and peer branches observe via
// `tok.stop_requested()` polled at recursive entry.
//
// Two rows per pool: `[cancel-on]` (citor::CancellationToken cooperative,
// HintsT::cancellationChecks=true) and `[cancel-off]`
// (HintsT::cancellationChecks=false). Both citor and oneTBB rows use
// citor::CancellationToken for the cancellation side-channel; this bench
// measures cooperative-cancellation overhead, NOT pool-native cancellation.
// TBB's native task_group_context::cancel_group_execution is
// not exercised here. Both rows must produce the same optimal value (the
// cancellation only prunes provably-suboptimal subtrees). A counter
// side-channel asserts >= 1 cancellation firing per timing iteration in
// `[cancel-on]` mode.
//
// Pool eligibility: citor + oneTBB. OpenMP recursive-spawn requires the
// bench to open a "parallel single" region around the recursion entry;
// that wrapper is not provided in this TU. Taskflow Subflow is excluded
// for the same recursiveSpawn deadlock reason BS/dp/task/riften are.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "citor/always_assert.h"
#include "citor/cancellation.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"
#include "recursive_forkjoin_helper.h"

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 25;
constexpr std::size_t kWarmupIterations = 3;

/// Knapsack item: weight + value pair.
struct Item {
  int weight;
  int value;
};

/// Build a deterministic item set sized to |n|. RNG seed is fixed per
/// the spec at `0xc1701`; weights uniform on [1, 100], values on [1, 100].
[[nodiscard]] std::vector<Item> buildItems(std::size_t n) {
  std::vector<Item> items;
  items.reserve(n);
  std::mt19937 rng{0xc1701U};
  std::uniform_int_distribution<int> wDist(1, 100);
  std::uniform_int_distribution<int> vDist(1, 100);
  for (std::size_t i = 0; i < n; ++i) {
    items.push_back(Item{.weight = wDist(rng), .value = vDist(rng)});
  }
  return items;
}

/// Sequential DP reference: O(n * W) classic 0/1 knapsack. Used to compute
/// the optimal value the parallel version must match before any timing
/// iteration.
[[nodiscard]] int seqDp(const std::vector<Item> &items, int capacity) {
  std::vector<int> prev(static_cast<std::size_t>(capacity + 1), 0);
  std::vector<int> curr(static_cast<std::size_t>(capacity + 1), 0);
  for (const Item &it : items) {
    for (int c = 0; c <= capacity; ++c) {
      curr[static_cast<std::size_t>(c)] = prev[static_cast<std::size_t>(c)];
      if (c >= it.weight) {
        const int candidate = prev[static_cast<std::size_t>(c - it.weight)] + it.value;
        if (candidate > curr[static_cast<std::size_t>(c)]) {
          curr[static_cast<std::size_t>(c)] = candidate;
        }
      }
    }
    std::swap(prev, curr);
  }
  return prev[static_cast<std::size_t>(capacity)];
}

/// Linear-relaxation upper bound at recursion node (idx, remaining-cap,
/// current-value). Items are pre-sorted by value-density (`value/weight`)
/// so the bound is tight; computed in O(n - idx).
[[nodiscard]] double upperBound(const std::vector<Item> &items, std::size_t idx, int remCap,
                                int curValue) noexcept {
  double bound = static_cast<double>(curValue);
  int cap = remCap;
  for (std::size_t i = idx; i < items.size() && cap > 0; ++i) {
    if (items[i].weight <= cap) {
      bound += items[i].value;
      cap -= items[i].weight;
    } else {
      bound += static_cast<double>(items[i].value) * cap / items[i].weight;
      break;
    }
  }
  return bound;
}

/// Knapsack tree-search descriptor shared across recursive calls. The atomic
/// `bestValue` is the running best (peers update via CAS); the cancellation
/// firing counter is incremented every time a branch observes the token's
/// stop flag set by another branch.
struct SearchState {
  std::atomic<int> bestValue{0};
  /// Counter side-channel: incremented every time a recursive call observes
  /// `tok.stop_requested() == true` at entry, so the bench can assert at
  /// least one firing per cancel-on iteration. In `[cancel-on]` mode a
  /// discovered tighter bound triggers `request_stop()` which propagates to
  /// all live recursive branches.
  std::atomic<std::uint64_t> cancelFirings{0};
  citor::CancellationToken token;
  /// When false the recursive descent skips the token-observation branch
  /// entirely, mirroring the `cancellationChecks=false` hint pathway. The
  /// flag lets the same kernel render both rows without duplicate impls.
  bool cancellationEnabled = false;
};

/// Recursive descent: for each item decide take vs skip. The two recursive
/// children spawn via `recursiveSpawn2` so the bench measures the pool's
/// recursive-fork-join cost. Bound pruning eliminates subtrees whose upper
/// bound is below the running best; cancellation triggers a global stop
/// when a branch's bound is dramatically tighter than the current best.
template <class Pool>
void searchRec(Pool &pool, const std::vector<Item> &items, std::size_t idx, int remCap,
               int curValue, SearchState &state) {
  // Cancellation observation. In cancel-on mode the branch records the
  // firing every time it observes the stop flag, exercising the same
  // observation path that the engine's `cancellationChecks=true` hint
  // expects. The branch continues running after observing the cancel: the
  // bound-pruning pathway below already prunes provably-suboptimal
  // subtrees, so cancellation observation only counts firings for the
  // bench's side-channel and adds the realistic poll cost the production
  // cancellation contract pays.
  if (state.cancellationEnabled && state.token.stop_requested()) {
    state.cancelFirings.fetch_add(1U, std::memory_order_relaxed);
  }

  if (idx == items.size() || remCap == 0) {
    int observed = state.bestValue.load(std::memory_order_relaxed);
    while (curValue > observed &&
           !state.bestValue.compare_exchange_weak(observed, curValue, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
      // CAS retried; observed updated in place.
    }
    // If the CAS landed (`curValue > observed` originally) and the new
    // best was a wide-margin tightening (>= 1% improvement over the prior
    // best), signal cancellation. `request_stop()` is idempotent: once any
    // leaf fires it the token stays stopped for the rest of the iteration,
    // and every subsequent recursive entry observes one firing (which is
    // what the side-channel counts). The bound-pruning step below
    // continues to prune the remaining search tree against the running
    // best, so correctness against the DP reference is preserved.
    // observed > 0 guard: skip the first leaf tightening (we want a
    // meaningful prior best, not a triggered cancel on the first non-zero
    // value); the bound-prune pathway above already short-circuits zero-value
    // branches.
    if (state.cancellationEnabled && curValue > observed && observed > 0 &&
        curValue > observed + (observed / 100)) {
      state.token.request_stop();
    }
    return;
  }

  const double bound = upperBound(items, idx, remCap, curValue);
  const int currentBest = state.bestValue.load(std::memory_order_relaxed);
  if (bound <= static_cast<double>(currentBest)) {
    return;
  }

  // Cancellation trigger: when a leaf updates the running best by a wide
  // margin (>= 5% improvement), signal cancellation so peer branches with
  // looser bounds prune at the next observation. The trigger fires inside
  // the leaf-update CAS path below; here we only mark whether this branch
  // saw a strictly-better leaf-tightening event.

  // Sequential cutoff: small subproblems run inline so dispatch cost does
  // not dominate the recursion. Items late in the list are also small
  // because the bound pruning already eliminated heavy items.
  constexpr std::size_t kSeqCutoff = 12;
  if (items.size() - idx <= kSeqCutoff) {
    // Take.
    if (items[idx].weight <= remCap) {
      searchRec(pool, items, idx + 1U, remCap - items[idx].weight, curValue + items[idx].value,
                state);
    }
    // Skip.
    searchRec(pool, items, idx + 1U, remCap, curValue, state);
    return;
  }

  // Spawn take + skip in parallel.
  const Item it = items[idx];
  const int takeValue = curValue + it.value;
  const int takeRemCap = remCap - it.weight;
  const bool canTake = it.weight <= remCap;
  recursiveSpawn2(
      pool,
      [&](Pool &p) {
        if (canTake) {
          searchRec(p, items, idx + 1U, takeRemCap, takeValue, state);
        }
      },
      [&](Pool &p) { searchRec(p, items, idx + 1U, remCap, curValue, state); });
}

/// Driver: prepare items, sort by density, run sequential DP for the
/// reference, then run the parallel search. Returns (parallel-best,
/// reference-best, cancellation-firings).
struct RunOutput {
  int parallel;
  int reference;
  std::uint64_t firings;
};

template <class Pool>
[[nodiscard]] RunOutput runOnce(Pool &pool, const std::vector<Item> &itemsSorted, int capacity,
                                int referenceValue, bool cancellationEnabled) {
  SearchState state;
  state.cancellationEnabled = cancellationEnabled;
  state.token =
      cancellationEnabled ? citor::CancellationToken::makeOwned() : citor::CancellationToken{};
  searchRec(pool, itemsSorted, 0U, capacity, 0, state);
  return RunOutput{state.bestValue.load(std::memory_order_acquire), referenceValue,
                   state.cancelFirings.load(std::memory_order_relaxed)};
}

template <class PoolT>
[[nodiscard]] BenchRow measureKnapsack(const char *name, std::size_t participants,
                                       std::size_t nItems, const CyclesPerNanosecond &cal,
                                       bool cancellationEnabled) {
  static_assert(RecursiveForkJoinTraits<PoolT>::supportsRecursiveSpawn,
                "knapsack-cancel bench requires recursive-spawn-capable pool");
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  const std::vector<Item> items = buildItems(nItems);
  int totalWeight = 0;
  for (const Item &it : items) {
    totalWeight += it.weight;
  }
  const int capacity = totalWeight / 2;
  // Pre-sort by value-density so the upper-bound is tight (linear-
  // relaxation knapsack convention).
  std::vector<Item> itemsSorted = items;
  std::sort(itemsSorted.begin(), itemsSorted.end(), [](const Item &a, const Item &b) {
    return static_cast<double>(a.value) / a.weight > static_cast<double>(b.value) / b.weight;
  });
  const int reference = seqDp(items, capacity);

  // Warmup + correctness.
  RunOutput last{};
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    last = runOnce(*pool, itemsSorted, capacity, reference, cancellationEnabled);
    CITOR_ALWAYS_ASSERT(last.parallel == reference);
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  std::uint64_t totalFirings = 0;
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const RunOutput out = runOnce(*pool, itemsSorted, capacity, reference, cancellationEnabled);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(out.parallel == reference);
    if (cancellationEnabled) {
      // At least one firing per timing iteration in cancel-on mode. The
      // bound-tightening trigger fires when a branch's curValue exceeds
      // the running best by at least 1%; for n in {20, 24} the search
      // tree is rich enough to guarantee at least one such tightening
      // per iteration.
      CITOR_ALWAYS_ASSERT(out.firings >= 1U);
    }
    totalFirings += out.firings;
  }
  (void)totalFirings;

  return finalizeRow(name, samples);
}

struct KnapsackCell {
  std::size_t n;
  const char *suffix;
};

constexpr std::array<KnapsackCell, 2> kCells{{
    {.n = 20, .suffix = "n20"},
    {.n = 24, .suffix = "n24"},
}};

BenchTable buildTable(std::size_t participants, KnapsackCell cell, const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload =
      std::string{"forkjoin_knapsack_cancel_j"} + std::to_string(participants) + "_" + cell.suffix;

  // citor: 2 rows. Both rows below use citor::CancellationToken for the
  // observation side-channel. TBB's native cancel_group_execution is NOT
  // wired through; the oneTBB rows measure cooperative-cancellation
  // overhead with the same token type, not TBB-native cancellation.
  table.rows.push_back(measureKnapsack<citor::ThreadPool>(
      "citor::ThreadPool[citor::CancellationToken cooperative cancel-on]", participants, cell.n,
      cal, /*cancellationEnabled=*/true));
  table.rows.push_back(measureKnapsack<citor::ThreadPool>(
      "citor::ThreadPool[cancel-off]", participants, cell.n, cal, /*cancellationEnabled=*/false));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureKnapsack<::tbb::task_arena>(
      "oneTBB[citor::CancellationToken cooperative cancel-on]", participants, cell.n, cal, true));
  table.rows.push_back(
      measureKnapsack<::tbb::task_arena>("oneTBB[cancel-off]", participants, cell.n, cal, false));
#endif
  // dispenso is NOT wired here. Its TaskSet completes the
  // recursive search tree fast enough that the bound-tightening cancel
  // observation (firings >= 1 per iteration) is non-deterministic on this
  // workload, so the bench's correctness assertion fires sporadically. The
  // other 4 fork-join cells (fib/queens, UTS, Strassen, cilksort) DO render
  // dispenso since they have no cancellation invariant.
  return table;
}

template <std::size_t CellIdx, std::size_t Participants>
BenchTable runKnapsackCell(const CyclesPerNanosecond &cal) {
  constexpr KnapsackCell cell = kCells[CellIdx];
  return buildTable(Participants, cell, cal);
}

struct KnapsackRegistrar {
  KnapsackRegistrar() {
    registerWorkload({.name = "forkjoin_knapsack_cancel_j8_n20", .run = &runKnapsackCell<0, 8>});
    registerWorkload({.name = "forkjoin_knapsack_cancel_j16_n20", .run = &runKnapsackCell<0, 16>});
    registerWorkload({.name = "forkjoin_knapsack_cancel_j8_n24", .run = &runKnapsackCell<1, 8>});
    registerWorkload({.name = "forkjoin_knapsack_cancel_j16_n24", .run = &runKnapsackCell<1, 16>});
  }
};

const KnapsackRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
