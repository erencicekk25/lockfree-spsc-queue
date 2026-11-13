// SPDX-License-Identifier: MIT
//
// lockfree-spsc-queue: a header-only, dependency-free, bounded
// single-producer / single-consumer (SPSC) ring buffer for C++17.
//
// ---------------------------------------------------------------------------
// Memory layout
// ---------------------------------------------------------------------------
//
// The queue object itself is split into three cache-line sized blocks so that
// the producer and the consumer never write to the same cache line:
//
//   offset   0 +-------------------------------------------------------+
//              | producer_  : head (atomic write index), cached_tail     |
//   offset  64 +-------------------------------------------------------+
//              | consumer_  : tail (atomic read index),  cached_head     |
//   offset 128 +-------------------------------------------------------+
//              | config_    : slot_count, slots pointer (read-only)      |
//   offset 192 +-------------------------------------------------------+
//
// - `head` is written only by the producer, `tail` only by the consumer.
// - Each side keeps a private, non-atomic cache of the other side's index
//   (`cached_tail` / `cached_head`) on its *own* cache line, so the shared
//   index is only re-read when the cached value suggests the queue is
//   full (producer) or empty (consumer).
// - `config_` is written once in the constructor and only read afterwards,
//   so it can be shared by both cores without coherence traffic.
//
// The slot storage is a separate heap allocation:
//
//   [ padding ][ slot 0 ][ slot 1 ] ... [ slot N ][ padding ]
//
// The leading and trailing padding (at least one cache line each) keeps the
// first and last slots from sharing a cache line with unrelated heap objects.
//
// One slot is always left empty to distinguish "full" from "empty", so a
// queue created with capacity N allocates N + 1 slots (plus padding).
// ---------------------------------------------------------------------------

#ifndef LOCKFREE_SPSC_SPSC_QUEUE_HPP
#define LOCKFREE_SPSC_SPSC_QUEUE_HPP

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lockfree_spsc {

/// Assumed size of a cache line in bytes.
///
/// `std::hardware_destructive_interference_size` is intentionally not used:
/// its value is not ABI-stable across compiler flags and several toolchains
/// either lack it or warn when it is used in headers.
inline constexpr std::size_t cache_line_size = 64;

/// Bounded, lock-free, single-producer / single-consumer FIFO queue.
///
/// Thread-safety contract:
/// - Exactly one thread may act as the producer at any time.
/// - Exactly one thread may act as the consumer at any time.
/// - Construction and destruction must not race with any other operation.
///
/// @tparam T  Element type. Must be a non-const, non-volatile object type
///            with a non-throwing destructor.
template <typename T>
class spsc_queue {
    static_assert(std::is_object_v<T>, "spsc_queue<T>: T must be an object type");
    static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>,
                  "spsc_queue<T>: T must not be cv-qualified");
    static_assert(!std::is_array_v<T>, "spsc_queue<T>: T must not be an array type");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "spsc_queue<T>: T must have a non-throwing destructor");

public:
    using value_type = T;
    using size_type = std::size_t;
    using reference = T&;
    using const_reference = const T&;

    /// Creates an empty queue able to hold up to `capacity` elements.
    ///
    /// The only throwing step is the single slot allocation; no element is
    /// constructed here, so a failed construction leaks nothing.
    ///
    /// @throws std::invalid_argument if `capacity == 0`.
    /// @throws std::length_error     if the requested capacity is too large.
    /// @throws std::bad_alloc        if the slot storage cannot be allocated.
    explicit spsc_queue(size_type capacity) : config_{checked_slot_count(capacity), nullptr} {
        allocator_type allocator;
        config_.slots = allocator_traits::allocate(allocator, allocation_size());
    }

    /// Destroys all elements that are still enqueued and releases storage.
    ///
    /// Must not race with any producer or consumer operation.
    ~spsc_queue() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            size_type tail = consumer_.tail.load(std::memory_order_relaxed);
            const size_type head = producer_.head.load(std::memory_order_relaxed);
            while (tail != head) {
                slot(tail)->~T();
                tail = next_index(tail);
            }
        }
        allocator_type allocator;
        allocator_traits::deallocate(allocator, config_.slots, allocation_size());
    }

    // The queue owns raw storage and is shared between threads by reference;
    // copying or moving it would break that sharing, so both are disabled.
    spsc_queue(const spsc_queue&) = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;
    spsc_queue(spsc_queue&&) = delete;
    spsc_queue& operator=(spsc_queue&&) = delete;

    /// Maximum number of elements the queue can hold.
    [[nodiscard]] size_type capacity() const noexcept { return config_.slot_count - 1; }

    /// Number of enqueued elements.
    ///
    /// Exact when called by the producer or consumer while the other side is
    /// idle; otherwise a snapshot that may be stale by the time it is used.
    /// The result is always in the range [0, capacity()].
    [[nodiscard]] size_type size() const noexcept {
        const size_type head = producer_.head.load(std::memory_order_acquire);
        const size_type tail = consumer_.tail.load(std::memory_order_acquire);
        return head >= tail ? head - tail : head + config_.slot_count - tail;
    }

    /// Whether the queue is empty (snapshot semantics, see size()).
    [[nodiscard]] bool empty() const noexcept {
        return producer_.head.load(std::memory_order_acquire) ==
               consumer_.tail.load(std::memory_order_acquire);
    }

    // -----------------------------------------------------------------------
    // Producer operations (call from the single producer thread only)
    // -----------------------------------------------------------------------

    /// Constructs an element in place from `args`.
    ///
    /// @return `false` (and constructs nothing) if the queue is full.
    ///         If the constructor throws, the queue is unchanged.
    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>) {
        static_assert(std::is_constructible_v<T, Args&&...>,
                      "spsc_queue<T>::try_emplace: T is not constructible from Args");
        return push_impl(std::forward<Args>(args)...);
    }

    /// Copies `value` into the queue.
    ///
    /// @return `false` (and leaves the queue unchanged) if the queue is full.
    ///         If the copy constructor throws, the queue is also unchanged.
    [[nodiscard]] bool try_push(const T& value) noexcept(
        std::is_nothrow_copy_constructible_v<T>) {
        static_assert(std::is_copy_constructible_v<T>,
                      "spsc_queue<T>::try_push(const T&) requires a copyable T");
        return push_impl(value);
    }

    /// Moves `value` into the queue.
    ///
    /// @return `false` if the queue is full; `value` is then left untouched.
    [[nodiscard]] bool try_push(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        static_assert(std::is_move_constructible_v<T>,
                      "spsc_queue<T>::try_push(T&&) requires a movable T");
        return push_impl(std::move(value));
    }

    // -----------------------------------------------------------------------
    // Consumer operations (call from the single consumer thread only)
    // -----------------------------------------------------------------------

    /// Move-assigns the oldest element into `out` and removes it.
    ///
    /// @return `false` (and leaves `out` untouched) if the queue is empty.
    ///         If the move assignment throws, the element stays enqueued.
    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        static_assert(std::is_move_assignable_v<T>,
                      "spsc_queue<T>::try_pop(T&) requires a move-assignable T");
        const size_type tail = consumer_.tail.load(std::memory_order_relaxed);
        if (!readable(tail)) {
            return false;
        }
        T* const element = slot(tail);
        out = std::move(*element);
        release_slot(element, tail);
        return true;
    }

    /// Removes the oldest element and returns it by value.
    ///
    /// @return `std::nullopt` if the queue is empty. If the move constructor
    ///         throws, the element stays enqueued.
    [[nodiscard]] std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
        static_assert(std::is_move_constructible_v<T>,
                      "spsc_queue<T>::try_pop() requires a move-constructible T");
        const size_type tail = consumer_.tail.load(std::memory_order_relaxed);
        if (!readable(tail)) {
            return std::nullopt;
        }
        T* const element = slot(tail);
        std::optional<T> result{std::in_place, std::move(*element)};
        release_slot(element, tail);
        return result;
    }

    /// Zero-copy access to the oldest element.
    ///
    /// @return A pointer to the oldest element, or `nullptr` if the queue is
    ///         empty. The pointer stays valid until the next call to pop().
    [[nodiscard]] T* front() noexcept {
        const size_type tail = consumer_.tail.load(std::memory_order_relaxed);
        return readable(tail) ? slot(tail) : nullptr;
    }

    /// Destroys the oldest element.
    ///
    /// @pre front() returned a non-null pointer since the last pop().
    void pop() noexcept {
        const size_type tail = consumer_.tail.load(std::memory_order_relaxed);
        assert(readable(tail) && "spsc_queue::pop() called on an empty queue");
        release_slot(slot(tail), tail);
    }

private:
    // Stateless standard allocator; handles over-aligned T via aligned new.
    using allocator_type = std::allocator<T>;
    using allocator_traits = std::allocator_traits<allocator_type>;

    /// Number of padding slots placed before and after the live slots so that
    /// the ring never shares a cache line with neighbouring heap objects.
    static constexpr size_type padding_slots = (cache_line_size - 1) / sizeof(T) + 1;

    /// Producer-owned cache line.
    struct alignas(cache_line_size) producer_state {
        std::atomic<size_type> head{0};  ///< Next slot to write (published).
        size_type cached_tail{0};        ///< Producer's last view of `tail`.
    };

    /// Consumer-owned cache line.
    struct alignas(cache_line_size) consumer_state {
        std::atomic<size_type> tail{0};  ///< Next slot to read (published).
        size_type cached_head{0};        ///< Consumer's last view of `head`.
    };

    /// Read-only after construction.
    struct alignas(cache_line_size) config_state {
        size_type slot_count;  ///< capacity + 1 (one slot is always empty).
        T* slots;              ///< Start of the allocation, including padding.
    };

    static_assert(sizeof(producer_state) == cache_line_size,
                  "producer state must occupy exactly one cache line");
    static_assert(sizeof(consumer_state) == cache_line_size,
                  "consumer state must occupy exactly one cache line");
    static_assert(sizeof(config_state) == cache_line_size,
                  "config state must occupy exactly one cache line");
    static_assert(std::atomic<size_type>::is_always_lock_free,
                  "spsc_queue requires lock-free atomic indices");

    [[nodiscard]] static size_type checked_slot_count(size_type capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("spsc_queue: capacity must be greater than zero");
        }
        constexpr size_type max_slots =
            std::numeric_limits<size_type>::max() / sizeof(T) - 2 * padding_slots;
        if (capacity >= max_slots) {
            throw std::length_error("spsc_queue: capacity is too large");
        }
        return capacity + 1;
    }

    [[nodiscard]] size_type allocation_size() const noexcept {
        return config_.slot_count + 2 * padding_slots;
    }

    [[nodiscard]] T* slot(size_type index) const noexcept {
        return config_.slots + padding_slots + index;
    }

    [[nodiscard]] size_type next_index(size_type index) const noexcept {
        const size_type next = index + 1;
        return next == config_.slot_count ? 0 : next;
    }

    // Memory ordering protocol
    // ------------------------
    // Producer:  construct element in slot[head]  ->  head.store(release)
    // Consumer:  head.load(acquire)  ->  read/destroy slot[tail]
    //            ->  tail.store(release)
    // Producer:  tail.load(acquire)  ->  reuse slot
    //
    // The release store of `head` makes the fully constructed element visible
    // to the consumer's acquire load; the release store of `tail` guarantees
    // the element was destroyed before the producer may overwrite the slot.
    // Each side reads its *own* index with relaxed ordering because no other
    // thread ever writes it.

    /// Producer side: constructs a new element in place. Strong exception
    /// guarantee: `head` is only advanced after construction succeeded.
    template <typename... Args>
    [[nodiscard]] bool push_impl(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>) {
        const size_type head = producer_.head.load(std::memory_order_relaxed);
        const size_type next = next_index(head);
        if (next == producer_.cached_tail) {
            producer_.cached_tail = consumer_.tail.load(std::memory_order_acquire);
            if (next == producer_.cached_tail) {
                return false;  // full
            }
        }
        ::new (static_cast<void*>(slot(head))) T(std::forward<Args>(args)...);
        producer_.head.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side: whether slot `tail` holds a published element.
    [[nodiscard]] bool readable(size_type tail) noexcept {
        if (tail == consumer_.cached_head) {
            consumer_.cached_head = producer_.head.load(std::memory_order_acquire);
            if (tail == consumer_.cached_head) {
                return false;  // empty
            }
        }
        return true;
    }

    /// Consumer side: destroys the element at `tail` and hands the slot back
    /// to the producer.
    void release_slot(T* element, size_type tail) noexcept {
        element->~T();
        consumer_.tail.store(next_index(tail), std::memory_order_release);
    }

    producer_state producer_{};
    consumer_state consumer_{};
    config_state config_;
};

}  // namespace lockfree_spsc

#endif  // LOCKFREE_SPSC_SPSC_QUEUE_HPP
