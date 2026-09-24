// include/ob/order.hpp
#pragma once

#include <ob/types.hpp>

#include <type_traits>

namespace ob {

// Exactly 32 bytes: 2 per 64-byte cache line on x86, 4 per 128-byte line on Apple
// Silicon. Every field is load-bearing and there is no room for anything else,
// which is why the arrival sequence used by the invariant checker lives only in
// ReferenceEngine (see kTracksArrival).
//
// `next` does double duty: the per-level FIFO link while the slot is live, and the
// free-list link while it is not. The two lifetimes are disjoint.
struct Order {
    OrderId       id        = 0;             // 8.  0 means this slot is free.
    Ticks         price     = kNoPrice;      // 4
    Qty           remaining = 0;             // 4
    Slot          next      = kInvalidSlot;  // 4
    Slot          prev      = kInvalidSlot;  // 4
    Side          side      = Side::Buy;     // 1
    std::uint8_t  flags     = 0;             // 1.  reserved: participant / STP
    std::uint16_t pad       = 0;             // 2
};

static_assert(sizeof(Order) == 32, "Order must stay 32 bytes");
static_assert(alignof(Order) == 8);
static_assert(std::is_trivially_copyable_v<Order>);

}  // namespace ob
