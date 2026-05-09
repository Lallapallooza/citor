// Default TSan options compiled into every TSan test binary.
//
// `__tsan_default_options` is a weak symbol the TSan runtime reads at
// startup, so the values below take effect even when the binary is
// launched directly (no ctest, no `TSAN_OPTIONS` env var). A user
// `TSAN_OPTIONS` value still wins because the runtime parses the env
// var after the weak symbol.
//
// Keep this list tight: every flag here changes how TSan instruments
// shadow memory, and a stale or aggressive value can starve user-code
// atomics and turn a fast fuzz test into an apparent deadlock. Only
// the diagnostics-quality flags below are project-wide. Per-flag list
// in `tsan_flags.inc` (compiler-rt).

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CITOR_TSAN_DEFAULTS 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define CITOR_TSAN_DEFAULTS 1
#endif

#ifdef CITOR_TSAN_DEFAULTS

extern "C" const char *__tsan_default_options() noexcept {
  // `second_deadlock_stack=1` and `print_full_thread_history=1` are
  // diagnostics-only flags that improve report quality without
  // altering instrumentation cost. `atexit_sleep_ms=0` skips the
  // unused 1 s post-exit pause. `halt_on_error=0` keeps the existing
  // contract of running every fuzz iteration to surface every race in
  // a single test invocation.
  return "halt_on_error=0"
         ":second_deadlock_stack=1"
         ":print_full_thread_history=1"
         ":atexit_sleep_ms=0";
}

#endif
