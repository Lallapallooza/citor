#pragma once

#include <cstddef>

namespace citor::detail {

// Predicate gating the inline fallback for small fan-outs.
//
// The pool's dispatch overhead is amortized over |n| items at
// |estimatedItemNs| each; when the fan-out's per-participant wall time falls
// below |minTaskUs|, the dispatch round-trip dominates and the producer should
// run inline. The exact gate is `n * estimatedItemNs * 1e-3 < minTaskUs *
// participants`, in `double` arithmetic to avoid integer-overflow surprises.
//
// When |estimatedItemNs| is zero, the gate defaults to "do not inline": the
// inline fallback is opt-in via a non-zero estimate.
[[nodiscard]] inline bool shouldRunInline(std::size_t n,
                                          std::size_t participants,
                                          double estimatedItemNs,
                                          double minTaskUs) noexcept {
  if (participants <= 1) {
    return true;
  }
  if (n == 0) {
    return true;
  }
  if (estimatedItemNs <= 0.0) {
    return false;
  }
  const double estimatedTotalUs =
      static_cast<double>(n) * estimatedItemNs * 1e-3;
  const double threshold = minTaskUs * static_cast<double>(participants);
  return estimatedTotalUs < threshold;
}

// Compile-time-hinted variant. With a non-zero `HintsT::estimatedItemNs` the
// caller pays the runtime gate; otherwise the gate folds to `participants <=
// 1` at compile time. Centralises the `if constexpr` cascade open-coded at
// every typed primitive's entry.
template <class HintsT>
[[nodiscard]] inline bool shouldRunInlineHinted(std::size_t n,
                                                std::size_t participants) {
  if constexpr (HintsT::estimatedItemNs > 0.0) {
    return shouldRunInline(n, participants, HintsT::estimatedItemNs,
                           HintsT::minTaskUs);
  } else {
    (void)n;
    return participants <= 1;
  }
}

} // namespace citor::detail
