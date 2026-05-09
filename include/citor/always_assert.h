#pragma once

#include <cstdio>
#include <cstdlib>

namespace citor {

/// Emits a diagnostic to `stderr` and terminates via `std::abort`.
///
/// The format is fixed ("citor: always-assert failed: <cond> at
/// <file>:<line>\n") so death tests can match the output with a stable regex.
/// `std::abort` is chosen over `std::terminate` so GoogleTest's `EXPECT_DEATH`
/// catches the signal without routing through the terminate handler.
[[noreturn]] inline void alwaysAssertFail(const char *cond, const char *file,
                                          int line) noexcept {
  std::fprintf(stderr, "citor: always-assert failed: %s at %s:%d\n", cond, file,
               line);
  std::abort();
}

} // namespace citor

/// Release-active assertion: evaluates |cond| in every build configuration.
///
/// Unlike `assert`, this check survives -DNDEBUG. Use it at public API entry
/// points whose failure would corrupt memory or trigger undefined behavior past
/// the debug boundary -- writes through a read-only borrow, null output
/// buffers, etc.
#define CITOR_ALWAYS_ASSERT(cond)                                              \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ::citor::alwaysAssertFail(#cond, __FILE__, __LINE__);                    \
    }                                                                          \
  } while (0)
