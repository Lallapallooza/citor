#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace citor::detail {

/// Combine an array of partial values via a chunk-id pairwise reduction tree.
///
/// The combine order is the deterministic pairwise tree on chunk ids (NOT
/// worker ids):
///
/// - Step 1 combines `(0,1), (2,3), (4,5), ...`
/// - Step 2 combines `(0,2), (4,6), ...`
/// - The doubling step continues until a single value remains in `partials[0]`.
///
/// If the number of partials is not a power of two, the surplus right-hand
/// element is carried forward to the next step unchanged (i.e., interior nodes
/// always combine two siblings, leaves with no sibling get reused as their own
/// subtree partial). This preserves the Demmel-Nguyen (TOMS 2014)
/// bit-reproducibility property: the tree shape is `n`-determined and each
/// interior node has fixed left/right operands, so the output is bit-identical
/// regardless of which worker computed which leaf.
///
/// T       Partial value type (e.g. `double`, `KahanPair`).
/// Combine Binary combiner; called as `combine(left, right)` and must return a
/// `T`. partials In-place workspace; mutated as the tree collapses upward.
/// combine  Combiner function.
/// The fully combined partial covering every chunk; matches `partials.front()`
/// after the
///         call. Returns a default-constructed `T` when |partials| is empty.
template <class T, class Combine>
[[nodiscard]] T pairwiseTreeCombine(std::vector<T> &partials, Combine combine) {
  if (partials.empty()) {
    return T{};
  }
  std::size_t step = 1;
  while (step < partials.size()) {
    const std::size_t stride = step * 2;
    for (std::size_t i = 0; i + step < partials.size(); i += stride) {
      partials[i] = combine(partials[i], partials[i + step]);
    }
    step = stride;
  }
  return std::move(partials[0]);
}

} // namespace citor::detail
