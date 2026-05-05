#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace citor {

// Primary template; the specialization below handles function-type signatures.
// Instantiating `FunctionRef` with a non-function type produces a compile-time
// diagnostic.
template <class Sig>
class FunctionRef;

// Non-owning callable reference; analogous to `std::string_view` for callables.
// Stores two pointers: an erased object pointer and a thunk that invokes the
// original callable through it.
//
// Parameter-only: the bound callable's lifetime must exceed every invocation
// through this `FunctionRef`. Storing one as a class data member, or returning
// one from a function that constructed the bound callable, creates a dangling
// pointer.
//
// The shape (16 bytes, two pointers, no allocation) matches
// `llvm::function_ref`, `absl::FunctionRef`, and `folly::FunctionRef`.
// Trivially copyable, trivially destructible, and passable in registers. Each
// instance is single-owner; concurrent invocation from multiple threads is
// undefined.
template <class R, class... Args>
class FunctionRef<R(Args...)> {
public:
  // Construct an empty `FunctionRef`. Invoking an empty instance is undefined.
  constexpr FunctionRef() noexcept = default;

  // Bind to a callable |fn| living in the caller's storage. Stores a non-owning
  // pointer to |fn| and a thunk that invokes it through the erased pointer.
  // The SFINAE constraint excludes `FunctionRef`-of-`FunctionRef` self-binding
  // so copy semantics stay intact.
  template <class F>
    requires(!std::is_same_v<std::remove_cv_t<std::remove_reference_t<F>>,
                             FunctionRef> &&
             std::is_invocable_r_v<R, F &, Args...>)
  // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
  constexpr FunctionRef(F &&fn) noexcept
      : m_obj(
            const_cast<void *>(static_cast<const void *>(std::addressof(fn)))),
        m_invoke(
            &FunctionRef::template invokeImpl<std::remove_reference_t<F>>) {}

  // Forward |args| through the thunk to the bound callable. Behavior is
  // undefined if the underlying callable has been destroyed or this
  // `FunctionRef` is empty.
  R operator()(Args... args) const {
    return m_invoke(m_obj, std::forward<Args>(args)...);
  }

  // Equality on (object pointer, thunk pointer). Used by descriptor write
  // elision: when the same callable is re-bound across back-to-back calls
  // (steady-state bench loop), the producer skips the redundant store and
  // keeps the descriptor cache line MESI-Shared.
  constexpr bool operator==(const FunctionRef &other) const noexcept {
    return m_obj == other.m_obj && m_invoke == other.m_invoke;
  }
  constexpr bool operator!=(const FunctionRef &other) const noexcept {
    return !(*this == other);
  }

  // Returns `true` if a callable is bound; `false` for a default-constructed
  // instance.
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return m_invoke != nullptr;
  }

private:
  // Static thunk that downcasts the erased pointer back to the source type
  // and invokes it. Lives as a function pointer in the `m_invoke` slot so the
  // `FunctionRef` pays no virtual call cost.
  template <class F>
  static R invokeImpl(void *obj, Args... args) {
    return (*static_cast<F *>(obj))(std::forward<Args>(args)...);
  }

  // Erased pointer to the bound callable; null when empty.
  void *m_obj = nullptr;
  // Thunk that recovers the source type and invokes the callable; null when
  // empty.
  R (*m_invoke)(void *, Args...) = nullptr;
};

// Two-pointer (16-byte) layout invariant. The dispatch hot path depends on
// this shape; if a future compiler regresses it, the hot-dispatch budget
// breaks.
static_assert(sizeof(FunctionRef<void(std::size_t, std::size_t)>) == 16,
              "FunctionRef must be exactly two pointers (16 bytes on x86-64)");

} // namespace citor
