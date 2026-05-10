#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "citor/cpos/inclusive_scan.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Member-and-CPO equivalence: `pool.inclusiveScan<HintsT>(...)` and
// `citor::inclusiveScan(pool, ...)` route through the same engine and
// must produce identical output and identical inclusive totals.
TEST(InclusiveScanCpoEquivalence,
     MemberCallAndCpoCallProduceIdenticalScanOutput) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 4096;
  std::vector<std::int64_t> in(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    in[i] = static_cast<std::int64_t>(1 + (i & 0x3FU));
  }
  std::vector<std::int64_t> outMember(kN, 0);
  std::vector<std::int64_t> outCpo(kN, 0);

  const std::int64_t totalMember = pool.inclusiveScan<HintsDefaults>(
      std::span<const std::int64_t>(in.data(), kN),
      std::span<std::int64_t>(outMember.data(), kN), std::int64_t{0},
      std::plus<std::int64_t>{});

  const std::int64_t totalCpo =
      citor::inclusiveScan.template operator()<HintsDefaults>(
          pool, std::span<const std::int64_t>(in.data(), kN),
          std::span<std::int64_t>(outCpo.data(), kN), std::int64_t{0},
          std::plus<std::int64_t>{});

  EXPECT_EQ(totalMember, totalCpo);
  EXPECT_EQ(outMember, outCpo);

  // Final inclusive total matches the sequential reference.
  std::int64_t reference = 0;
  for (std::size_t i = 0; i < kN; ++i) {
    reference += in[i];
  }
  EXPECT_EQ(totalCpo, reference);
}
