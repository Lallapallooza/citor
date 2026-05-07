#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "citor/detail/chase_lev_deque.h"

using citor::detail::ChaseLevDeque;

// Resizing under sustained push: pushing more elements than the initial
// capacity triggers grow, and every element survives both grow and a subsequent
// drain via pop.
TEST(ChaseLevDequeGrowth, GrowsCapacityAndPreservesElementsUnderOverflow) {
  ChaseLevDeque<int> dq(/*initialCapacity=*/16);
  EXPECT_GE(dq.capacity(), 16U);
  const std::size_t initialCap = dq.capacity();
  // Push past initial capacity to force at least one grow.
  const int kPushed = static_cast<int>(initialCap * 4);
  for (int i = 0; i < kPushed; ++i) {
    dq.push(i);
  }
  EXPECT_GT(dq.capacity(), initialCap);
  std::vector<int> got;
  while (true) {
    auto v = dq.pop();
    if (!v) {
      break;
    }
    got.push_back(*v);
  }
  EXPECT_EQ(got.size(), static_cast<std::size_t>(kPushed));
  for (int i = 0; i < kPushed; ++i) {
    EXPECT_EQ(got[static_cast<std::size_t>(i)], kPushed - 1 - i);
  }
}
