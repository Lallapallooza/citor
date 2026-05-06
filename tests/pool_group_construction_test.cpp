#include <gtest/gtest.h>

#include "citor/pool_group.h"

using citor::PoolGroup;

TEST(PoolGroupConstruction, GlobalReturnsTheSameSingletonInstanceAcrossCalls) {
  const PoolGroup *const first = &PoolGroup::global();
  const PoolGroup *const second = &PoolGroup::global();
  EXPECT_EQ(first, second);
}
