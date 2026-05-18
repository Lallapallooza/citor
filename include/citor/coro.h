#pragma once

// Coroutine wrappers for citor's synchronous primitives. Opt-in header
// for users whose call sites are already in coroutine code: each wrapper
// returns an awaitable that routes the call through `submitDetached` and
// resumes the awaiting coroutine when the wrapped primitive returns.
//
// Tradeoffs vs. the direct synchronous primitives:
//   - Coroutine frames are heap-allocated by the compiler.
//   - One worker plays the wrapped primitive's "producer" role until it
//     returns; the rest of the pool fans out as usual.

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include "cancellation.h"
#include "hints.h"
#include "thread_pool.h"

namespace citor::coro {

namespace detail {

/// Holds the result of a primitive wrapped as a coroutine.
template <class T>
struct ResultStorage {
  /// Populated when the wrapped primitive returns.
  std::optional<T> value;
  /// Move-construct the stored value in place.
  template <class U>
  void set(U &&v) {
    value.emplace(std::forward<U>(v));
  }
  /// Move the stored value out for return to the awaiting coroutine.
  T take() { return std::move(*value); }
};

/// Void specialisation; carries no payload.
template <>
struct ResultStorage<void> {
  /// No-op for symmetry with the value specialisation.
  void set() {}
  /// No-op for symmetry with the value specialisation.
  void take() {}
};

/// Forward declaration for `Task<T>::promise_type`.
template <class T>
class Promise;

} // namespace detail

/// Coroutine handle wrapper used as the return type of every wrapper. Eager
/// suspension at `initial_suspend` lets the awaiter set its continuation
/// before the first body executes.
template <class T = void>
class Task {
public:
  /// Required by the coroutine machinery.
  using promise_type = detail::Promise<T>;
  /// Convenience alias for the typed coroutine handle.
  using Handle = std::coroutine_handle<promise_type>;

  /// Default-constructed task carries no handle and is not awaitable.
  Task() = default;
  /// Wrap an existing handle; used by `Promise::get_return_object`.
  explicit Task(Handle h) noexcept : m_handle(h) {}

  /// Non-copyable.
  Task(const Task &) = delete;
  /// Non-copyable.
  Task &operator=(const Task &) = delete;
  /// Steals the handle from |o|.
  Task(Task &&o) noexcept : m_handle(std::exchange(o.m_handle, {})) {}
  /// Destroys this handle (if any) and steals |o|'s.
  Task &operator=(Task &&o) noexcept {
    if (this != &o) {
      if (m_handle) {
        m_handle.destroy();
      }
      m_handle = std::exchange(o.m_handle, {});
    }
    return *this;
  }
  /// Destroys the owned coroutine handle if still present.
  ~Task() {
    if (m_handle) {
      m_handle.destroy();
    }
  }

  /// Read the underlying coroutine handle without transferring ownership.
  [[nodiscard]] Handle handle() const noexcept { return m_handle; }
  /// Release ownership of the handle to the caller.
  [[nodiscard]] Handle release() noexcept {
    return std::exchange(m_handle, {});
  }

  /// Per-task awaiter returned by `operator co_await`.
  struct Awaiter {
    /// Inner task's handle.
    Handle inner;
    /// True when the inner task has no handle or already ran to completion.
    [[nodiscard]] bool await_ready() const noexcept {
      return !inner || inner.done();
    }
    /// Records the outer coroutine as the continuation and resumes the
    /// inner task via symmetric transfer.
    Handle await_suspend(std::coroutine_handle<> outer) noexcept {
      inner.promise().continuation = outer;
      return inner;
    }
    /// Rethrows the inner exception if any, otherwise returns its value.
    T await_resume() {
      if (inner.promise().exc) {
        std::rethrow_exception(inner.promise().exc);
      }
      return inner.promise().result.take();
    }
  };
  /// Make the task awaitable by another coroutine.
  Awaiter operator co_await() && noexcept { return Awaiter{m_handle}; }

private:
  /// Owned coroutine handle; null after move or default construction.
  Handle m_handle = nullptr;
};

namespace detail {

/// Awaiter returned by every `Promise::final_suspend`; transfers control
/// back to the stored continuation or to a no-op coroutine.
struct FinalAwaiter {
  /// Always suspends so the continuation runs in a clean stack frame.
  [[nodiscard]] static bool await_ready() noexcept { return false; }
  /// Symmetric-transfers into the stored continuation, or no-op if absent.
  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
    if (h.promise().continuation) {
      return h.promise().continuation;
    }
    return std::noop_coroutine();
  }
  /// Unreachable; the coroutine is suspended for good at final_suspend.
  static void await_resume() noexcept {}
};

/// Promise type for `Task<T>` with non-void payload.
template <class T>
class Promise {
public:
  /// Captured return value or exception.
  ResultStorage<T> result;
  /// Latched exception from the body if any.
  std::exception_ptr exc;
  /// Continuation handle resumed at final_suspend; set by the inner Awaiter.
  std::coroutine_handle<> continuation;

  /// Coroutine machinery hook.
  Task<T> get_return_object() {
    return Task<T>{std::coroutine_handle<Promise>::from_promise(*this)};
  }
  /// Lazy initial suspend: the coroutine does not run until first awaited.
  static std::suspend_always initial_suspend() noexcept { return {}; }
  /// Transfer to the continuation when the coroutine returns.
  static FinalAwaiter final_suspend() noexcept { return {}; }
  /// Capture the returned value.
  template <class U>
  void return_value(U &&v) {
    result.set(std::forward<U>(v));
  }
  /// Capture an exception thrown from the body.
  void unhandled_exception() { exc = std::current_exception(); }
};

/// Void specialisation of `Promise`.
template <>
class Promise<void> {
public:
  /// Unused payload kept for symmetry with the value specialisation.
  ResultStorage<void> result;
  /// Latched exception from the body if any.
  std::exception_ptr exc;
  /// Continuation handle resumed at final_suspend; set by the inner Awaiter.
  std::coroutine_handle<> continuation;

  /// Coroutine machinery hook.
  Task<void> get_return_object() {
    return Task<void>{std::coroutine_handle<Promise>::from_promise(*this)};
  }
  /// Lazy initial suspend: the coroutine does not run until first awaited.
  static std::suspend_always initial_suspend() noexcept { return {}; }
  /// Transfer to the continuation when the coroutine returns.
  static FinalAwaiter final_suspend() noexcept { return {}; }
  /// No payload to capture.
  void return_void() noexcept {}
  /// Capture an exception thrown from the body.
  void unhandled_exception() { exc = std::current_exception(); }
};

/// Wraps a synchronous callable so the coroutine awaiting it resumes once
/// the callable returns. Lives in the awaiting coroutine's frame.
template <class Body>
struct PoolAwaiter {
  /// Pool the body runs on (via `submitDetached`).
  ThreadPool *pool;
  /// Captured callable executed by the worker.
  Body body;
  /// Deduced return type of the callable.
  using Result = std::invoke_result_t<Body &>;
  /// Captured return value from the body.
  ResultStorage<Result> result;
  /// Latched exception thrown from the body if any.
  std::exception_ptr exc;

  /// Always suspends; the body runs on a worker.
  [[nodiscard]] static bool await_ready() noexcept { return false; }

  /// Schedules the body on a worker and resumes the coroutine when done.
  void await_suspend(std::coroutine_handle<> h) {
    pool->template submitDetached<HintsDefaults>([this, h]() mutable {
      try {
        if constexpr (std::is_void_v<Result>) {
          body();
        } else {
          result.set(body());
        }
      } catch (...) {
        exc = std::current_exception();
      }
      h.resume();
    });
  }

  /// Rethrows a body exception or returns the captured value.
  Result await_resume() {
    if (exc) {
      std::rethrow_exception(exc);
    }
    if constexpr (!std::is_void_v<Result>) {
      return result.take();
    }
  }
};

} // namespace detail

/// Run an arbitrary callable on the pool and await its return value.
template <class F>
[[nodiscard]] auto async(ThreadPool &pool, F body) {
  return detail::PoolAwaiter<F>{&pool, std::move(body), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::parallelFor`.
template <class HintsT = HintsDefaults, class Body>
[[nodiscard]] auto parallelFor(ThreadPool &pool, std::size_t lo, std::size_t hi,
                               Body body,
                               CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, lo, hi, body = std::move(body),
             tok = std::move(tok)]() mutable {
    pool.parallelFor<HintsT>(lo, hi, std::move(body), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::parallelReduce`.
template <class HintsT = HintsDefaults, class T, class Map, class Combine>
[[nodiscard]] auto parallelReduce(ThreadPool &pool, std::size_t lo,
                                  std::size_t hi, T identity, Map map,
                                  Combine combine,
                                  CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, lo, hi, identity = std::move(identity),
             map = std::move(map), combine = std::move(combine),
             tok = std::move(tok)]() mutable -> T {
    return pool.parallelReduce<HintsT>(lo, hi, identity, std::move(map),
                                       std::move(combine), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::parallelScan`.
template <class HintsT = HintsDefaults, class T, class BodyFn, class PrefixFn>
[[nodiscard]] auto parallelScan(ThreadPool &pool, std::size_t n, T identity,
                                BodyFn body, PrefixFn prefix,
                                CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, n, identity = std::move(identity), body = std::move(body),
             prefix = std::move(prefix), tok = std::move(tok)]() mutable -> T {
    return pool.parallelScan<HintsT>(n, identity, std::move(body),
                                     std::move(prefix), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::parallelChain`.
template <class ChainHintsT = ChainHintsDefaults, class... Stages>
[[nodiscard]] auto parallelChain(ThreadPool &pool, std::size_t n,
                                 Stages... stages) {
  auto fn = [&pool, n,
             stagesTuple = std::make_tuple(std::move(stages)...)]() mutable {
    std::apply(
        [&pool, n](auto &&...st) {
          pool.parallelChain<ChainHintsT>(n, std::forward<decltype(st)>(st)...);
        },
        stagesTuple);
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::runPlex`.
template <class HintsT = HintsDefaults, class Phase>
[[nodiscard]] auto runPlex(ThreadPool &pool, std::size_t nPhases, std::size_t n,
                           Phase phase,
                           CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, nPhases, n, phase = std::move(phase),
             tok = std::move(tok)]() mutable {
    pool.runPlex<HintsT>(nPhases, n, std::move(phase), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::bulkForQueries`.
template <class HintsT = HintsDefaults, class QueryFn>
[[nodiscard]] auto bulkForQueries(ThreadPool &pool, std::size_t q,
                                  QueryFn query,
                                  CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, q, query = std::move(query),
             tok = std::move(tok)]() mutable {
    pool.bulkForQueries<HintsT>(q, std::move(query), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::forkJoin`.
template <class HintsT = HintsDefaults, class... TaskFns>
[[nodiscard]] auto forkJoin(ThreadPool &pool, TaskFns... fns) {
  auto fn = [&pool, fnsTuple = std::make_tuple(std::move(fns)...)]() mutable {
    std::apply(
        [&pool](auto &&...f) {
          pool.forkJoin<HintsT>(std::forward<decltype(f)>(f)...);
        },
        fnsTuple);
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine variant that waits for the body to complete. Use the pool's
/// direct `submitDetached` for fire-and-forget without the round-trip.
template <class HintsT = HintsDefaults, class F>
[[nodiscard]] auto submitDetached(ThreadPool &pool, F body,
                                  CancellationToken tok = CancellationToken{}) {
  auto fn = [body = std::move(body), tok = std::move(tok)]() mutable {
    if (tok.stop_requested()) {
      return;
    }
    body();
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Block the calling thread until |task| completes, then return its value or
/// rethrow its exception. The expected top-level driver for coroutine code
/// that needs to interface with synchronous callers.
template <class T>
T syncWait(Task<T> task) {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  std::exception_ptr exc;
  detail::ResultStorage<T> result;

  auto wrapper = [&]() -> Task<void> {
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
      } else {
        result.set(co_await std::move(task));
      }
    } catch (...) {
      exc = std::current_exception();
    }
    {
      const std::lock_guard<std::mutex> lk(mu);
      done = true;
    }
    cv.notify_one();
  };

  Task<void> outer = wrapper();
  outer.handle().resume();
  {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return done; });
  }
  // `done` is set in the wrapper body before `final_suspend` runs. The
  // worker thread is still inside the final-suspend chain at that point;
  // destroying the coroutine handle now would race the worker. Spin until
  // the handle reaches its final-suspend point so destruction is safe.
  while (!outer.handle().done()) {
    std::this_thread::yield();
  }
  if (exc) {
    std::rethrow_exception(exc);
  }
  if constexpr (!std::is_void_v<T>) {
    return result.take();
  }
}

} // namespace citor::coro
