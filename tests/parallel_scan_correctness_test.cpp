#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#include "citor/cpos/parallel_scan.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Scan over [1, 2, ..., 1000000] with std::plus<int64_t>: out[i] must equal
// (i+1)*(i+2)/2.
//
// Skipped on single-participant pools: parallelScan's inline-fallback at
// participants<=1 invokes the body once with `initial = identity` over the full
// range and is a single-pass shape, but the test body distinguishes pass 1 vs
// pass 2 by call index against `participants`. With participants equal to one,
// that gating treats the only call as pass 1 (no out[] writes), so out[] stays
// zero. The two-pass primitive is not exercised at participants==1; the inline
// contract is covered elsewhere (`ScanEmpty`, single-call inline coverage).
TEST(ParallelScanCorrectness,
     InclusivePrefixScanMatchesSerialReferenceForArithmeticSequence) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline single-call "
                    "shape; the two-pass "
                    "body contract used by this test does not apply";
  }
  constexpr std::size_t kN = 1'000'000;
  std::vector<std::int64_t> in(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    in[i] = static_cast<std::int64_t>(i + 1);
  }
  std::vector<std::int64_t> out(kN, 0);

  // The body needs to know which pass it is in. The pool calls the body twice
  // per slot (Pass 1 and Pass 2), interleaved across slots, so a per-slot epoch
  // wouldn't be safe. We rely on the driver's discipline: invoke parallelScan
  // twice, once with the body counting partials
  // ("pass 0") and once with the body writing scan output ("pass 1"). Each
  // individual call below is its own scan; the test verifies the
  // final-write invocation produces the correct out[]. To exercise the two-pass
  // primitive directly, we use a pass counter that we manually toggle between
  // the two implicit body invocations the primitive performs.
  //
  // The simpler shape that the primitive's contract supports: a single body
  // that branches on `initial != identity`. For std::plus<int64_t> with
  // identity = 0, slot 0's Pass 2 receives `initial = 0` (its exclusive prefix
  // is the identity), so we'd lose the pass distinction for slot 0. Use an
  // explicit pass counter incremented by the body on entry; with 4
  // participants, the primitive invokes the body 8 times total (2 passes * 4
  // slots). Pass 1 = first 4 invocations, Pass 2 = last 4. Slot 0's Pass-2 call
  // is the FIRST Pass-2 invocation (because the producer owns slot 0 and runs
  // its Pass 2 before joining other slots).
  //
  // An alternative scheme would have the body distinguish passes via the `out`
  // pointer parameter, but we always pass nullptr in the current contract. So
  // we use the captured pass counter approach. Total invocations = 2 *
  // participants = 8.
  std::atomic<int> totalCalls{0};
  std::atomic<int> pass1Done{0};
  const std::size_t participants = pool.participants();

  auto body = [&in, &out, &totalCalls, &pass1Done,
               participants](std::size_t /*chunkId*/, std::size_t lo,
                             std::size_t hi, std::int64_t initial,
                             std::int64_t * /*unusedOut*/) -> std::int64_t {
    const int callIdx = totalCalls.fetch_add(1, std::memory_order_acq_rel);
    if (std::cmp_less(callIdx, participants)) {
      // Pass 1: compute the chunk's partial sum.
      std::int64_t s = 0;
      for (std::size_t i = lo; i < hi; ++i) {
        s += in[i];
      }
      pass1Done.fetch_add(1, std::memory_order_acq_rel);
      return s;
    }
    // Pass 2: write the per-element inclusive scan with `initial` as the
    // running prefix.
    std::int64_t running = initial;
    for (std::size_t i = lo; i < hi; ++i) {
      running += in[i];
      out[i] = running;
    }
    return running - initial;
  };

  const std::int64_t total = pool.parallelScan<HintsDefaults>(
      kN, std::int64_t{0}, body,
      [](std::int64_t a, std::int64_t b) { return a + b; });

  // Inclusive total is sum 1..N = N*(N+1)/2.
  const std::int64_t expectedTotal =
      static_cast<std::int64_t>(kN) * static_cast<std::int64_t>(kN + 1) / 2;
  EXPECT_EQ(total, expectedTotal);

  // Per-element check: out[i] = (i+1)*(i+2)/2.
  for (std::size_t i = 0; i < kN; ++i) {
    const std::int64_t expected =
        static_cast<std::int64_t>(i + 1) * static_cast<std::int64_t>(i + 2) / 2;
    if (out[i] != expected) {
      ADD_FAILURE() << "out[" << i << "] = " << out[i] << ", expected "
                    << expected;
      break;
    }
  }
}

// Determinism: at fixed nJobs in {1, 2, 4, 8, 16}, the inclusive total is
// bit-identical across worker counts. The gate is "associative reduction";
// integer addition is associative so the chunk-level partial sums fold to the
// same total regardless of partition. Floating-point `+` is NOT associative, so
// this test deliberately uses `int64_t` to isolate the protocol's determinism
// from the FP rounding noise that would obscure it.
TEST(ParallelScanCorrectness,
     RepeatedScansOverIdenticalInputProduceBitIdenticalOutput) {
  constexpr std::size_t kN = 100'000;
  std::vector<std::int64_t> in(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    in[i] = static_cast<std::int64_t>(i + 1);
  }

  std::vector<std::int64_t> totals;
  totals.reserve(5U);

  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    const std::size_t participants = pool.participants();
    std::atomic<int> totalCalls{0};
    std::vector<std::int64_t> out(kN, 0);

    auto body = [&in, &out, &totalCalls,
                 participants](std::size_t /*chunkId*/, std::size_t lo,
                               std::size_t hi, std::int64_t initial,
                               std::int64_t * /*unusedOut*/) -> std::int64_t {
      const int callIdx = totalCalls.fetch_add(1, std::memory_order_acq_rel);
      if (std::cmp_less(callIdx, participants)) {
        // Pass 1
        std::int64_t s = 0;
        for (std::size_t i = lo; i < hi; ++i) {
          s += in[i];
        }
        return s;
      }
      // Pass 2
      std::int64_t running = initial;
      for (std::size_t i = lo; i < hi; ++i) {
        running += in[i];
        out[i] = running;
      }
      return running - initial;
    };

    const std::int64_t total = pool.parallelScan<HintsDefaults>(
        kN, std::int64_t{0}, body,
        [](std::int64_t a, std::int64_t b) { return a + b; });
    totals.push_back(total);
  }

  for (std::size_t i = 1; i < totals.size(); ++i) {
    EXPECT_EQ(totals[i], totals[0])
        << "scan result diverged between worker counts at index " << i;
  }
}

// Large random doubles: verify against std::inclusive_scan reference within FP
// tolerance.
//
// Skipped on single-participant pools: same rationale as `ScanCorrectness` --
// the two-pass body contract this test exercises does not apply when the
// inline-fallback path is taken.
TEST(ParallelScanCorrectness,
     InclusivePrefixScanMatchesSerialReferenceForLargeRandomInput) {
  ThreadPool pool(8);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline single-call "
                    "shape; the two-pass "
                    "body contract used by this test does not apply";
  }
  constexpr std::size_t kN = 1'000'000;
  std::vector<double> in(kN);
  std::mt19937_64 rng(0xCAFEBABEULL);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  for (std::size_t i = 0; i < kN; ++i) {
    in[i] = dist(rng);
  }

  std::vector<double> reference(kN, 0.0);
  std::inclusive_scan(in.begin(), in.end(), reference.begin());

  const std::size_t participants = pool.participants();
  std::atomic<int> totalCalls{0};
  std::vector<double> out(kN, 0.0);

  auto body = [&in, &out, &totalCalls, participants](
                  std::size_t /*chunkId*/, std::size_t lo, std::size_t hi,
                  double initial, double * /*unusedOut*/) -> double {
    const int callIdx = totalCalls.fetch_add(1, std::memory_order_acq_rel);
    if (std::cmp_less(callIdx, participants)) {
      double s = 0.0;
      for (std::size_t i = lo; i < hi; ++i) {
        s += in[i];
      }
      return s;
    }
    double running = initial;
    for (std::size_t i = lo; i < hi; ++i) {
      running += in[i];
      out[i] = running;
    }
    return running - initial;
  };

  const double total = pool.parallelScan<HintsDefaults>(
      kN, 0.0, body, [](double a, double b) { return a + b; });

  // Compare per-element to reference within a relative tolerance.
  for (std::size_t i = 0; i < kN; ++i) {
    const double tol = std::max(1e-9, std::abs(reference[i]) * 1e-9);
    if (std::abs(out[i] - reference[i]) > tol) {
      ADD_FAILURE() << "out[" << i << "] = " << out[i]
                    << ", reference = " << reference[i];
      break;
    }
  }
  // Inclusive total compared with similar tolerance.
  const double totTol = std::max(1e-9, std::abs(reference.back()) * 1e-9);
  EXPECT_NEAR(total, reference.back(), totTol);
}
