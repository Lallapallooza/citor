#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>
#include <utility>

#include "citor/cancellation.h"
#include "citor/function_ref.h"
#include "citor/hints.h"

using citor::Affinity;
using citor::Balance;
using citor::BarrierKind;
using citor::CancellationToken;
using citor::ChainHints;
using citor::Deadline;
using citor::Determinism;
using citor::FunctionRef;
using citor::Hints;
using citor::kCacheLine;
using citor::makeStage;
using citor::Priority;
using citor::Stage;
using citor::StealPolicy;

// 16-byte FunctionRef invariant (compile-time).
static_assert(sizeof(FunctionRef<void(std::size_t, std::size_t)>) == 16,
              "FunctionRef<void(size_t, size_t)> must be 16 bytes");
static_assert(sizeof(FunctionRef<int(int)>) == 16,
              "FunctionRef<int(int)> must be 16 bytes");
static_assert(sizeof(FunctionRef<void()>) == 16,
              "FunctionRef<void()> must be 16 bytes");

// ===== Trivial-copy invariant: lets the descriptor sit in registers across
// boundaries =====
static_assert(std::is_trivially_copyable_v<FunctionRef<void()>>,
              "FunctionRef must be trivially copyable");
static_assert(std::is_trivially_destructible_v<FunctionRef<void()>>,
              "FunctionRef must be trivially destructible");

// Bundled presets inherit the documented HintsDefaults fields and override only
// what differs. These static_asserts pin the surviving overrides so a future
// edit cannot silently change them.
static_assert(citor::HintsDefaults::balance == Balance::DynamicChunked,
              "HintsDefaults::balance must be DynamicChunked");
static_assert(citor::KahanReduceHints::determinism ==
                  Determinism::KahanCompensated,
              "KahanReduceHints::determinism must be KahanCompensated");
static_assert(citor::LatencyHints::priority == Priority::Latency,
              "LatencyHints::priority must be Latency");
static_assert(citor::ChainHintsDefaults::pipelineSameChunk,
              "ChainHintsDefaults::pipelineSameChunk must be true");
static_assert(citor::DynamicChainHints::balance == Balance::DynamicChunked,
              "DynamicChainHints::balance must be DynamicChunked");
static_assert(!citor::DynamicChainHints::pipelineSameChunk,
              "DynamicChainHints must opt out of same-chunk pipelining");

// ===== Stage / ChainHints structural sanity =====
static_assert(
    Stage<int (*)(std::size_t, std::size_t), BarrierKind::Global>::barrier ==
        BarrierKind::Global,
    "Stage::barrier must reflect the After non-type template parameter");

// ===== kCacheLine is hardcoded to 128, never the std value =====
static_assert(kCacheLine == 128, "kCacheLine must be hardcoded to 128 bytes");

// Cancellation API noexcept contract.
static_assert(
    noexcept(std::declval<const CancellationToken &>().stop_requested()),
    "CancellationToken::stop_requested must be noexcept");
static_assert(noexcept(std::declval<CancellationToken &>().request_stop()),
              "CancellationToken::request_stop must be noexcept");
static_assert(noexcept(std::declval<const Deadline &>().expired()),
              "Deadline::expired must be noexcept");

// Runtime check on top of the static_assert: the constexpr fields should be
// observable.
TEST(ParallelHints,
     HintsDefaultsExposesEveryDocumentedStaticConstexprConstant) {
  using HintsT = citor::HintsDefaults;
  EXPECT_EQ(HintsT::balance, Balance::DynamicChunked);
  EXPECT_EQ(HintsT::determinism, Determinism::FixedBlockOrder);
  EXPECT_EQ(HintsT::affinity, Affinity::PerCluster);
  EXPECT_EQ(HintsT::stealPolicy, StealPolicy::ClusterLocal);
  EXPECT_EQ(HintsT::priority, Priority::Throughput);
  EXPECT_TRUE(HintsT::cancellationChecks);
}

// Runtime Hints POD must be default-constructible with the documented defaults.
TEST(ParallelHints, RuntimeHintsStructHasEveryDocumentedDefaultValue) {
  const Hints h{};
  EXPECT_EQ(h.balance, Balance::StaticUniform);
  EXPECT_EQ(h.determinism, Determinism::FixedBlockOrder);
  EXPECT_EQ(h.affinity, Affinity::PerCluster);
  EXPECT_EQ(h.stealPolicy, StealPolicy::ClusterLocal);
  EXPECT_EQ(h.priority, Priority::Throughput);
  EXPECT_TRUE(h.cancellationChecks);
}

// Stage helper produces a Stage<F, BarrierKind> deducing F from the lambda.
TEST(ParallelHints, StageHelperDeducesCallableTypeFromBareLambda) {
  auto stage = makeStage<BarrierKind::Global>([](std::size_t, std::size_t) {});
  EXPECT_EQ(decltype(stage)::barrier, BarrierKind::Global);
}

TEST(ParallelHints, ChainHintsPodHasEveryDocumentedDefaultValue) {
  const ChainHints ch{};
  EXPECT_EQ(ch.balance, Balance::StaticUniform);
  EXPECT_TRUE(ch.pipelineSameChunk);
  EXPECT_TRUE(ch.cancellationChecks);
}

TEST(ParallelHints,
     DynamicChainHintsExposesEveryDocumentedStaticConstexprConstant) {
  using HintsT = citor::DynamicChainHints;
  EXPECT_EQ(HintsT::balance, Balance::DynamicChunked);
  EXPECT_FALSE(HintsT::pipelineSameChunk);
  EXPECT_TRUE(HintsT::cancellationChecks);
}
