#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

#include "citor/cancellation.h"

using citor::cancelled_exception;
using citor::cancelled_value_exception;

// cancelled_exception::what() returns a non-null C-string.
TEST(ParallelCancellationException,
     CancelledExceptionWhatReturnsNonNullMessage) {
  const cancelled_exception ex;
  ASSERT_NE(ex.what(), nullptr);
  EXPECT_GT(std::strlen(ex.what()), 0U);
}

// cancelled_value_exception<T> carries a partial_value that round-trips through
// the field.
TEST(ParallelCancellationException,
     CancelledValueExceptionCarriesPartialIntValue) {
  const cancelled_value_exception<int> ex{42};
  EXPECT_EQ(ex.partial_value, 42);
  ASSERT_NE(ex.what(), nullptr);
  EXPECT_GT(std::strlen(ex.what()), 0U);
}

TEST(ParallelCancellationException,
     CancelledValueExceptionCarriesPartialDoubleValue) {
  const cancelled_value_exception<double> ex{3.14};
  EXPECT_DOUBLE_EQ(ex.partial_value, 3.14);
}
