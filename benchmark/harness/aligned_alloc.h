#pragma once

#include <cstddef>
#include <cstdlib>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace citor::bench {

/// Portable aligned allocation. POSIX wraps `posix_memalign`; MSVC
/// routes to `_aligned_malloc`, whose buffers must be released through
/// `_aligned_free`. Returns `nullptr` on failure.
[[nodiscard]] inline void *alignedAlloc(std::size_t bytes,
                                        std::size_t align) noexcept {
#if defined(_MSC_VER)
  return ::_aligned_malloc(bytes, align);
#else
  void *p = nullptr;
  if (::posix_memalign(&p, align, bytes) != 0) {
    return nullptr;
  }
  return p;
#endif
}

/// Companion to `alignedAlloc`. On MSVC `_aligned_malloc`'s buffers are
/// invalid input to `std::free`, so we dispatch on the platform here.
inline void alignedFree(void *p) noexcept {
  if (p == nullptr) {
    return;
  }
#if defined(_MSC_VER)
  ::_aligned_free(p);
#else
  std::free(p);
#endif
}

} // namespace citor::bench
