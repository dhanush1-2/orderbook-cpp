// include/ob/invariants.hpp
#pragma once

// Whole-book invariant checker. Generic over any engine that can enumerate its
// resting orders, so the FastEngine in Phase 2 gets the same scrutiny for free.
//
// Returns a result rather than asserting, so the fuzzer can report WHICH invariant
// broke instead of just dying.

#include <cstddef>
#include <ob/engine_concept.hpp>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace ob {

template <class E>
concept Inspectable = Engine<E> && requires(const E e) {
    { e.live_order_count() } -> std::same_as<std::size_t>;
    e.for_each_resting([](const RestingOrder&) {});
    // Whether RestingOrder::arrival is meaningful. Compile-time, so the FIFO check
    // below simply does not exist for engines that cannot afford to track it.
    requires std::convertible_to<decltype(E::kTracksArrival), bool>;
};

struct InvariantResult {
    bool        ok      = true;
    const char* failure = nullptr;
};

inline constexpr InvariantResult kInvariantsHold{};

template <Inspectable E>
[[nodiscard]] InvariantResult check_invariants(const E& engine) {
    std::vector<RestingOrder> orders;
    engine.for_each_resting([&orders](const RestingOrder& o) { orders.push_back(o); });

    // 1. The reported live count matches what enumeration finds.
    if (orders.size() != engine.live_order_count()) {
        return {false, "live_order_count disagrees with for_each_resting"};
    }

    std::unordered_set<OrderId> ids;
    Ticks                       max_bid = kNoPrice;
    Ticks                       min_ask = kNoPrice;

    // Per-side, per-level FIFO tracking.
    Side  cur_side     = Side::Buy;
    Ticks cur_price    = kNoPrice;
    Seq   last_arrival = 0;
    bool  in_level     = false;

    Ticks prev_bid_px = kNoPrice;
    Ticks prev_ask_px = kNoPrice;

    for (const RestingOrder& o : orders) {
        // 2. No resting order may have zero remaining quantity.
        if (o.remaining == 0) {
            return {false, "resting order has zero remaining quantity"};
        }
        // 3. Every resting price is inside the ladder.
        if (!price_in_range(o.price)) {
            return {false, "resting order price is outside the ladder"};
        }
        // 4. Order ids are unique among live orders.
        if (!ids.insert(o.id).second) {
            return {false, "duplicate order id among resting orders"};
        }

        // 5. Within a price level, arrival sequence strictly increases (FIFO).
        //    Only checkable on engines that track arrival order. For engines that
        //    do not, FIFO is established by their own structural invariants plus
        //    differential testing, not here.
        if (in_level && o.side == cur_side && o.price == cur_price) {
            if constexpr (E::kTracksArrival) {
                if (o.arrival <= last_arrival) {
                    return {false, "FIFO order violated within a price level"};
                }
            }
        } else {
            // 6. Levels are visited best-to-worst within each side.
            if (o.side == Side::Buy) {
                if (prev_bid_px != kNoPrice && o.price >= prev_bid_px) {
                    return {false, "bid levels not enumerated in descending price order"};
                }
                prev_bid_px = o.price;
            } else {
                if (prev_ask_px != kNoPrice && o.price <= prev_ask_px) {
                    return {false, "ask levels not enumerated in ascending price order"};
                }
                prev_ask_px = o.price;
            }
            cur_side  = o.side;
            cur_price = o.price;
            in_level  = true;
        }
        last_arrival = o.arrival;

        if (o.side == Side::Buy) {
            if (max_bid == kNoPrice || o.price > max_bid) {
                max_bid = o.price;
            }
        } else {
            if (min_ask == kNoPrice || o.price < min_ask) {
                min_ask = o.price;
            }
        }
    }

    // 7. best_bid()/best_ask() agree with the resting orders that exist.
    if (engine.best_bid() != max_bid) {
        return {false, "best_bid disagrees with the resting bid orders"};
    }
    if (engine.best_ask() != min_ask) {
        return {false, "best_ask disagrees with the resting ask orders"};
    }

    // 8. The book is never observably crossed.
    if (max_bid != kNoPrice && min_ask != kNoPrice && max_bid >= min_ask) {
        return {false, "book is crossed: best_bid >= best_ask"};
    }

    return kInvariantsHold;
}

}  // namespace ob
