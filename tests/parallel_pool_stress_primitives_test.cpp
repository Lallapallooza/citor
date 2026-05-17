#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "citor/chain.h"
#include "citor/cpos/fork_join.h"
#include "citor/cpos/parallel_scan.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::BarrierKind;
using citor::ChainHintsDefaults;
using citor::HintsDefaults;
using citor::KahanReduceHints;
using citor::makeStage;
using citor::staticStage;
using citor::ThreadPool;

// Hint presets at TU scope so clang-tidy treats every static-constexpr member
// as a public field of a named type rather than an unused constant.
struct StressDynamicHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr std::size_t chunk = 16;
};

struct StressForkJoinHints : HintsDefaults {
  static constexpr citor::StealPolicy stealPolicy =
      citor::StealPolicy::ClusterLocal;
};

// Cross-primitive stress: each iteration constructs a fresh pool with a
// randomized participant count, then exercises one of every public primitive
// with random shapes. Under ThreadSanitizer the contract is "TSan does not
// flag a race on any primitive's rendezvous"; under native the contract is
// "no primitive crashes, deadlocks, or returns NaN under random shapes".
//
// Skipped under ThreadSanitizer pending LLVM issue 177529: runPlex /
// parallelChain / dispatch join paths rendezvous via tight
// `atomic.load(acquire)` spins on a single shared atomic. TSan instruments
// every atomic op through a per-address shadow-memory mutex
// (`__sanitizer::Mutex`, `compiler-rt/lib/sanitizer_common/sanitizer_mutex.h`)
// that is reader-preferring; N spinning readers monopolize the reader lock
// and the producer's `atomic.store(release)` parks indefinitely in
// `__sanitizer::Semaphore::Wait` inside `__tsan_atomic64_store`. The
// behaviour is also documented at `google/sanitizers` issue 1552. Re-enable
// the TSan run when the upstream fairness issue is resolved; native
// validation continues to run on every CI build.
TEST(ParallelPoolStressPrimitives,
     EveryPrimitiveUnderRandomizedParticipantCountsTerminatesCleanly) {
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
  GTEST_SKIP()
      << "Pending LLVM issue 177529: TSan's reader-preferring metaslot mutex "
         "starves the producer's atomic.store on rendezvous primitives.";
#endif
#endif
#ifdef __SANITIZE_THREAD__
  GTEST_SKIP()
      << "Pending LLVM issue 177529: TSan's reader-preferring metaslot mutex "
         "starves the producer's atomic.store on rendezvous primitives.";
#endif
  // 10000 iterations is the target. Each iteration constructs and destructs
  // a pool plus a single primitive call so the loop fits inside ctest's
  // per-test timeout under a TSan build. A wall-clock cap bounds total runtime
  // when a single iteration is unusually slow under TSan (e.g. heavy steal
  // traffic at j=16 makes the per-iter cost vary by an order of magnitude).
  constexpr int kIterations = 10000;
  constexpr std::chrono::seconds kWallBudget{300};
  const auto loopStart = std::chrono::steady_clock::now();

  std::mt19937_64 rng(0xDEADBEEFCAFEBABEULL);
  std::uniform_int_distribution<std::size_t> participantsDist(1U, 16U);
  std::uniform_int_distribution<int> primitiveDist(0, 7);
  std::uniform_int_distribution<std::size_t> sizeDist(0U, 256U);

  // Persistent buffers reused across iterations so workers race on the same
  // memory; this is the shape the sanitizer needs to detect a missing
  // happens-before edge between producer and worker.
  std::vector<std::atomic<std::uint32_t>> sharedCounts(256);
  for (auto &c : sharedCounts) {
    c.store(0, std::memory_order_relaxed);
  }
  std::vector<double> sharedData(1024);
  for (std::size_t i = 0; i < sharedData.size(); ++i) {
    sharedData[i] = static_cast<double>(i) * 0.001;
  }

  for (int iter = 0; iter < kIterations; ++iter) {
    if ((iter & 0xFF) == 0 &&
        std::chrono::steady_clock::now() - loopStart >= kWallBudget) {
      GTEST_LOG_(INFO) << "Stress wall budget reached at iter=" << iter << "/"
                       << kIterations;
      break;
    }
    const std::size_t participants = participantsDist(rng);
    ThreadPool pool(participants);
    const int primitive = primitiveDist(rng);
    const std::size_t n = sizeDist(rng);

    switch (primitive) {
    case 0: {
      // parallelFor with concurrent body-side state mutation; workers race on
      // relaxed atomics.
      pool.parallelFor<HintsDefaults>(
          0, n, [&sharedCounts, n](std::size_t lo, std::size_t hi) {
            const std::size_t cap = sharedCounts.size();
            for (std::size_t i = lo; i < hi && i < n && i < cap; ++i) {
              sharedCounts[i].fetch_add(1, std::memory_order_relaxed);
            }
          });
      break;
    }
    case 1: {
      // parallelFor with DynamicChunked balance to exercise the second dispatch
      // tier.
      pool.parallelFor<StressDynamicHints>(
          0, n, [&sharedCounts, n](std::size_t lo, std::size_t hi) {
            const std::size_t cap = sharedCounts.size();
            for (std::size_t i = lo; i < hi && i < n && i < cap; ++i) {
              sharedCounts[i].fetch_add(1, std::memory_order_relaxed);
            }
          });
      break;
    }
    case 2: {
      // parallelReduce with FixedBlockOrder + Kahan via the KahanReduceHints
      // named hint. The producer joins on every chunk, then reads partials
      // back; TSan would flag any missing happens-before edge between the
      // worker's slot write and the producer's tree combine.
      const std::size_t reduceN = std::min(sharedData.size(), n);
      if (reduceN > 0U) {
        const double total = pool.parallelReduce<KahanReduceHints>(
            0, reduceN, 0.0,
            [&sharedData](std::size_t lo, std::size_t hi) {
              double s = 0.0;
              for (std::size_t i = lo; i < hi; ++i) {
                s += sharedData[i];
              }
              return s;
            },
            [](double a, double b) { return a + b; });
        // Read the result so TSan must observe the producer's combine completed
        // before the read.
        EXPECT_FALSE(std::isnan(total));
      } else {
        const double total = pool.parallelReduce<HintsDefaults>(
            0, 0, 42.0, [](std::size_t, std::size_t) { return 0.0; },
            [](double a, double b) { return a + b; });
        EXPECT_EQ(total, 42.0);
      }
      break;
    }
    case 3: {
      // runPlex<3> with a pre-phase hook so the producer runs serial
      // bookkeeping between phases; the sanitizer must not flag the prePhase ->
      // phaseFn happens-before edge.
      constexpr std::size_t kPhases = 3;
      std::int64_t phaseTarget = 0;
      pool.runPlex<HintsDefaults>(
          kPhases, n,
          [&sharedCounts, &phaseTarget, n](std::size_t /*phaseIdx*/,
                                           std::uint32_t /*slot*/,
                                           std::size_t lo, std::size_t hi) {
            const std::size_t cap = sharedCounts.size();
            for (std::size_t i = lo; i < hi && i < n && i < cap; ++i) {
              sharedCounts[i].fetch_add(
                  static_cast<std::uint32_t>(phaseTarget) | 1U,
                  std::memory_order_relaxed);
            }
          },
          [&phaseTarget](std::size_t phaseIdx) {
            phaseTarget = static_cast<std::int64_t>(phaseIdx);
          });
      break;
    }
    case 4: {
      // bulkForQueries with mixed query lengths; the chunked dispatch tier
      // exercises atomic nextBlock counter contention.
      const std::size_t cap = sharedCounts.size();
      pool.bulkForQueries<StressDynamicHints>(
          n, [&sharedCounts, cap](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi && i < cap; ++i) {
              sharedCounts[i].fetch_add(1, std::memory_order_relaxed);
            }
          });
      break;
    }
    case 5: {
      // parallelChain with empty stages limited to j<=8 to keep the chain time
      // short. Both stages are no-op so this stresses the barrier protocol
      // itself; TSan must not flag the sense-reversing or per-slot done-epoch
      // synchronization.
      const std::size_t chainN = std::min<std::size_t>(n, 256);
      const std::size_t chainParticipants =
          std::min<std::size_t>(participants, 8U);
      if (chainParticipants == participants) {
        pool.parallelChain<ChainHintsDefaults>(
            chainN,
            staticStage("chain-empty-a",
                        [](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                           std::size_t /*lo*/, std::size_t /*hi*/) noexcept {}),
            makeStage<BarrierKind::Global>(
                [](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                   std::size_t /*lo*/, std::size_t /*hi*/) noexcept {}));
      } else {
        // For pools larger than 8 participants, drop to a single-stage chain so
        // the per-iteration chain time stays bounded; this path still exercises
        // construction/destruction at the larger participant counts.
        pool.parallelChain<ChainHintsDefaults>(
            chainN, staticStage("chain-empty-single",
                                [](std::size_t /*stageIdx*/,
                                   std::uint32_t /*slot*/, std::size_t /*lo*/,
                                   std::size_t /*hi*/) noexcept {}));
      }
      break;
    }
    case 6: {
      // parallelScan two-pass dispatch: workers race on the per-chunk
      // done-epoch ladder and on `prefixesPublished`. The body counts chunk
      // invocations through `sharedCounts` so TSan must observe the
      // publish/join happens-before edge between Pass 1 and Pass 2.
      const std::size_t scanN = std::min<std::size_t>(n, sharedCounts.size());
      if (scanN > 0U) {
        const std::size_t scanParticipants = pool.participants();
        std::atomic<int> totalCalls{0};
        const std::int64_t total = pool.parallelScan<HintsDefaults>(
            scanN, std::int64_t{0},
            [&sharedCounts, &totalCalls,
             scanParticipants](std::size_t /*chunkId*/, std::size_t lo,
                               std::size_t hi, std::int64_t initial,
                               std::int64_t * /*unusedOut*/) -> std::int64_t {
              const int callIdx =
                  totalCalls.fetch_add(1, std::memory_order_acq_rel);
              if (std::cmp_less(callIdx, scanParticipants)) {
                std::int64_t s = 0;
                for (std::size_t i = lo; i < hi; ++i) {
                  sharedCounts[i].fetch_add(1, std::memory_order_relaxed);
                  s += 1;
                }
                return s;
              }
              std::int64_t running = initial;
              for (std::size_t i = lo; i < hi; ++i) {
                running += 1;
              }
              return running - initial;
            },
            [](std::int64_t a, std::int64_t b) { return a + b; });
        // Read the result so TSan must observe the producer's combine completed
        // before the read.
        EXPECT_EQ(total, static_cast<std::int64_t>(scanN));
      }
      break;
    }
    case 7: {
      // forkJoin recursive task fan-out: workers pop from their own deque, fall
      // back to stealing from CCD-local victims. The sanitizer must observe the
      // deque release/acquire pair on push and on the steal CAS, plus the
      // pendingTasks counter's release-decrement closing every task body's
      // writes against the producer's join-side acquire-load.
      std::array<std::atomic<int>, 8> taskCounts{};
      pool.forkJoin<StressForkJoinHints>(
          [&]() { taskCounts[0].fetch_add(1, std::memory_order_relaxed); },
          [&]() { taskCounts[1].fetch_add(1, std::memory_order_relaxed); },
          [&]() { taskCounts[2].fetch_add(1, std::memory_order_relaxed); },
          [&]() { taskCounts[3].fetch_add(1, std::memory_order_relaxed); },
          [&]() { taskCounts[4].fetch_add(1, std::memory_order_relaxed); },
          [&]() { taskCounts[5].fetch_add(1, std::memory_order_relaxed); },
          [&]() { taskCounts[6].fetch_add(1, std::memory_order_relaxed); },
          [&]() { taskCounts[7].fetch_add(1, std::memory_order_relaxed); });
      for (auto &c : taskCounts) {
        EXPECT_EQ(c.load(), 1);
      }
      break;
    }
    default:
      break;
    }
    // Pool destruction at the end of each iteration's scope exercises the
    // shutdown release/acquire chain under contention; the sanitizer must not
    // flag the worker-join edge.
  }

  // Read the shared buffer once after the loop so TSan registers the post-loop
  // happens-before edge from the final pool's destruction.
  std::uint64_t total = 0;
  for (auto &c : sharedCounts) {
    total += c.load(std::memory_order_relaxed);
  }
  EXPECT_GE(total, 0U);
}
