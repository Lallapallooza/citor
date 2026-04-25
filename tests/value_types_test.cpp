#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

#include "citor/cancellation.h"
#include "citor/example_hints.h"
#include "citor/function_ref.h"
#include "citor/hints.h"

using citor::Affinity;
using citor::Balance;
using citor::BarrierKind;
using citor::CancellationToken;
using citor::cancelled_exception;
using citor::cancelled_value_exception;
using citor::ChainHints;
using citor::Deadline;
using citor::Determinism;
using citor::FunctionRef;
using citor::Hints;
using citor::kCacheLine;
using citor::makeStage;
using citor::Partition;
using citor::Priority;
using citor::Stage;

// 16-byte FunctionRef invariant (compile-time).
static_assert(sizeof(FunctionRef<void(std::size_t, std::size_t)>) == 16,
              "FunctionRef<void(size_t, size_t)> must be 16 bytes");
static_assert(sizeof(FunctionRef<int(int)>) == 16, "FunctionRef<int(int)> must be 16 bytes");
static_assert(sizeof(FunctionRef<void()>) == 16, "FunctionRef<void()> must be 16 bytes");

// ===== Trivial-copy invariant: lets the descriptor sit in registers across boundaries =====
static_assert(std::is_trivially_copyable_v<FunctionRef<void()>>,
              "FunctionRef must be trivially copyable");
static_assert(std::is_trivially_destructible_v<FunctionRef<void()>>,
              "FunctionRef must be trivially destructible");

// Example presets expose at minimum the `balance` and `priority` constexpr fields and route
// through the generic `Hints` concept. Additional fields are preset-specific and not asserted
// here; users pick a preset and tune the constants for their call site.
static_assert(citor::BulkBalancedHints::balance == Balance::StaticUniform,
              "BulkBalancedHints::balance must be StaticUniform at compile time");
static_assert(citor::KahanReduceHints::determinism == Determinism::KahanCompensated,
              "KahanReduceHints::determinism must be KahanCompensated");
static_assert(citor::ChainBalancedHints::balance == Balance::StaticUniform,
              "ChainBalancedHints::balance must be StaticUniform");

// ===== Stage / ChainHints structural sanity =====
static_assert(Stage<int (*)(std::size_t, std::size_t), BarrierKind::PerChunk>::barrier ==
                  BarrierKind::PerChunk,
              "Stage::barrier must reflect the After non-type template parameter");

// ===== kCacheLine is hardcoded to 128, never the std value =====
static_assert(kCacheLine == 128, "kCacheLine must be hardcoded to 128 bytes");

// Cancellation API noexcept contract.
static_assert(noexcept(std::declval<const CancellationToken &>().stop_requested()),
              "CancellationToken::stop_requested must be noexcept");
static_assert(noexcept(std::declval<CancellationToken &>().request_stop()),
              "CancellationToken::request_stop must be noexcept");
static_assert(noexcept(std::declval<const Deadline &>().expired()),
              "Deadline::expired must be noexcept");

// Runtime confirmation in addition to the compile-time static_assert.
TEST(ParallelFunctionRef, SizeIsExactlySixteenBytes) {
  EXPECT_EQ(sizeof(FunctionRef<void(std::size_t, std::size_t)>), 16U);
  EXPECT_EQ(sizeof(FunctionRef<int(int)>), 16U);
}

// FunctionRef must round-trip a stateful lambda: capture by reference and observe a side effect.
TEST(ParallelFunctionRef, RoundTripsStatefulLambda) {
  int counter = 0;
  auto inc = [&counter](std::size_t lo, std::size_t hi) noexcept {
    counter += static_cast<int>(hi - lo);
  };
  const FunctionRef<void(std::size_t, std::size_t)> ref = inc;
  ref(0U, 5U);
  ref(5U, 10U);
  EXPECT_EQ(counter, 10);
}

// FunctionRef returns the bound callable's value.
TEST(ParallelFunctionRef, ForwardsReturnValue) {
  auto square = [](int x) { return x * x; };
  const FunctionRef<int(int)> ref = square;
  EXPECT_EQ(ref(7), 49);
}

// Default-constructed FunctionRef is empty and contextually converts to false.
TEST(ParallelFunctionRef, DefaultConstructedIsEmpty) {
  const FunctionRef<void()> empty;
  EXPECT_FALSE(static_cast<bool>(empty));
}

// Runtime check on top of the static_assert: the constexpr field should be observable.
TEST(ParallelHints, BulkBalancedHintsBalanceIsStaticUniform) {
  using HintsT = citor::BulkBalancedHints;
  EXPECT_EQ(HintsT::balance, Balance::StaticUniform);
  EXPECT_EQ(HintsT::affinity, Affinity::SplitCcd);
  EXPECT_TRUE(HintsT::tlsRequired);
}

// Runtime Hints POD must be default-constructible with the documented defaults.
TEST(ParallelHints, RuntimeHintsHaveDocumentedDefaults) {
  const Hints h{};
  EXPECT_EQ(h.balance, Balance::StaticUniform);
  EXPECT_EQ(h.determinism, Determinism::FixedBlockOrder);
  EXPECT_EQ(h.affinity, Affinity::None);
  EXPECT_EQ(h.priority, Priority::Throughput);
  EXPECT_EQ(h.partition, Partition::ContiguousRanges);
  EXPECT_TRUE(h.allowProducer);
  EXPECT_TRUE(h.fpDeterministicTree);
  EXPECT_TRUE(h.cancellationChecks);
  EXPECT_FALSE(h.allowWorkerSteal);
  EXPECT_FALSE(h.allowNestedParallelism);
}

// Stage helper produces a Stage<F, BarrierKind> deducing F from the lambda.
TEST(ParallelHints, StageHelperDeducesCallable) {
  auto stage = makeStage<BarrierKind::Global>([](std::size_t, std::size_t) {});
  EXPECT_EQ(decltype(stage)::barrier, BarrierKind::Global);
}

TEST(ParallelHints, ChainHintsDefaults) {
  const ChainHints ch{};
  EXPECT_EQ(ch.balance, Balance::StaticUniform);
  EXPECT_TRUE(ch.pipelineSameChunk);
  EXPECT_TRUE(ch.fpDeterministicTree);
}

// CancellationToken returns false initially; after request_stop, returns true.
TEST(ParallelCancellation, TokenStartsNotStoppedAndTransitions) {
  CancellationToken tok = CancellationToken::makeOwned();
  EXPECT_FALSE(tok.stop_requested());
  EXPECT_TRUE(tok.request_stop());
  EXPECT_TRUE(tok.stop_requested());
}

// Subsequent request_stop calls return false (the first call wins the CAS).
TEST(ParallelCancellation, SecondRequestStopReturnsFalse) {
  CancellationToken tok = CancellationToken::makeOwned();
  EXPECT_TRUE(tok.request_stop());
  EXPECT_FALSE(tok.request_stop());
  EXPECT_TRUE(tok.stop_requested());
}

// Copies of an owned token share the underlying state: stopping the copy also stops the
// original. Default-constructed tokens are sentinel and are not subject to this contract.
TEST(ParallelCancellation, CopiesShareState) {
  const CancellationToken a = CancellationToken::makeOwned();
  CancellationToken b = a;
  EXPECT_FALSE(a.stop_requested());
  EXPECT_FALSE(b.stop_requested());
  EXPECT_TRUE(b.request_stop());
  EXPECT_TRUE(a.stop_requested());
  EXPECT_TRUE(b.stop_requested());
}

// Stop signal becomes visible to a worker thread (release / acquire synchronization).
TEST(ParallelCancellation, CrossThreadStopVisibility) {
  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<int> observed{-1};
  std::thread worker([&]() {
    while (!tok.stop_requested()) {
      // spin until producer stops the token
    }
    observed.store(1, std::memory_order_release);
  });
  tok.request_stop();
  worker.join();
  EXPECT_EQ(observed.load(std::memory_order_acquire), 1);
}

// Deadline default-constructs to never-expires; explicit thresholds expire on TSC advance.
TEST(ParallelCancellation, DeadlineNeverExpiresByDefault) {
  const Deadline d;
  EXPECT_FALSE(d.expired());
  EXPECT_EQ(d.threshold(), std::numeric_limits<std::uint64_t>::max());
}

#if defined(__x86_64__) || defined(_M_X64)
TEST(ParallelCancellation, DeadlineExpiresAfterTscAdvance) {
  // Threshold a few microseconds in the past relative to "now": already expired.
  const std::uint64_t now = __rdtsc();
  // Defensive: if the TSC is somehow at zero, skip rather than underflow.
  if (now > 1024U) {
    const Deadline expired{now - 1024U};
    EXPECT_TRUE(expired.expired());
  }
}

TEST(ParallelCancellation, DeadlineNotExpiredInTheFuture) {
  // Threshold ~1 second in the future at any plausible TSC frequency: not yet expired.
  const std::uint64_t now = __rdtsc();
  const Deadline future{now + (std::uint64_t{1} << 40)};
  EXPECT_FALSE(future.expired());
}
#endif

// cancelled_exception::what() returns a non-null C-string.
TEST(ParallelCancellation, CancelledExceptionWhatNonNull) {
  const cancelled_exception ex;
  ASSERT_NE(ex.what(), nullptr);
  EXPECT_GT(std::strlen(ex.what()), 0U);
}

// cancelled_value_exception<T> carries a partial_value that round-trips through the field.
TEST(ParallelCancellation, CancelledValueExceptionCarriesPartialValue) {
  const cancelled_value_exception<int> ex{42};
  EXPECT_EQ(ex.partial_value, 42);
  ASSERT_NE(ex.what(), nullptr);
  EXPECT_GT(std::strlen(ex.what()), 0U);
}

TEST(ParallelCancellation, CancelledValueExceptionWorksWithDouble) {
  const cancelled_value_exception<double> ex{3.14};
  EXPECT_DOUBLE_EQ(ex.partial_value, 3.14);
}
