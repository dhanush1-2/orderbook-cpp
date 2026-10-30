// include/ob/spsc_ring.hpp
#pragma once

// Lock-free single-producer, single-consumer ring.
//
// Exactly one thread may push and exactly one may pop. Each index has one writer, so
// no read-modify-write is needed, and the indices run freely: full is
// `head - tail == Capacity`, which leaves every slot usable.
//
// Ordering: the release store of an index publishes the slot access before it, and the
// acquire load on the other side synchronizes with it. Nothing here needs a single
// total order across both indices, so seq_cst would only add cost (a full barrier on
// x86-64). With relaxed on head_, the consumer could read a slot before its contents
// are visible.
//
// Each side keeps a private copy of the other's index and re-reads the shared one only
// when that copy says full or empty, so the common path does not pull the other core's
// cache line. A stale copy is safe: it can only understate the free or filled slots.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ob/cache_line.hpp>
#include <type_traits>

namespace ob {

template <class T, std::size_t Capacity>
    requires(std::is_trivially_copyable_v<T> && Capacity >= 2 && std::has_single_bit(Capacity))
class SpscRing {
public:
    // Allocates once, here, so that neither push nor pop ever does.
    SpscRing() : slots_(std::make_unique<T[]>(Capacity)) {}
    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer thread only. Returns false when full.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        if (head - cached_tail_ == Capacity) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head - cached_tail_ == Capacity) {
                return false;
            }
        }
        slots_[head & kMask] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returns false when empty, leaving `out` untouched.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) {
                return false;
            }
        }
        out = slots_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // A snapshot for monitoring and for tests once the threads have stopped. Tail is
    // read first so the difference cannot go negative; the clamp covers the producer
    // advancing between the two loads.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(std::min<std::uint64_t>(head - tail, Capacity));
    }
    [[nodiscard]] bool                         empty() const noexcept { return size_approx() == 0; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    std::unique_ptr<T[]> slots_;

    alignas(detail::kCacheLine) std::atomic<std::uint64_t> head_{0};
    alignas(detail::kCacheLine) std::uint64_t cached_tail_ = 0;  // producer-private
    alignas(detail::kCacheLine) std::atomic<std::uint64_t> tail_{0};
    alignas(detail::kCacheLine) std::uint64_t cached_head_ = 0;  // consumer-private
};

}  // namespace ob
