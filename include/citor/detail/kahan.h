#pragma once

namespace citor::detail {

/// Compensated floating-point pair carrying a running sum and its rounding residual.
///
/// `KahanPair` represents a partial reduction state used by `Determinism::KahanCompensated`
/// `parallelReduce` calls. Every per-chunk accumulator is a `KahanPair`; per-chunk pairs are
/// combined deterministically through `kahanCombine` so the whole reduction tree carries
/// compensation through every interior node.
///
/// `sum` is the running cancelled sum and `c` is the running compensation term (negated
/// lost-low-bits). The pair is initialized to zero by default; user code never sees a partially
/// constructed pair on the producer's stack.
struct KahanPair {
  /// Cancelled running sum.
  double sum = 0.0;

  /// Compensation term (negated lost-low-bits).
  double c = 0.0;
};

/// Add a scalar |x| to a running `KahanPair` accumulator using Kahan compensation.
///
/// Implements one step of the textbook compensated summation. The compensation term |a|.c is
/// subtracted from |x| to recover the previously lost low bits before the running sum is bumped;
/// the new compensation captures the rounding error introduced by this step.
///
/// a Current accumulator.
/// x Scalar to add.
/// New accumulator with |x| folded in.
[[nodiscard]] inline KahanPair kahanAdd(KahanPair a, double x) noexcept {
  const double y = x - a.c;
  const double t = a.sum + y;
  KahanPair r;
  r.c = (t - a.sum) - y;
  r.sum = t;
  return r;
}

/// Combine two `KahanPair` accumulators into a single compensated sum.
///
/// Used at every interior node of the chunk-id pairwise reduction tree: each subtree's partial sum
/// is itself a `KahanPair`, and combining two siblings preserves the compensation contract. The
/// implementation folds |b|.sum into |a| via `kahanAdd`, then folds |b|.c (the right child's
/// compensation) so the residual carried into the parent is the sum of both children's residuals.
///
/// a Left subtree accumulator.
/// b Right subtree accumulator.
/// Combined accumulator covering both subtrees.
[[nodiscard]] inline KahanPair kahanCombine(KahanPair a, KahanPair b) noexcept {
  const KahanPair afterSum = kahanAdd(a, b.sum);
  return kahanAdd(afterSum, -b.c);
}

} // namespace citor::detail
