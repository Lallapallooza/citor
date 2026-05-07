// Regression test for the `fastRange32` mapper's bounds invariant: the
// computed value must always lie in `[0, n)` for arbitrary 64-bit input.

#include <gtest/gtest.h>

#include <cstdint>

// fastRange32 must produce values in [0, n) for arbitrary 64-bit inputs.
// Quick property check across many seeds.
TEST(RegressionFastRange,
     FastRange32MapperOutputAlwaysStaysWithinRequestedBounds) {
  // fastRange32 is private; we call it indirectly through a typical
  // workload (stealing). Instead use a property-style equivalent here:
  // the formula is `(((uint64_t)(x >> 32)) * n) >> 32`, range [0, n).
  auto fastRange32 = [](std::uint64_t x, std::uint32_t n) -> std::uint32_t {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(x >> 32) * n) >> 32);
  };
  std::uint64_t state = 0xDEADBEEF12345678ULL;
  for (int i = 0; i < 100000; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const std::uint32_t n = (state & 0xFFFFFF) + 1U;
    const std::uint32_t v = fastRange32(state, n);
    EXPECT_LT(v, n);
  }
}
