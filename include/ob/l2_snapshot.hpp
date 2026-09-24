// include/ob/l2_snapshot.hpp
#pragma once

// Aggregated top-of-book depth, as a venue's level-2 feed publishes it.
//
// Trivially copyable and a whole number of 64-bit words, because Seqlock requires
// both. kDepth is small on purpose: a snapshot is a handful of cache lines, so the
// matching thread can publish one cheaply and a reader can copy one without
// noticeably widening the window in which a tear can happen.

#include <cstdint>
#include <ob/types.hpp>
#include <type_traits>

namespace ob {

struct L2Level {
    Ticks         price  = kNoPrice;  // 4
    std::uint32_t orders = 0;         // 4
    QtySum        qty    = 0;         // 8
};
static_assert(sizeof(L2Level) == 16);
static_assert(std::is_trivially_copyable_v<L2Level>);

struct L2Snapshot {
    static constexpr std::uint32_t kDepth = 15;

    Seq           seq        = 0;  // engine sequence at publication
    std::uint32_t bid_levels = 0;
    std::uint32_t ask_levels = 0;
    L2Level       bids[kDepth]{};  // best first, descending price
    L2Level       asks[kDepth]{};  // best first, ascending price

    [[nodiscard]] Ticks best_bid() const noexcept {
        return bid_levels == 0 ? kNoPrice : bids[0].price;
    }
    [[nodiscard]] Ticks best_ask() const noexcept {
        return ask_levels == 0 ? kNoPrice : asks[0].price;
    }
    // kNoPrice when either side is empty: there is no spread without two sides.
    [[nodiscard]] Ticks spread() const noexcept {
        return (bid_levels == 0 || ask_levels == 0) ? kNoPrice : asks[0].price - bids[0].price;
    }
    [[nodiscard]] QtySum total_bid_qty() const noexcept { return side_total(bids, bid_levels); }
    [[nodiscard]] QtySum total_ask_qty() const noexcept { return side_total(asks, ask_levels); }

private:
    static QtySum side_total(const L2Level* lv, std::uint32_t n) noexcept {
        QtySum t = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            t += lv[i].qty;
        }
        return t;
    }
};

static_assert(std::is_trivially_copyable_v<L2Snapshot>);
static_assert(sizeof(L2Snapshot) % sizeof(std::uint64_t) == 0,
              "Seqlock copies the payload in 64-bit words");

}  // namespace ob
