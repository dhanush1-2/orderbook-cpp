// include/ob/order_pool.hpp
#pragma once

// Pre-allocated arena of order slots with an O(1) index free list.
//
// Allocates exactly once, in the constructor. After that, alloc() and free() are
// a handful of instructions and touch no allocator. Exhaustion returns
// kInvalidSlot, which the engine turns into Rejected(EngineCapacity) (spec E39).

#include <ob/order.hpp>

#include <cassert>
#include <cstddef>
#include <vector>

namespace ob {

class OrderPool {
public:
    explicit OrderPool(std::size_t capacity) : slots_(capacity) {
        assert(capacity > 0 && capacity < kInvalidSlot &&
               "capacity must be positive and leave kInvalidSlot free as a sentinel");
        build_free_list();
    }

    // kInvalidSlot when full. The returned slot's id is 0 until the caller sets it.
    [[nodiscard]] Slot alloc() noexcept {
        if (free_head_ == kInvalidSlot) {
            return kInvalidSlot;
        }
        const Slot s = free_head_;
        assert(slots_[s].id == 0 && "free-list slot was still live");
        free_head_     = slots_[s].next;
        slots_[s].next = kInvalidSlot;
        slots_[s].prev = kInvalidSlot;
        ++live_;
        return s;
    }

    void free(Slot s) noexcept {
        assert(s < slots_.size() && "slot out of range");
        // id == 0 means already free. A double free would splice a cycle into the
        // free list and the symptom would surface arbitrarily far from the cause.
        assert(slots_[s].id != 0 && "double free of an order slot");
        slots_[s].id   = 0;
        slots_[s].next = free_head_;
        free_head_     = s;
        --live_;
    }

    [[nodiscard]] Order& at(Slot s) noexcept {
        assert(s < slots_.size() && "slot out of range");
        return slots_[s];
    }
    [[nodiscard]] const Order& at(Slot s) const noexcept {
        assert(s < slots_.size() && "slot out of range");
        return slots_[s];
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return live_; }
    [[nodiscard]] bool full() const noexcept { return free_head_ == kInvalidSlot; }

    // Debug helper for the invariant checker. Returns the free-list length, or
    // SIZE_MAX if the walk exceeds capacity, which means the list has a cycle.
    [[nodiscard]] std::size_t free_list_length() const noexcept {
        std::size_t n   = 0;
        Slot        cur = free_head_;
        while (cur != kInvalidSlot) {
            if (n > slots_.size()) {
                return static_cast<std::size_t>(-1);  // cycle
            }
            cur = slots_[cur].next;
            ++n;
        }
        return n;
    }

    // No prefault() here on purpose. std::vector<Order> slots_(capacity)
    // value-initializes every element, which WRITES every byte, which faults in
    // every page. The arena is therefore resident from construction and a separate
    // prefault would be a no-op wearing a reassuring name. Cache warming is a
    // different problem, handled by the harness's discarded warm-up iterations.
    void reset() noexcept {
        for (Order& o : slots_) {
            o = Order{};
        }
        live_ = 0;
        build_free_list();
    }

private:
    void build_free_list() noexcept {
        const std::size_t n = slots_.size();
        for (std::size_t i = 0; i + 1 < n; ++i) {
            slots_[i].next = static_cast<Slot>(i + 1);
        }
        slots_[n - 1].next = kInvalidSlot;
        free_head_         = 0;
    }

    std::vector<Order> slots_;
    Slot               free_head_ = kInvalidSlot;
    std::size_t        live_      = 0;
};

}  // namespace ob
