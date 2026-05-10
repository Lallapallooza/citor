#pragma once

// Compile-time predicates over `HintsT` parameters. Centralises the
// `requires { HintsT::cancellationChecks; }` probe + default that every
// primitive's entry would otherwise open-code.

namespace citor::detail {

/// Cancellation-active probe. `true` when |HintsT| has no
/// `cancellationChecks` member (default to honour the token), or when
/// `HintsT::cancellationChecks` is `true`.
template <class HintsT>
inline constexpr bool kCancellationActive = []() {
  if constexpr (requires { HintsT::cancellationChecks; }) {
    return HintsT::cancellationChecks;
  } else {
    return true;
  }
}();

} // namespace citor::detail
