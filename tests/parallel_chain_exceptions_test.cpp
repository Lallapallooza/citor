#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::ChainHintsDefaults;
using citor::globalStage;
using citor::ThreadPool;

// Exception propagation through `parallelChain`. A throw from inside any
// stage must surface to the producer at join.

// Exception in a stage propagates to the caller.
TEST(ParallelChainExceptions, RethrowsStageBodyExceptionAtJoin) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 32;

  EXPECT_THROW(
      pool.parallelChain<ChainHintsDefaults>(
          kN,
          globalStage("ok",
                      [&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                          std::size_t /*lo*/, std::size_t /*hi*/) noexcept {}),
          globalStage("throws",
                      [&](std::size_t /*stageIdx*/, std::uint32_t slot,
                          std::size_t /*lo*/, std::size_t /*hi*/) {
                        if (slot == 0U) {
                          throw std::runtime_error("chain stage fault");
                        }
                      })),
      std::runtime_error);
}
