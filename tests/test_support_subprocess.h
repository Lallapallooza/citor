#pragma once

// Subprocess timeout helper for regression tests that pin deadlock fixes.
// A test body that would hang reads as a `ChildOutcome::TimedOut` outcome
// from the parent rather than stalling the whole gtest binary forever.
//
// Linux-only: relies on `fork`, `waitpid`, and `kill`. Each TU that includes
// this header gets its own copy of the template helper through the inline
// `namespace citor_test_support`; ODR is per-TU since gtest binaries are
// 1:1 with `.cpp` files in this tree.

#include <chrono>
#include <cstdint>

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <thread>
#endif

namespace citor_test_support {

#if defined(__linux__)

enum class ChildOutcome : std::uint8_t {
  Exited0,
  ExitedNonZero,
  Signaled,
  TimedOut
};

template <class Fn>
inline ChildOutcome runInChildWithTimeout(Fn &&fn,
                                          std::chrono::milliseconds timeout) {
  using namespace std::chrono_literals;
  const pid_t pid = fork();
  if (pid == 0) {
    try {
      fn();
      _exit(0);
    } catch (...) {
      _exit(111);
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return ChildOutcome::Exited0;
      }
      if (WIFEXITED(status)) {
        return ChildOutcome::ExitedNonZero;
      }
      return ChildOutcome::Signaled;
    }
    std::this_thread::sleep_for(1ms);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return ChildOutcome::TimedOut;
}

#endif

} // namespace citor_test_support
