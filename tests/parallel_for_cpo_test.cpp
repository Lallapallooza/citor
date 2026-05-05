#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "citor/cpos/parallel_for.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::Hints;
using citor::HintsDefaults;
using citor::ThreadPool;

// Member-and-CPO equivalence: both surfaces produce equivalent observable
// behavior.
TEST(ParallelForCpoEquivalence,
     MemberCallAndCpoCallProduceIdenticalSideEffects) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 256;
  std::vector<int> dataMember(kN, 0);
  std::vector<int> dataCpo(kN, 0);

  pool.parallelFor<HintsDefaults>(
      0, kN, [&dataMember](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          dataMember[i] = static_cast<int>(i);
        }
      });

  citor::parallelFor.template operator()<HintsDefaults>(
      pool, 0, kN, [&dataCpo](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          dataCpo[i] = static_cast<int>(i);
        }
      });

  EXPECT_EQ(dataMember, dataCpo);
}

// parallelForRuntime (runtime-hint sibling) routes through the same engine and
// observably matches the member template's behavior.
TEST(ParallelForCpoEquivalence,
     RuntimeHintsSiblingProducesSameOutputAsCompileTimeHints) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 256;
  std::vector<int> dataCompile(kN, 0);
  std::vector<int> dataRuntime(kN, 0);

  pool.parallelFor<HintsDefaults>(
      0, kN, [&dataCompile](std::size_t lo, std::size_t hi) {
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
