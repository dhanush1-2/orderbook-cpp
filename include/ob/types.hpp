// include/ob/types.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ob {

using OrderId = std::uint64_t;
using Ticks   = std::int32_t;   // price, in whole ticks. never floating point.
using Qty     = std::uint32_t;  // per-order quantity
using QtySum  = std::uint64_t;  // aggregates; wider so level overflow is unreachable
using Seq     = std::uint64_t;  // event sequence number
using Slot    = std::uint32_t;  // index into the order pool

// Ladder bounds. Tick 0 is deliberately invalid so that a zero-initialised or
// default-constructed price can never be mistaken for a real one.
inline constexpr Ticks kMinTick = 1;
inline constexpr Ticks kMaxTick = 65536;
inline constexpr std::size_t kLadderSize =
    static_cast<std::size_t>(kMaxTick - kMinTick + 1);

inline constexpr Slot  kInvalidSlot = 0xFFFF'FFFFu;
inline constexpr Ticks kNoPrice     = std::numeric_limits<Ticks>::min();

// Bounded so that a level's QtySum total cannot overflow: 2^31 per order across
// at most 2^32 orders still fits in 2^63.
inline constexpr Qty kMaxOrderQty = 1u << 31;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : std::uint8_t { Limit = 0, Market, Ioc, Fok, PostOnly };

enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidQuantity,
    PriceOutOfRange,
    DuplicateOrderId,
    UnknownOrderId,
    WouldCross,
    EngineCapacity,
};

enum class CancelReason : std::uint8_t {
    None = 0,
    UserRequested,
    NoLiquidity,
    Unfillable,
    IocRemainder,
};

// A resting order as seen by an external inspector. Used by the invariant checker
// and the L2 publisher so neither needs access to engine internals.
struct RestingOrder {
    Side    side;
    Ticks   price;
    OrderId id;
    Qty     remaining;
    // Arrival order, strictly increasing. Meaningful only when the engine sets
    // E::kTracksArrival; FastEngine reports 0 because its Order struct is exactly
    // 32 bytes with no room for it, and its FIFO order is instead established by
    // the intrusive list structure plus differential testing against the reference.
    Seq     arrival;
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr bool price_in_range(Ticks p) noexcept {
    return p >= kMinTick && p <= kMaxTick;
}

[[nodiscard]] constexpr bool qty_valid(Qty q) noexcept {
    return q > 0 && q <= kMaxOrderQty;
}

// True when `taker_price` at `taker_side` crosses a resting order at `book_price`.
// Crossing is INCLUSIVE of equality (edge case E30).
[[nodiscard]] constexpr bool crosses(Side taker_side, Ticks taker_price,
                                     Ticks book_price) noexcept {
    return taker_side == Side::Buy ? taker_price >= book_price
                                   : taker_price <= book_price;
}

}  // namespace ob
