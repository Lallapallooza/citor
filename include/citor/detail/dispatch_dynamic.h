#pragma once

// `Balance::DynamicChunked` shares its iteration / typed-entry / cold-collapse implementation
// with `Balance::StaticUniform` via the unified `typedWorkerEntry` and `runPartition` templates
// in `dispatch_static.h`. The only per-balance difference is the `BlockClaim<B>::next` policy.
//
// This header keeps a few legacy aliases callers historically referenced; new code should use
// the unified entries directly.

#include "citor/detail/dispatch_static.h"

namespace citor::detail {

/// Typed slot-0 runner for `Balance::DynamicChunked`. Sibling of `runStaticPartitionTyped`;
/// both alias `runPartitionTyped<B>` parameterized by balance.
template <class FOp>
[[gnu::always_inline]] inline void runDynamicCounterTyped(JobDescriptor &desc,
                                                          std::uint32_t rank, FOp &fn) noexcept {
  runPartitionTyped<Balance::DynamicChunked>(desc, rank, fn);
}

} // namespace citor::detail
