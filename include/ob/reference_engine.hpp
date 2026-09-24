// include/ob/reference_engine.hpp
#pragma once

// The correctness oracle. This engine is DELIBERATELY SIMPLE and is NEVER
// OPTIMIZED. std::map and std::list are used on purpose: the value of this file
// is that a reader can confirm it is right by reading it. Every optimization in
// Phase 2 is validated by differential testing against this.
//
// Do not "improve" the performance of anything in this file.

#include <ob/engine_concept.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <list>
#include <map>

namespace ob {

class ReferenceEngine {
public:
    // This engine can afford an arrival sequence per order, so the invariant
    // checker's FIFO check is enabled for it. FastEngine sets this to false.
    static constexpr bool kTracksArrival = true;

    explicit ReferenceEngine(std::size_t capacity = 1'000'000) : capacity_(capacity) {}

    void submit(const Command& c, EventBuffer& out) {
        if (c.type == CommandType::Cancel) {
            submit_cancel(c, out);
        } else {
            submit_new(c, out);
        }
    }

    [[nodiscard]] Ticks best_bid() const noexcept {
        return bids_.empty() ? kNoPrice : bids_.begin()->first;
    }
    [[nodiscard]] Ticks best_ask() const noexcept {
        return asks_.empty() ? kNoPrice : asks_.begin()->first;
    }
    [[nodiscard]] std::size_t live_order_count() const noexcept { return live_.size(); }

    void reset() {
        bids_.clear();
        asks_.clear();
        live_.clear();
        high_water_ = 0;
        arrival_counter_ = 0;
        seq_ = 0;
    }

private:
    struct RefOrder {
        OrderId id;
        Qty     remaining;
        Seq     arrival;  // strictly increasing; makes FIFO order checkable
    };
    using Level = std::list<RefOrder>;

    // Comparators chosen so begin() is always the best price on that side.
    using BidBook = std::map<Ticks, Level, std::greater<Ticks>>;
    using AskBook = std::map<Ticks, Level, std::less<Ticks>>;

    struct Loc {
        Side  side;
        Ticks price;
    };

    [[nodiscard]] Seq next_seq() noexcept { return seq_++; }

    Event base(EventType t, OrderId id) {
        Event e{};
        e.seq = next_seq();
        e.type = t;
        e.order_id = id;
        return e;
    }

    void emit_rejected(EventBuffer& out, OrderId id, RejectReason r) {
        Event e = base(EventType::Rejected, id);
        e.reject = r;
        out.push(e);
    }

    // Validation. Order is FIXED so reason codes are deterministic:
    // quantity, price range, duplicate id, capacity, post-only would-cross.
    // Returns None when the command is acceptable.
    [[nodiscard]] RejectReason validate_new(const Command& c) const {
        if (!qty_valid(c.qty)) {
            return RejectReason::InvalidQuantity;
        }
        // A Market order's price field is ignored, not validated (E15).
        if (c.order_type != OrderType::Market && !price_in_range(c.price)) {
            return RejectReason::PriceOutOfRange;
        }
        // Order IDs must be strictly increasing (spec E48). This single comparison
        // replaces an unbounded set of retired ids, which FastEngine could not hold
        // without allocating. ID 0 is rejected automatically because the mark
        // starts at 0 (spec E49).
        if (c.id <= high_water_) {
            return RejectReason::DuplicateOrderId;
        }
        // Capacity is checked only for types that can rest, conservatively, before
        // it is known whether the order would have fully filled. Both engines must
        // apply this identical rule or differential testing diverges on a full book.
        const bool can_rest =
            c.order_type == OrderType::Limit || c.order_type == OrderType::PostOnly;
        if (can_rest && live_.size() >= capacity_) {
            return RejectReason::EngineCapacity;
        }
        if (c.order_type == OrderType::PostOnly && would_cross(c)) {
            return RejectReason::WouldCross;
        }
        return RejectReason::None;
    }

    [[nodiscard]] bool would_cross(const Command& c) const {
        const Ticks opp = (c.side == Side::Buy) ? best_ask() : best_bid();
        return opp != kNoPrice && crosses(c.side, c.price, opp);
    }

    Event trade_event(OrderId taker, OrderId maker, Ticks px, Qty qty) {
        Event e = base(EventType::Trade, taker);
        e.maker_id = maker;
        e.price = px;
        e.qty = qty;
        return e;
    }

    // Matches `c` against `book` (the opposite side), consuming `remaining`.
    // Emits Trade per fill, and Filled for each maker it fully consumes.
    template <class BookMap>
    void match_into(BookMap& book, const Command& c, Qty& remaining, EventBuffer& out) {
        while (remaining > 0 && !book.empty()) {
            const auto lit = book.begin();  // best price on this side
            const Ticks level_px = lit->first;

            // A Market order ignores price entirely; everything else must cross.
            if (c.order_type != OrderType::Market && !crosses(c.side, c.price, level_px)) {
                break;
            }

            Level& level = lit->second;
            while (remaining > 0 && !level.empty()) {
                RefOrder& maker = level.front();  // FIFO: oldest fills first
                const Qty fill = std::min(remaining, maker.remaining);

                remaining -= fill;
                maker.remaining -= fill;
                out.push(trade_event(c.id, maker.id, level_px, fill));

                if (maker.remaining == 0) {
                    out.push(base(EventType::Filled, maker.id));
                    live_.erase(maker.id);
                    level.pop_front();
                }
            }
            if (level.empty()) {
                book.erase(lit);
            }
        }
    }

    void rest(const Command& c, Qty remaining) {
        const Seq arrival = arrival_counter_++;
        if (c.side == Side::Buy) {
            bids_[c.price].push_back(RefOrder{c.id, remaining, arrival});
        } else {
            asks_[c.price].push_back(RefOrder{c.id, remaining, arrival});
        }
        live_[c.id] = Loc{c.side, c.price};
    }

    void submit_new(const Command& c, EventBuffer& out) {
        const RejectReason r = validate_new(c);
        if (r != RejectReason::None) {
            emit_rejected(out, c.id, r);
            return;
        }
        out.push(base(EventType::Accepted, c.id));
        high_water_ = c.id;  // only an Accepted advances the mark

        Qty remaining = c.qty;

        // PostOnly never matches: validation already rejected it if it would cross.
        if (c.order_type != OrderType::PostOnly) {
            if (c.side == Side::Buy) {
                match_into(asks_, c, remaining, out);
            } else {
                match_into(bids_, c, remaining, out);
            }
        }

        if (remaining == 0) {
            out.push(base(EventType::Filled, c.id));
            return;
        }

        // Market/Ioc/Fok remainders are handled in Task 9. For now only the
        // resting types have a defined remainder behavior.
        const bool can_rest =
            c.order_type == OrderType::Limit || c.order_type == OrderType::PostOnly;
        if (can_rest) {
            rest(c, remaining);
        }
    }

    void submit_cancel(const Command& c, EventBuffer& out) {
        const auto it = live_.find(c.id);
        if (it == live_.end()) {
            emit_rejected(out, c.id, RejectReason::UnknownOrderId);
            return;
        }
        // Removal arrives in Task 8.
        emit_rejected(out, c.id, RejectReason::UnknownOrderId);
    }

    BidBook bids_;
    AskBook asks_;
    std::map<OrderId, Loc> live_;  // live orders only
    OrderId high_water_ = 0;       // highest accepted id; ids must strictly increase
    std::size_t capacity_;
    Seq arrival_counter_ = 0;
    Seq seq_ = 0;
};

static_assert(Engine<ReferenceEngine>);

}  // namespace ob
