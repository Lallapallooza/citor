#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

#include "citor/hints.h"

namespace citor::detail {

// Lock-free single-owner / multi-stealer work-stealing deque.
//
// Implements the Le / Pop / Cohen / Nardelli formulation (PPoPP 2013) of the
// Chase / Lev deque with explicit C++20 memory orders. The deque has a single
// owner thread that performs `push` and `pop` on the bottom; any number of
// stealer threads concurrently call `steal` at the top. The owner never
// resizes during a steal: growth happens only on the owner's `push` hot path
// when the underlying ring buffer is full.
//
// Memory-order summary (verbatim from Le 2013 figure 1):
//   - push: relaxed-load `bottom`, relaxed-load `top`, optional grow,
//     relaxed array store, release-store new `bottom`.
//   - pop: relaxed-store `bottom = bottom - 1`, seq_cst fence, relaxed-load
//     `top`, relaxed array load. On size 0 (empty) the owner restores
//     `bottom`. On size 1 the owner CAS-tightens `top` (seq_cst success,
//     relaxed failure) to settle the steal race.
//   - steal: acquire-load `top`, seq_cst fence, acquire-load `bottom`. If
//     empty, abandon. If non-empty, acquire-load array slot, then CAS `top`
//     (seq_cst success, relaxed failure). The `top` CAS is what serializes
//     against a concurrent `pop` on the last item.
//
// The internal buffer is a power-of-two `Array` indexed modulo capacity.
// Growth allocates a new `Array`, copies the live elements (those between
// `top` and `bottom`), and links the old array via a freelist that is only
// reaped at deque destruction time. A stealer holding a pointer into the old
// array remains valid for the rest of its current steal attempt because the
// freelist never frees mid-flight (Le 2013 section 3 footnote 2).
//
// Termination: at deque destruction, every owned `Array` (including any
// superseded ones pinned by an outstanding stealer) is freed via
// `reapAllArrays`. The owner is responsible for draining all in-flight steals
// before destroying the deque; the synchronous primitive that owns the deque
// joins on every worker before the deque goes out of scope.
template <class T>
class ChaseLevDeque {
public:
  static_assert(std::is_trivially_copyable_v<T>,
                "ChaseLevDeque payload must be trivially-copyable");
  static_assert(std::is_trivially_destructible_v<T>,
                "ChaseLevDeque payload must be trivially-destructible");

  // Initial capacity for a freshly-constructed deque; a power of two.
  static constexpr std::size_t kInitialCapacity = 64;

  // Construct an empty deque sized to |initialCapacity|, rounded up to a
  // power of two and clamped to at least `kInitialCapacity`.
  explicit ChaseLevDeque(std::size_t initialCapacity = kInitialCapacity) {
    const std::size_t cap = std::max(
        roundUpPow2(initialCapacity > 0 ? initialCapacity : kInitialCapacity),
        kInitialCapacity);
    auto *arr = allocateArray(cap);
    m_array.store(arr, std::memory_order_relaxed);
  }

  // Deques are pinned in the worker-state block; copying would require
  // duplicating the atomics.
  ChaseLevDeque(const ChaseLevDeque &) = delete;
  ChaseLevDeque &operator=(const ChaseLevDeque &) = delete;
  ChaseLevDeque(ChaseLevDeque &&) = delete;
  ChaseLevDeque &operator=(ChaseLevDeque &&) = delete;

  // Free every `Array` ever allocated by this deque: the active array plus
  // every retired array still pinned in the freelist. The owner has joined
  // with every stealer by construction, so no array can be in use here.
  ~ChaseLevDeque() {
    Array *active = m_array.load(std::memory_order_relaxed);
    deleteArray(active);
    Array *retired = m_freelist.load(std::memory_order_relaxed);
    while (retired != nullptr) {
      Array *next = retired->next;
      deleteArray(retired);
      retired = next;
    }
  }

  // Owner-only: pre-grow the underlying ring buffer so the next |needed|
  // pushes do not trigger an allocation. No-op when capacity is already
  // sufficient. Bulk-push call sites (e.g. `forkJoinAll` with many roots)
  // call this once to fold N growth allocations into at most one.
  void reserveOwner(std::size_t needed) {
    Array *arr = m_array.load(std::memory_order_relaxed);
    if (needed + 1U <= arr->capacity) {
      return;
    }
    const std::int64_t b = m_bottom.load(std::memory_order_relaxed);
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    std::size_t newCap = arr->capacity;
    while (newCap < needed + 1U) {
      newCap *= 2;
    }
    Array *newArr = allocateArray(newCap);
    for (std::int64_t i = t; i < b; ++i) {
      newArr->store(i, arr->load(i));
    }
    m_array.store(newArr, std::memory_order_release);
    Array *head = m_freelist.load(std::memory_order_relaxed);
    do {
      arr->next = head;
    } while (!m_freelist.compare_exchange_weak(
        head, arr, std::memory_order_release, std::memory_order_relaxed));
  }

  // Owner-only: push a value onto the bottom of the deque.
  //
  // Resizes the underlying ring buffer to twice its capacity when the buffer
  // is full. The grow path allocates a fresh `Array`, copies live elements,
  // and chains the old array onto a freelist so concurrent stealers'
  // acquire-loaded array pointers remain valid for the rest of their attempt.
  void push(T value) {
    const std::int64_t b = m_bottom.load(std::memory_order_relaxed);
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    Array *arr = m_array.load(std::memory_order_relaxed);
    const std::int64_t size = b - t;
    if (size >= static_cast<std::int64_t>(arr->capacity) - 1) {
      arr = grow(arr, b, t);
    }
    arr->store(b, value);
    std::atomic_thread_fence(std::memory_order_release);
    m_bottom.store(b + 1, std::memory_order_relaxed);
  }

  // Owner-only: pop a value from the bottom of the deque.
  //
  // Returns `std::nullopt` when the deque is empty. The last-item race with a
  // concurrent `steal` is settled via a seq_cst CAS on `top`: only one of
  // the two participants succeeds.
  std::optional<T> pop() noexcept {
    const Array *arr = m_array.load(std::memory_order_relaxed);
    const std::int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t t = m_top.load(std::memory_order_relaxed);
    if (t > b) {
      // Empty.
      m_bottom.store(b + 1, std::memory_order_relaxed);
      return std::nullopt;
    }
    const T value = arr->load(b);
    if (t != b) {
      // More than one element; the load is uncontended.
      return value;
    }
    // Last item: race against any in-flight stealer.
    const bool won = m_top.compare_exchange_strong(
        t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
    m_bottom.store(b + 1, std::memory_order_relaxed);
    if (!won) {
      return std::nullopt;
    }
    return value;
  }

  // Stealer: try to steal a value from the top of the deque.
  //
  // Returns `std::nullopt` on contention or empty. The caller retries (or
  // moves to another victim) when steal returns empty without distinguishing
  // the two cases; the canonical Chase-Lev formulation collapses them. The
  // `top` CAS uses seq_cst on success, relaxed on failure, matching Le 2013
  // figure 1.
  std::optional<T> steal() noexcept {
    std::int64_t t = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::int64_t b = m_bottom.load(std::memory_order_acquire);
    if (t >= b) {
      return std::nullopt;
    }
    const Array *arr = m_array.load(std::memory_order_acquire);
    const T value = arr->load(t);
    if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                       std::memory_order_relaxed)) {
      return std::nullopt;
    }
    return value;
  }

  // Stealer-friendly empty-probe used on the worker poll fast path.
  //
  // Reads `top` (acquire) and `bottom` (acquire) and returns true when `top
  // >= bottom`. The relaxed orders the canonical Chase-Lev empty-check would
  // use are insufficient here because the result is consumed before the
  // steal CAS; the acquire-loads are paired with the release-store on the
  // owner's `push` so the worker observes any payload published before the
  // steal probe.
  [[nodiscard]] bool empty() const noexcept {
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    const std::int64_t b = m_bottom.load(std::memory_order_acquire);
    return t >= b;
  }

  // Owner-side observation of the deque's logical size. Suitable for debug
  // assertions; not for hot-path scheduling, since the value can be
  // invalidated by an in-flight steal between the load and the consumer.
  [[nodiscard]] std::size_t size() const noexcept {
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    const std::int64_t b = m_bottom.load(std::memory_order_acquire);
    return (b > t) ? static_cast<std::size_t>(b - t) : std::size_t{0};
  }

  // Owner-side query: current capacity of the underlying ring buffer; always
  // a power of two.
  [[nodiscard]] std::size_t capacity() const noexcept {
    return m_array.load(std::memory_order_relaxed)->capacity;
  }

private:
  // Ring-buffer backing storage. Indexed modulo `capacity`. `next` chains
  // retired buffers.
  struct Array {
    // Ring-buffer capacity in elements; a power of two.
    std::size_t capacity = 0;
    // Power-of-two mask, equal to `capacity - 1`.
    std::size_t mask = 0;
    // Next retired buffer in the freelist; `nullptr` if not retired.
    Array *next = nullptr;
    // Trailing storage of `capacity` slots; flexible array layout via
    // `operator new`. The declared length of 1 is a placeholder; the actual
    // element count is `capacity` and is allocated by `allocateArray` via a
    // single oversized `operator new` call.
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    std::atomic<T> slots[1];

    // Store |v| at logical index |i| (mod capacity).
    //
    // Release ordering pairs with the matching acquire load on the steal path
    // so a stealer that acquire-loads `bottom` and then loads this slot
    // observes everything the owner wrote before the corresponding push,
    // including the descriptor fields the stolen payload points at. The Le
    // 2013 push protocol's release fence + relaxed `bottom` store would be
    // sufficient on its own, but the explicit per-slot release lets
    // ThreadSanitizer model the cross-thread happens-before edge directly
    // without relying on the fence-relaxed-store equivalence.
    void store(std::int64_t i, T v) noexcept {
      slots[static_cast<std::size_t>(i) & mask].store(
          v, std::memory_order_release);
    }

    // Load the value at logical index |i| (mod capacity).
    //
    // Acquire ordering pairs with the per-slot release store on push so a
    // stealer (or the owner after the seq_cst fence in pop) observes the
    // descriptor the slot points to. On x86-64 the acquire load is free; on
    // weaker memory models it is the canonical pairing for the slot.
    [[nodiscard]] T load(std::int64_t i) const noexcept {
      return slots[static_cast<std::size_t>(i) & mask].load(
          std::memory_order_acquire);
    }
  };

  // Allocate a fresh `Array` with the requested power-of-two |cap|. Payload
  // slots are default-initialized to `T{}`.
  static Array *allocateArray(std::size_t cap) {
    const std::size_t headerBytes = offsetof(Array, slots);
    const std::size_t totalBytes = headerBytes + (sizeof(std::atomic<T>) * cap);
    void *raw = ::operator new(totalBytes, std::align_val_t{kCacheLine});
    auto *arr = static_cast<Array *>(raw);
    arr->capacity = cap;
    arr->mask = cap - 1;
    arr->next = nullptr;
    for (std::size_t i = 0; i < cap; ++i) {
      ::new (static_cast<void *>(arr->slots + i)) std::atomic<T>(T{});
    }
    return arr;
  }

  // Free an array previously returned by `allocateArray`. Tolerates nullptr.
  static void deleteArray(Array *arr) noexcept {
    if (arr == nullptr) {
      return;
    }
    for (std::size_t i = 0; i < arr->capacity; ++i) {
      arr->slots[i].~atomic();
    }
    ::operator delete(static_cast<void *>(arr), std::align_val_t{kCacheLine});
  }

  // Owner-side: double the ring buffer's capacity in place.
  //
  // Allocates a new `Array` with `2 * old.capacity` slots, copies every live
  // element, retires the old array onto the freelist (so any in-flight
  // stealer's acquire-loaded pointer remains valid for the rest of its
  // attempt), and publishes the new array via release-store on `m_array`.
  Array *grow(Array *oldArr, std::int64_t b, std::int64_t t) {
    const std::size_t newCap = oldArr->capacity * 2;
    Array *newArr = allocateArray(newCap);
    for (std::int64_t i = t; i < b; ++i) {
      newArr->store(i, oldArr->load(i));
    }
    m_array.store(newArr, std::memory_order_release);
    // Retire `oldArr` onto the freelist; reaped only at deque destruction.
    // A stealer that already loaded `oldArr` via acquire load on `m_array`
    // keeps a valid pointer for the remainder of its current steal attempt.
    Array *head = m_freelist.load(std::memory_order_relaxed);
    do {
      oldArr->next = head;
    } while (!m_freelist.compare_exchange_weak(
        head, oldArr, std::memory_order_release, std::memory_order_relaxed));
    return newArr;
  }

  // Round |v| up to the next power of two. Input must be at least 1.
  static constexpr std::size_t roundUpPow2(std::size_t v) noexcept {
    if (v <= 1) {
      return 1;
    }
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if constexpr (sizeof(std::size_t) > 4) {
      v |= v >> 32;
    }
    return v + 1;
  }

  // Owner-incremented push index. Release-stored after a successful push.
  alignas(kCacheLine) std::atomic<std::int64_t> m_bottom{0};

  // Stealer-incremented pop index. CAS-updated by both the owner's `pop`
  // last-item branch and every successful `steal`.
  alignas(kCacheLine) std::atomic<std::int64_t> m_top{0};

  // Active backing array. Replaced via release-store by `grow`.
  alignas(kCacheLine) std::atomic<Array *> m_array{nullptr};

  // Singly-linked freelist of retired arrays; reaped at destruction time.
  alignas(kCacheLine) std::atomic<Array *> m_freelist{nullptr};
};

} // namespace citor::detail
