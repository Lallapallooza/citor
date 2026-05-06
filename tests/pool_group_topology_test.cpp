#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iostream>

#include "citor/detail/topology.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"

using citor::PoolGroup;
using citor::PoolKind;

TEST(PoolGroupTopology, CcdCountReportedByPoolGroupMatchesSysfsCcdEnumeration) {
  PoolGroup &group = PoolGroup::global();
  ASSERT_GE(group.ccdCount(), std::size_t{1});

  // Probe the topology directly so we can log the host's CCD enumeration
  // alongside the arena's view; the two must agree.
  const auto enumerated = citor::detail::enumerateCcds();
  ASSERT_EQ(enumerated.size(), group.ccdCount());

  std::cerr << "[pool_group_test] topology probe reports " << enumerated.size()
            << " CCD(s); ";
  for (std::size_t ccd = 0; ccd < enumerated.size(); ++ccd) {
    std::cerr << "CCD " << ccd << " has " << enumerated[ccd].size()
              << " CPU(s)";
    if (ccd + 1 < enumerated.size()) {
      std::cerr << ", ";
    }
  }
  std::cerr << '\n';

  for (std::size_t ccd = 0; ccd < group.ccdCount(); ++ccd) {
    EXPECT_GE(group.arena(ccd).participants(), std::size_t{1});
    EXPECT_EQ(group.arena(ccd).kind(), PoolKind::Arena);
    EXPECT_EQ(group.arena(ccd).arenaIndex(), static_cast<std::uint32_t>(ccd));
  }
}
