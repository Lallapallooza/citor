#pragma once

// Coroutine wrappers for citor's synchronous primitives. Opt-in
// header: each wrapper returns an awaitable that runs the body on a
// per-pool driver thread and resumes the coroutine when the body
// returns.

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "citor/cancellation.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

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
  /// Kept non-static: the coroutine protocol invokes awaiter and promise
  /// hooks through an instance, so `static` here trips
  /// `readability-static-accessed-through-instance` at every await site.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[nodiscard]] bool await_ready() const noexcept { return false; }
  /// Symmetric-transfers into the stored continuation, or no-op if absent.
  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
    if (h.promise().continuation) {
      return h.promise().continuation;
    }
    return std::noop_coroutine();
  }
  /// Unreachable; the coroutine is suspended for good at final_suspend.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void await_resume() const noexcept {}
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
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  std::suspend_always initial_suspend() noexcept { return {}; }
  /// Transfer to the continuation when the coroutine returns.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  FinalAwaiter final_suspend() noexcept { return {}; }
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
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  std::suspend_always initial_suspend() noexcept { return {}; }
  /// Transfer to the continuation when the coroutine returns.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  FinalAwaiter final_suspend() noexcept { return {}; }
  /// No payload to capture.
  void return_void() noexcept {}
  /// Capture an exception thrown from the body.
  void unhandled_exception() { exc = std::current_exception(); }
};

/// Wrapper-destruction handshake for `syncWait`. `state` packs a done
/// bit (set by the wrapper body) and an in-flight worker counter
/// (bumped in `PoolAwaiter::await_suspend`, dropped after `h.resume`
/// returns). `syncWait` destroys the wrapper only when both signal.
struct SyncWaitGate {
  /// High bit of `state`: set when the wrapper coroutine body completes.
  static constexpr std::uint64_t kDoneBit = std::uint64_t{1} << 63;
  /// Low 63 bits of `state`: in-flight worker count.
  static constexpr std::uint64_t kCounterMask = kDoneBit - 1;
  /// Combined state word; the producer waits for `kDoneBit` set and the
  /// counter at zero before destroying the wrapper.
  std::atomic<std::uint64_t> state{0};
};

/// Thread-local pointer to the gate of the syncWait the current thread
/// is participating in. `syncWait` sets it on the producer thread for
/// the initial resume, and every `PoolAwaiter` worker lambda re-sets it
/// for the duration of `h.resume` so that nested `co_await`s constructed
/// on the worker side inherit the same gate. Null when no syncWait is
/// active on this thread.
inline thread_local std::shared_ptr<SyncWaitGate> *tlSyncWaitGate = nullptr;

/// One persistent driver thread per pool. Runs the body and resumes
/// the coroutine for each `co_await`, so the wrapper does not pay a
/// `pthread_create` per call. Constructed lazily, joined at process
/// exit. Holds no pool reference; destroying the pool while the
/// driver still exists is safe as long as no coroutine work is in
/// flight.
class CoroDriver {
public:
  /// Type-erased closure the driver invokes for each enqueued await.
  using Task = std::function<void()>;

  /// Spawns the driver thread.
  CoroDriver() : m_thread([this]() { run(); }) {}

  CoroDriver(const CoroDriver &) = delete;
  CoroDriver &operator=(const CoroDriver &) = delete;

  /// Signals shutdown and joins the driver thread.
  ~CoroDriver() {
    {
      const std::lock_guard<std::mutex> lk(m_mu);
      m_shutdown = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  /// Enqueue |t| for the driver thread to run. Returns immediately.
  void submit(Task t) {
    {
      const std::lock_guard<std::mutex> lk(m_mu);
      m_queue.push_back(std::move(t));
    }
    m_cv.notify_one();
  }

private:
  /// Driver loop: wait for work or shutdown, dequeue, run, repeat.
  void run() {
    while (true) {
      std::unique_lock<std::mutex> lk(m_mu);
      m_cv.wait(lk, [this]() { return m_shutdown || !m_queue.empty(); });
      if (m_queue.empty()) {
        return;
      }
      const Task t = std::move(m_queue.front());
      m_queue.pop_front();
      lk.unlock();
      t();
    }
  }

  /// Guards `m_queue` and `m_shutdown`.
  std::mutex m_mu;
  /// Signalled on submit and at shutdown.
  std::condition_variable m_cv;
  /// FIFO of pending body-and-resume closures.
  std::deque<Task> m_queue;
  /// Set by the destructor to wind the driver loop down.
  bool m_shutdown = false;
  /// The driver loop runs here.
  std::thread m_thread;
};

/// Lazy per-pool accessor; the first `co_await` against |pool|
/// spawns its driver, subsequent awaits reuse it.
inline CoroDriver &driverFor(ThreadPool &pool) {
  static std::mutex mu;
  static std::unordered_map<ThreadPool *, std::unique_ptr<CoroDriver>> drivers;
  const std::lock_guard<std::mutex> lk(mu);
  auto it = drivers.find(&pool);
  if (it == drivers.end()) {
    it = drivers.emplace(&pool, std::make_unique<CoroDriver>()).first;
  }
  return *it->second;
}

/// Wraps a synchronous callable so the coroutine awaiting it resumes once
/// the callable returns. Lives in the awaiting coroutine's frame.
template <class Body>
struct PoolAwaiter {
  /// Pool whose driver thread runs the body.
  ThreadPool *pool;
  /// Captured callable executed by the worker.
  Body body;
  /// Deduced return type of the callable.
  using Result = std::invoke_result_t<Body &>;
  /// Captured return value from the body.
  ResultStorage<Result> result;
  /// Latched exception thrown from the body if any.
  std::exception_ptr exc;
  /// Gate inherited from the surrounding `syncWait`, captured at awaiter
  /// construction time. Null outside any `syncWait` scope; the awaiter
  /// then runs without producer-side counter coordination.
  std::shared_ptr<SyncWaitGate> gate{tlSyncWaitGate != nullptr
                                         ? *tlSyncWaitGate
                                         : std::shared_ptr<SyncWaitGate>{}};

  /// Always suspends; the body runs on a worker.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[nodiscard]] bool await_ready() const noexcept { return false; }

  /// Enqueues the body on the pool's driver thread and resumes the
  /// coroutine when the body returns.
  void await_suspend(std::coroutine_handle<> h) {
    auto g = gate;
    if (g) {
      g->state.fetch_add(1, std::memory_order_relaxed);
    }
    driverFor(*pool).submit([this, h, g]() mutable {
      std::shared_ptr<SyncWaitGate> *savedTL = nullptr;
      if (g) {
        savedTL = std::exchange(tlSyncWaitGate, &g);
      }
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
      // h.resume has returned. The wrapper coroutine and every coroutine
      // it transferred to have fully unwound on this thread; the frame
      // members (body, result, exc) are no longer touched below.
      if (g) {
        tlSyncWaitGate = savedTL;
        const auto prev = g->state.fetch_sub(1, std::memory_order_release);
        if ((prev & SyncWaitGate::kCounterMask) == 1 &&
            (prev & SyncWaitGate::kDoneBit) != 0) {
          g->state.notify_all();
        }
      }
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

/// Block the calling thread until |task| completes; return its value
/// or rethrow its exception. Destruction waits on the `SyncWaitGate`
/// so no worker is still inside `h.resume` when the wrapper frame
/// goes away.
template <class T>
T syncWait(Task<T> task) {
  std::exception_ptr exc;
  // The non-void instantiation mutates `result` through `result.set()`;
  // `misc-const-correctness` sees only the void instantiation's dead branch.
  // NOLINTNEXTLINE(misc-const-correctness)
  detail::ResultStorage<T> result;
  auto gate = std::make_shared<detail::SyncWaitGate>();

  auto wrapper = [&, gate]() -> Task<void> {
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
      } else {
        result.set(co_await std::move(task));
      }
    } catch (...) {
      exc = std::current_exception();
    }
    gate->state.fetch_or(detail::SyncWaitGate::kDoneBit,
                         std::memory_order_release);
    // Skip the notify here: the last worker to fetch_sub its counter slot
    // will see (prev & kCounterMask) == 1 and the done bit set, and will
    // notify then. Notifying here would only produce spurious wakeups
    // (counter still non-zero).
  };

  auto *savedTL = std::exchange(detail::tlSyncWaitGate, &gate);
  const Task<void> outer = wrapper();
  outer.handle().resume();
  detail::tlSyncWaitGate = savedTL;

  std::uint64_t s = gate->state.load(std::memory_order_acquire);
  while ((s & detail::SyncWaitGate::kDoneBit) == 0 ||
         (s & detail::SyncWaitGate::kCounterMask) != 0) {
    gate->state.wait(s, std::memory_order_acquire);
    s = gate->state.load(std::memory_order_acquire);
  }

  if (exc) {
    std::rethrow_exception(exc);
  }
  if constexpr (!std::is_void_v<T>) {
    return result.take();
  }
}

} // namespace citor::coro
