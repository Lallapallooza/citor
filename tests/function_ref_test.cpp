#include <gtest/gtest.h>

#include <cstddef>

#include "citor/function_ref.h"

using citor::FunctionRef;

// Runtime confirmation in addition to the compile-time static_assert.
TEST(ParallelFunctionRef, SizeIsExactlySixteenBytes) {
  EXPECT_EQ(sizeof(FunctionRef<void(std::size_t, std::size_t)>), 16U);
  EXPECT_EQ(sizeof(FunctionRef<int(int)>), 16U);
}

// FunctionRef must round-trip a stateful lambda: capture by reference and
// observe a side effect.
TEST(ParallelFunctionRef, RoundTripsStatefulLambdaCallableWithoutCopying) {
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
TEST(ParallelFunctionRef, ForwardsReturnValueFromBoundCallableToCaller) {
  auto square = [](int x) { return x * x; };
  const FunctionRef<int(int)> ref = square;
  EXPECT_EQ(ref(7), 49);
}

// Default-constructed FunctionRef is empty and contextually converts to false.
TEST(ParallelFunctionRef, DefaultConstructedFunctionRefHasNullPayloadAndThunk) {
  const FunctionRef<void()> empty;
  EXPECT_FALSE(static_cast<bool>(empty));
}
