// include/ob/engine_concept.hpp
#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <ob/command.hpp>
#include <ob/events.hpp>
#include <span>
#include <utility>

namespace ob {

// A non-owning, fixed-capacity output buffer. The engine never owns output
// memory; that is what makes "no allocation on the hot path" a true statement
// rather than an aspiration.
class EventBuffer {
public:
    EventBuffer(Event* data, std::size_t capacity) noexcept : data_(data), capacity_(capacity) {}

    void push(const Event& e) noexcept {
        // Overflow is a caller programming error (spec E47). Asserting keeps the
        // "at least one event per command" contract honest; truncating would not.
        assert(n_ < capacity_ && "EventBuffer overflow: caller under-sized the buffer");
        data_[n_++] = e;
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool        empty() const noexcept { return n_ == 0; }
    void                      clear() noexcept { n_ = 0; }

    [[nodiscard]] const Event& operator[](std::size_t i) const noexcept {
        assert(i < n_);
        return data_[i];
    }

    [[nodiscard]] std::span<const Event> view() const noexcept { return {data_, n_}; }

    [[nodiscard]] const Event* begin() const noexcept { return data_; }
    [[nodiscard]] const Event* end() const noexcept { return data_ + n_; }

private:
    Event*      data_     = nullptr;
    std::size_t capacity_ = 0;
    std::size_t n_        = 0;
};

namespace detail {
// Holds the storage so that it is initialised BEFORE the EventBuffer base below,
// because base classes are initialised in declaration order while members are
// initialised after all bases. Passing a member's address to a base constructor
// is the classic "base-from-member" problem: legal, but -Wuninitialized flags it
// and the warning is right to. Making the storage its own base fixes the ordering
// instead of arguing with the compiler about it.
template <std::size_t N>
struct EventStorage {
    std::array<Event, N> data{};
};
}  // namespace detail

// Convenience for tests and tools: an EventBuffer that owns inline storage.
// Not used on a measured hot path.
template <std::size_t N>
class FixedEventBuffer : private detail::EventStorage<N>, public EventBuffer {
public:
    // detail::EventStorage<N> is declared first, so its `data` array is fully
    // initialised by the time EventBuffer's constructor runs.
    FixedEventBuffer() noexcept : EventBuffer(detail::EventStorage<N>::data.data(), N) {}

    FixedEventBuffer(const FixedEventBuffer&)            = delete;
    FixedEventBuffer& operator=(const FixedEventBuffer&) = delete;
};

// The compile-time engine interface. A concept rather than a virtual base class:
// the benchmark must not measure vtable dispatch, and the optimizer has to be
// able to inline across this boundary. Phase 3 extends this with snapshot_l2.
template <class E>
concept Engine = requires(E e, const Command& c, EventBuffer& out) {
    { e.submit(c, out) } -> std::same_as<void>;
    { std::as_const(e).best_bid() } -> std::same_as<Ticks>;
    { std::as_const(e).best_ask() } -> std::same_as<Ticks>;
    { e.reset() } -> std::same_as<void>;
};

}  // namespace ob
