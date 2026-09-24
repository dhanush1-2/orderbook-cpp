// include/ob/events.hpp
#pragma once

#include <ob/types.hpp>

#include <ostream>
#include <type_traits>

namespace ob {

enum class EventType : std::uint8_t { Accepted = 0, Rejected, Trade, Cancelled, Filled };

struct Event {
    Seq          seq      = 0;
    EventType    type     = EventType::Accepted;
    OrderId      order_id = 0;         // for Trade this is the TAKER
    OrderId      maker_id = 0;         // Trade only, otherwise 0
    Ticks        price    = kNoPrice;  // Trade: the MAKER's price
    Qty          qty      = 0;         // Trade: filled qty. Cancelled: qty removed.
    RejectReason reject   = RejectReason::None;
    CancelReason cancel   = CancelReason::None;
};

static_assert(std::is_trivially_copyable_v<Event>);

[[nodiscard]] constexpr bool operator==(const Event& a, const Event& b) noexcept {
    return a.seq == b.seq && a.type == b.type && a.order_id == b.order_id &&
           a.maker_id == b.maker_id && a.price == b.price && a.qty == b.qty &&
           a.reject == b.reject && a.cancel == b.cancel;
}

[[nodiscard]] constexpr const char* to_string(EventType t) noexcept {
    switch (t) {
        case EventType::Accepted:  return "Accepted";
        case EventType::Rejected:  return "Rejected";
        case EventType::Trade:     return "Trade";
        case EventType::Cancelled: return "Cancelled";
        case EventType::Filled:    return "Filled";
    }
    return "?";
}

[[nodiscard]] constexpr const char* to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:             return "None";
        case RejectReason::InvalidQuantity:  return "InvalidQuantity";
        case RejectReason::PriceOutOfRange:  return "PriceOutOfRange";
        case RejectReason::DuplicateOrderId: return "DuplicateOrderId";
        case RejectReason::UnknownOrderId:   return "UnknownOrderId";
        case RejectReason::WouldCross:       return "WouldCross";
        case RejectReason::EngineCapacity:   return "EngineCapacity";
    }
    return "?";
}

[[nodiscard]] constexpr const char* to_string(CancelReason r) noexcept {
    switch (r) {
        case CancelReason::None:          return "None";
        case CancelReason::UserRequested: return "UserRequested";
        case CancelReason::NoLiquidity:   return "NoLiquidity";
        case CancelReason::Unfillable:    return "Unfillable";
        case CancelReason::IocRemainder:  return "IocRemainder";
    }
    return "?";
}

// Found by ADL. GoogleTest uses it to print Event in failure messages, which is
// what makes a differential-test failure readable instead of a hex dump.
inline void PrintTo(const Event& e, std::ostream* os) {
    *os << "Event{seq=" << e.seq << " " << to_string(e.type) << " id=" << e.order_id;
    if (e.type == EventType::Trade) {
        *os << " maker=" << e.maker_id;
    }
    if (e.price != kNoPrice) {
        *os << " px=" << e.price;
    }
    if (e.qty != 0) {
        *os << " qty=" << e.qty;
    }
    if (e.reject != RejectReason::None) {
        *os << " reject=" << to_string(e.reject);
    }
    if (e.cancel != CancelReason::None) {
        *os << " cancel=" << to_string(e.cancel);
    }
    *os << "}";
}

inline std::ostream& operator<<(std::ostream& os, const Event& e) {
    PrintTo(e, &os);
    return os;
}

}  // namespace ob
