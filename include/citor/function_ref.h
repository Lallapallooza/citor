#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace citor {

/// Primary template; specialized below for function-type signatures.
///
/// Instantiating `FunctionRef` with a non-function type is a programmer error and produces a
/// compile-time diagnostic. The empty primary template is the surface that catches
/// misuse before the specialization mismatch can leak into a downstream context.
template <class Sig> class FunctionRef;

/// 16-byte non-owning callable reference; analogous to `std::string_view` for callables.
///
/// `FunctionRef<R(Args...)>` stores exactly two pointers: an erased object pointer and a thunk
/// function pointer that knows how to invoke the original callable through the erased pointer.
/// The class is a parameter-only type: it does not extend the lifetime of the callable it refers
/// to, so the caller must keep the underlying object alive for the duration of every invocation.
///
/// This shape (16 bytes, two pointers, no heap, no allocation) matches the convention used by
/// `llvm::function_ref`, `absl::FunctionRef`, and `folly::FunctionRef`. It is the only shape that
/// fits the pool's hot-dispatch contract: closure lambdas live on the producer's stack and the
/// descriptor stores a `FunctionRef` pointing into them; primitives are synchronous so the
/// descriptor's callable is guaranteed live for the call.
///
/// `FunctionRef` is trivially copyable, trivially destructible, and may be passed in registers
/// across function-call boundaries. It is NOT thread-safe by construction: each user owns its own
/// instance and may invoke it from one thread at a time.
///
/// Storing a `FunctionRef` as a class data member or returning one from a function that
///          constructed the underlying callable creates a dangling pointer; use of the returned
///          reference is undefined behavior.
///
/// R    Return type of the wrapped callable.
/// Args Argument types of the wrapped callable.
template <class R, class... Args> class FunctionRef<R(Args...)> {
public:
  /// Construct an empty `FunctionRef`. Invoking an empty instance is undefined behavior.
  constexpr FunctionRef() noexcept = default;

  /// Bind to a callable |fn| living in the caller's storage.
  ///
  /// The constructor stores a non-owning pointer to |fn| and a thunk that invokes |fn| through
  /// the erased pointer. Any callable whose type's invocation produces something convertible to
  /// `R` from `Args...` is accepted; the SFINAE constraint excludes recursive `FunctionRef`-of-
  /// `FunctionRef` self-binding to keep copy semantics intact.
  ///
  /// F The deduced callable type. Must not itself be `FunctionRef<R(Args...)>`.
  /// fn Reference to the callable; its lifetime must exceed every invocation through this
  ///           `FunctionRef`.
  template <class F>
    requires(!std::is_same_v<std::remove_cv_t<std::remove_reference_t<F>>, FunctionRef> &&
             std::is_invocable_r_v<R, F &, Args...>)
  // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
  constexpr FunctionRef(F &&fn) noexcept
      : m_obj(const_cast<void *>(static_cast<const void *>(std::addressof(fn)))),
        m_invoke(&FunctionRef::template invokeImpl<std::remove_reference_t<F>>) {}

  /// Invoke the bound callable.
  ///
  /// Forwards |args| through the thunk to the bound callable. Behavior is undefined if the
  /// underlying callable has been destroyed or this `FunctionRef` is empty.
  ///
  /// args Arguments forwarded to the bound callable.
  /// The bound callable's return value.
  R operator()(Args... args) const { return m_invoke(m_obj, std::forward<Args>(args)...); }

  /// Equality on (object pointer, thunk pointer). Used by descriptor write elision: when the
  /// same callable is re-bound across back-to-back calls (steady-state bench loop), the
  /// producer skips the redundant store and keeps the descriptor cache line MESI-Shared.
  constexpr bool operator==(const FunctionRef &other) const noexcept {
    return m_obj == other.m_obj && m_invoke == other.m_invoke;
  }
  constexpr bool operator!=(const FunctionRef &other) const noexcept { return !(*this == other); }

  /// Test whether the `FunctionRef` is bound to a callable.
  /// `true` if a callable is bound, `false` if the instance is default-constructed.
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return m_invoke != nullptr; }

private:
  /// Static thunk that downcasts the erased pointer back to the source type and invokes it.
  ///
  /// Lives as a function pointer in the `m_invoke` slot so the `FunctionRef` pays no virtual call
  /// cost. Visible only inside the class; users construct the binding via the converting ctor.
  ///
  /// F    Concrete callable type recovered from the erased pointer.
  /// obj   Erased pointer to the source callable.
  /// args  Arguments forwarded to the recovered callable.
  /// The recovered callable's return value.
  template <class F> static R invokeImpl(void *obj, Args... args) {
    return (*static_cast<F *>(obj))(std::forward<Args>(args)...);
  }

  /// Erased pointer to the bound callable; null when the `FunctionRef` is empty.
  void *m_obj = nullptr;
  /// Thunk that recovers the source type and invokes the callable; null when empty.
  R (*m_invoke)(void *, Args...) = nullptr;
};

// 16-byte size invariant: two pointers, no padding. The descriptor layout invariant and the dispatch budget
// depend on this shape; if a future compiler regresses it, the hot-dispatch budget
// breaks.
static_assert(sizeof(FunctionRef<void(std::size_t, std::size_t)>) == 16,
              "FunctionRef must be exactly two pointers (16 bytes on x86-64)");

} // namespace citor
