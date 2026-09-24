// include/ob/command.hpp
#pragma once

#include <ob/types.hpp>

#include <type_traits>

namespace ob {

enum class CommandType : std::uint8_t { New = 0, Cancel };

// A command is a value: trivially copyable, no ownership, no allocation.
struct Command {
    CommandType type       = CommandType::New;
    Side        side       = Side::Buy;
    OrderType   order_type = OrderType::Limit;
    OrderId     id         = 0;
    Ticks       price      = kNoPrice;
    Qty         qty        = 0;
};

static_assert(std::is_trivially_copyable_v<Command>);

[[nodiscard]] constexpr Command make_new(OrderId id, Side side, OrderType type,
                                        Ticks price, Qty qty) noexcept {
    Command c{};
    c.type = CommandType::New;
    c.side = side;
    c.order_type = type;
    c.id = id;
    c.price = price;
    c.qty = qty;
    return c;
}

[[nodiscard]] constexpr Command make_cancel(OrderId id) noexcept {
    Command c{};
    c.type = CommandType::Cancel;
    c.id = id;
    c.price = kNoPrice;
    c.qty = 0;
    return c;
}

}  // namespace ob
