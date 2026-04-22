#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "citor/cpos/parallel_for.h"
#include "citor/example_hints.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::BulkBalancedHints;
using citor::CancellationToken;
using citor::Hints;
using citor::ThreadPool;

// Hint presets used by the tests. They live at TU scope (not in an anonymous namespace) so
// clang-tidy treats every static-constexpr member as a public field of a named type rather than
// an unused constant. The presets mirror the named hint types in `example_hints.h`; only the
// fields the dispatch engine inspects are listed.
struct StaticUniformTestHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
};

struct DynamicChunkedTestHints {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 16;
};

struct InlineFallbackHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  /// One ns per item; the gate is `n * 1ns < minTaskUs * participants`. With `minTaskUs = 1000`
  /// the producer runs inline for any `n < 1000 * participants`.
  static constexpr double estimatedItemNs = 1.0;
  static constexpr double minTaskUs = 1000.0;
  static constexpr std::size_t chunk = 0;
};

// Range coverage: every index in [0, n) must be visited exactly once across the per-block ranges.
TEST(ParallelFor, RangeCoverage) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kN);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.parallelFor<StaticUniformTestHints>(0, kN, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      counts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "index " << i;
  }
}

// Blocking semantics: parallelFor returns only after every chunk completes. We stage a side-effect
// (worker writes) that the producer must observe synchronously without an additional fence.
TEST(ParallelFor, BlockingSemantics) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 256;
  std::vector<int> data(kN, 0);

  pool.parallelFor<StaticUniformTestHints>(0, kN, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      data[i] = static_cast<int>(i + 1);
    }
  });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(data[i], static_cast<int>(i + 1));
  }
}

// FunctionRef lifetime: the wrapping lambda goes out of scope after parallelFor returns; the test
// stays inside one TU and asserts no use-after-free is observed (validated under sanitizers).
TEST(ParallelFor, FunctionRefLifetime) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;
  std::vector<int> data(kN, 0);
  {
    // `local` is captured by reference so the FunctionRef thunk reads through a real address;
    // the read survives only because parallelFor blocks until every worker returns from the
    // closure, exercising the descriptor's "body alive for the call" contract end-to-end.
    std::atomic<int> local{5};
    pool.parallelFor<StaticUniformTestHints>(
        0, kN, [&data, &local](std::size_t lo, std::size_t hi) {
          const int value = local.load(std::memory_order_relaxed);
          for (std::size_t i = lo; i < hi; ++i) {
            data[i] = value;
          }
        });
  }
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(data[i], 5);
  }
}

// Cancellation at chunk boundaries: a token stopped mid-flight aborts subsequent chunks.
//
// Skipped on single-participant pools: the inline-fallback path runs the body once over the full
// range, leaving the chunk-boundary cancellation contract no observable surface to exercise.
TEST(ParallelFor, CancellationAtChunkBoundary) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; cancellation at chunk "
                    "boundary has no observable surface";
  }
  constexpr std::size_t kN = 4096;
  CancellationToken tok;
  std::atomic<std::size_t> processed{0};

  pool.parallelFor<DynamicChunkedTestHints>(
      0, kN,
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          processed.fetch_add(1, std::memory_order_relaxed);
        }
        if (processed.load(std::memory_order_relaxed) >= 32) {
          tok.request_stop();
        }
      },
      tok);

  // Cancellation is best-effort at chunk boundaries: at least one chunk runs, and not every chunk
  // ran (the stop was observed before the range was exhausted).
  const std::size_t total = processed.load(std::memory_order_relaxed);
  EXPECT_GT(total, 0U);
  EXPECT_LE(total, kN);
  EXPECT_LT(total, kN) << "cancellation never observed";
}

// Exception propagation: a worker throws -> the producer rethrows on join.
TEST(ParallelFor, ExceptionPropagation) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 256;

  bool threwAsExpected = false;
  try {
    pool.parallelFor<StaticUniformTestHints>(0, kN, [](std::size_t lo, std::size_t /*hi*/) {
      if (lo == 0) {
        throw std::runtime_error("test exception");
      }
    });
  } catch (const std::runtime_error &e) {
    threwAsExpected = std::string{e.what()} == "test exception";
  } catch (...) {
    threwAsExpected = false;
  }
  EXPECT_TRUE(threwAsExpected);
}

// Inline fallback: small n with a high estimated cost runs inline; the body executes once with
// the full range on the producer's thread without waking workers.
TEST(ParallelFor, InlineFallbackForSmallN) {
  ThreadPool pool(4);

  std::atomic<std::size_t> processed{0};
  std::atomic<std::size_t> blockCount{0};
  pool.parallelFor<InlineFallbackHints>(0, 1, [&](std::size_t lo, std::size_t hi) {
    EXPECT_EQ(lo, 0U);
    EXPECT_EQ(hi, 1U);
    processed.fetch_add(1, std::memory_order_relaxed);
    blockCount.fetch_add(1, std::memory_order_relaxed);
  });

  EXPECT_EQ(processed.load(std::memory_order_relaxed), 1U);
  // Exactly one block ran: the producer ran the full range inline rather than partitioning it
  // across workers.
  EXPECT_EQ(blockCount.load(std::memory_order_relaxed), 1U);
}

// Member-and-CPO equivalence: both surfaces produce equivalent observable behavior.
TEST(ParallelFor, MemberAndCpoDispatchAreEquivalent) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 256;
  std::vector<int> dataMember(kN, 0);
  std::vector<int> dataCpo(kN, 0);

  pool.parallelFor<StaticUniformTestHints>(0, kN, [&dataMember](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      dataMember[i] = static_cast<int>(i);
    }
  });

  citor::parallelFor.template operator()<StaticUniformTestHints>(
      pool, 0, kN, [&dataCpo](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          dataCpo[i] = static_cast<int>(i);
        }
      });

  EXPECT_EQ(dataMember, dataCpo);
}

// Static partition is block-strided: with `chunk == 0` (derived) and `participants == 4`,
// the four workers each handle one contiguous block. Verify each (lo, hi) range is touched.
TEST(ParallelFor, StaticPartitionIsBlockStrided) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 4096;
  std::atomic<std::size_t> blockCount{0};

  pool.parallelFor<StaticUniformTestHints>(0, kN, [&](std::size_t lo, std::size_t hi) {
    EXPECT_LT(lo, hi);
    EXPECT_LE(hi, kN);
    blockCount.fetch_add(1, std::memory_order_relaxed);
  });

  // The default chunk derivation produces ceil(N / participants) per block, so the block count is
  // exactly `participants` (every worker gets one contiguous range).
  EXPECT_EQ(blockCount.load(std::memory_order_relaxed), pool.participants());
}

// Dynamic-chunked balances a skewed workload across workers so total wall time is dominated by the
// slowest chunk. This is a smoke test that the dynamic-counter tier completes the full range.
TEST(ParallelFor, DynamicChunkedHandlesSkew) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kN);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.parallelFor<DynamicChunkedTestHints>(0, kN, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      counts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "index " << i;
  }
}

// Repeated dispatches reuse the persistent worker pool: 100 back-to-back calls all complete and
// the cumulative coverage is correct.
TEST(ParallelFor, RepeatedDispatchesReuseWorkers) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;
  std::vector<std::atomic<std::uint32_t>> counts(kN);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  for (int round = 0; round < 100; ++round) {
    pool.parallelFor<StaticUniformTestHints>(0, kN, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) {
        counts[i].fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 100U) << "index " << i;
  }
}

// BulkBalancedHints (the named site preset) is a valid policy type and routes through
// parallelFor without compile errors. This is the "realistic call site" smoke test.
TEST(ParallelFor, BulkBalancedHintCompilesAndRuns) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kN);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.parallelFor<BulkBalancedHints>(0, kN, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      counts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "index " << i;
  }
}

// parallelForRuntime (runtime-hint sibling) routes through the same engine and observably matches
// the member template's behavior.
TEST(ParallelFor, RuntimeHintsMatchCompileTimeHints) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 256;
  std::vector<int> dataCompile(kN, 0);
  std::vector<int> dataRuntime(kN, 0);

  pool.parallelFor<StaticUniformTestHints>(0, kN, [&dataCompile](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      dataCompile[i] = static_cast<int>(i);
    }
  });

  Hints runtimeHints;
  runtimeHints.balance = Balance::StaticUniform;
  runtimeHints.estimatedItemNs = 0.0;
  runtimeHints.minTaskUs = 0.0;
  runtimeHints.chunk = 0;
  pool.parallelForRuntime(
      0, kN,
      [&dataRuntime](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          dataRuntime[i] = static_cast<int>(i);
        }
      },
      runtimeHints);

  EXPECT_EQ(dataCompile, dataRuntime);
}

// A nested `parallelFor` on the same standalone pool from inside an outer body must not
// deadlock on the dispatch mutex. The inner call falls through to inline execution on the
// caller thread.
TEST(ParallelFor, NestedSamePoolCallDoesNotDeadlock) {
  ThreadPool pool(4);
  constexpr std::size_t kOuter = 4;
  constexpr std::size_t kInner = 8;
  std::atomic<int> innerWork{0};

  pool.parallelFor<StaticUniformTestHints>(0, kOuter, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
    pool.parallelFor<StaticUniformTestHints>(0, kInner, [&](std::size_t a, std::size_t b) {
      innerWork.fetch_add(static_cast<int>(b - a), std::memory_order_relaxed);
    });
  });

  EXPECT_EQ(innerWork.load(), static_cast<int>(kOuter * kInner));
}

// A pre-cancelled token on the inline-fallback path must short-circuit before the body runs.
TEST(ParallelFor, InlinePathHonorsPreCancelledToken) {
  ThreadPool pool(1);
  CancellationToken tok;
  tok.request_stop();

  std::atomic<int> calls{0};
  pool.parallelFor<StaticUniformTestHints>(
      0, 100,
      [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        calls.fetch_add(1, std::memory_order_relaxed);
      },
      tok);

  EXPECT_EQ(calls.load(), 0);
}
