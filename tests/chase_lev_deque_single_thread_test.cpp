#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "citor/detail/chase_lev_deque.h"

using citor::detail::ChaseLevDeque;

// Owner-side push and pop must return values in LIFO order. The pop sequence on
// a single thread matches the reverse of the push sequence; the test pins the
// deque's invariant for the uncontended case.
TEST(ChaseLevDequeSingleThread, OwnerPopReturnsValuesInLifoOrder) {
  ChaseLevDeque<int> dq;
  for (int i = 0; i < 100; ++i) {
    dq.push(i);
  }
  std::vector<int> got;
  while (true) {
    auto v = dq.pop();
    if (!v) {
      break;
    }
    got.push_back(*v);
  }
  ASSERT_EQ(got.size(), 100U);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(got[static_cast<std::size_t>(i)], 99 - i);
  }
}

// pop on an empty deque returns std::nullopt without UB.
TEST(ChaseLevDequeSingleThread, PopOnEmptyDequeReturnsNullopt) {
  ChaseLevDeque<int> dq;
  EXPECT_FALSE(dq.pop().has_value());
  EXPECT_TRUE(dq.empty());
  EXPECT_EQ(dq.size(), 0U);
}
