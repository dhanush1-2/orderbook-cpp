// include/ob/fast_engine.hpp
#pragma once

// The matching engine.
//
// After construction: no allocation, no exceptions, no syscalls, no virtual
// dispatch. Every branch of submit() either completes or returns a typed reason,
// and a rejected command leaves the book bit-for-bit unchanged.
//
// The logic here mirrors ReferenceEngine deliberately, line for line where it can.
// Any divergence is a bug, and differential testing is what finds it: generated
// streams compared event-for-event, not inspection.

#include <algorithm>
#include <cassert>
#include <ob/engine_concept.hpp>
#include <ob/id_index.hpp>
#include <ob/invariants.hpp>
#include <ob/price_ladder.hpp>

namespace ob {

class FastEngine {
public:
    // Order is exactly 32 bytes with no room for an arrival sequence, and a
    // parallel array would put a cold-line store on the hot path for the sake of
    // a debug-only check. FIFO order is established instead by
    // check_internal_invariants() and by differential testing.
    static constexpr bool kTracksArrival = false;

    static constexpr std::size_t kDefaultOrderCapacity = 1'000'000;

    struct Config {
        std::size_t order_capacity = kDefaultOrderCapacity;
    };

    // Two constructors rather than `Config cfg = {}`: a default argument of `{}`
    // needs Config's default member initializer inside the enclosing class
    // definition, which is not a complete-class context here. Delegating keeps one
    // source of truth for the default.
    explicit FastEngine(Config cfg) : pool_(cfg.order_capacity), index_(cfg.order_capacity) {}
    FastEngine() : FastEngine(Config{kDefaultOrderCapacity}) {}

    void submit(const Command& c, EventBuffer& out) noexcept {
        if (c.type == CommandType::Cancel) {
            submit_cancel(c, out);
        } else {
            submit_new(c, out);
        }
    }

    [[nodiscard]] Ticks best_bid() const noexcept { return bids_.best(); }
    [[nodiscard]] Ticks best_ask() const noexcept { return asks_.best(); }

    // Only resting orders hold a pool slot: takers are matched without allocating.
    [[nodiscard]] std::size_t live_order_count() const noexcept { return pool_.size(); }

    template <class Fn>
    void for_each_resting(Fn&& fn) const {
        bids_.for_each(pool_, [&fn](Ticks px, const Order& o) {
            fn(RestingOrder{Side::Buy, px, o.id, o.remaining, 0});
        });
        asks_.for_each(pool_, [&fn](Ticks px, const Order& o) {
            fn(RestingOrder{Side::Sell, px, o.id, o.remaining, 0});
        });
    }

    void reset() noexcept {
        bids_.reset();
        asks_.reset();
        pool_.reset();
        index_.reset();
        high_water_ = 0;
        seq_        = 0;
    }

    // Structural checks the generic checker cannot make, because they are about
    // this engine's representation rather than the book's semantics.
    [[nodiscard]] InvariantResult check_internal_invariants() const {
        if (const auto r = check_side(bids_); !r.ok) {
            return r;
        }
        if (const auto r = check_side(asks_); !r.ok) {
            return r;
        }
        if (index_.size() != pool_.size()) {
            return {false, "id index size disagrees with the pool's live count"};
        }
        const std::size_t freelen = pool_.free_list_length();
        if (freelen == static_cast<std::size_t>(-1)) {
            return {false, "free list has a cycle, which means a double free"};
        }
        if (freelen + pool_.size() != pool_.capacity()) {
            return {false, "free list plus live count does not cover the pool"};
        }
        return kInvariantsHold;
    }

private:
    [[nodiscard]] Seq next_seq() noexcept { return seq_++; }

    Event base(EventType t, OrderId id) noexcept {
        Event e{};
        e.seq      = next_seq();
        e.type     = t;
        e.order_id = id;
        return e;
    }

    void emit_rejected(EventBuffer& out, OrderId id, RejectReason r) noexcept {
        Event e  = base(EventType::Rejected, id);
        e.reject = r;
        out.push(e);
    }

    Event trade_event(OrderId taker, OrderId maker, Ticks px, Qty qty) noexcept {
        Event e    = base(EventType::Trade, taker);
        e.maker_id = maker;
        e.price    = px;
        e.qty      = qty;
        return e;
    }

    [[nodiscard]] bool would_cross(const Command& c) const noexcept {
        const Ticks opp = (c.side == Side::Buy) ? asks_.best() : bids_.best();
        return opp != kNoPrice && crosses(c.side, c.price, opp);
    }

    // Identical order and identical rules to ReferenceEngine::validate_new. Any
    // difference here is a differential-test failure waiting to happen.
    [[nodiscard]] RejectReason validate_new(const Command& c) const noexcept {
        if (!qty_valid(c.qty)) {
            return RejectReason::InvalidQuantity;
        }
        if (c.order_type != OrderType::Market && !price_in_range(c.price)) {
            return RejectReason::PriceOutOfRange;
        }
        if (c.id <= high_water_) {
            return RejectReason::DuplicateOrderId;
        }
        const bool can_rest =
            c.order_type == OrderType::Limit || c.order_type == OrderType::PostOnly;
        // The pool always binds before the index: index capacity is
        // next_pow2(2 * order_capacity) with a ceiling of half that, which is
        // always >= order_capacity. The index term is kept anyway, because it costs
        // one comparison and removes the need to trust that argument forever.
        if (can_rest && (pool_.full() || index_.full())) {
            return RejectReason::EngineCapacity;
        }
        if (c.order_type == OrderType::PostOnly && would_cross(c)) {
            return RejectReason::WouldCross;
        }
        return RejectReason::None;
    }

    // Non-mutating. Sums LEVEL totals rather than individual orders, so this is
    // O(levels) where the reference engine is O(orders). Same answer; the
    // differential test is what establishes that.
    template <Side OppSide>
    [[nodiscard]] QtySum fillable_qty(const PriceLadder<OppSide>& book,
                                      const Command&              c) const noexcept {
        QtySum total = 0;
        book.for_each_level([&](Ticks px, const PriceLevel& lv) {
            if (c.order_type != OrderType::Market && !crosses(c.side, c.price, px)) {
                return false;  // stop: prices only get worse from here
            }
            total += lv.total;
            return total < c.qty;  // stop early once it is provably enough
        });
        return total;
    }

    [[nodiscard]] bool fok_is_fillable(const Command& c) const noexcept {
        const QtySum available =
            (c.side == Side::Buy) ? fillable_qty(asks_, c) : fillable_qty(bids_, c);
        return available >= c.qty;
    }

    template <Side OppSide>
    void match_into(PriceLadder<OppSide>& book, const Command& c, Qty& remaining,
                    EventBuffer& out) noexcept {
        while (remaining > 0) {
            const Ticks level_px = book.best();
            if (level_px == kNoPrice) {
                break;
            }
            // A Market order ignores price; everything else must cross.
            if (c.order_type != OrderType::Market && !crosses(c.side, c.price, level_px)) {
                break;
            }

            while (remaining > 0) {
                const Slot s = book.head(level_px);
                if (s == kInvalidSlot) {
                    break;  // level drained; the outer loop re-reads best()
                }
                Order&    maker = pool_.at(s);
                const Qty fill  = std::min(remaining, maker.remaining);
                remaining -= fill;
                out.push(trade_event(c.id, maker.id, level_px, fill));

                if (fill == maker.remaining) {
                    const OrderId maker_id = maker.id;
                    // unlink BEFORE the slot is freed and before remaining is
                    // touched: it subtracts the order's current remaining.
                    const Qty removed = book.unlink(level_px, s, pool_);
                    assert(removed == fill);
                    static_cast<void>(removed);
                    out.push(base(EventType::Filled, maker_id));
                    const bool erased = index_.erase(maker_id);
                    assert(erased && "a resting order was missing from the index");
                    static_cast<void>(erased);
                    pool_.free(s);
                } else {
                    maker.remaining -= fill;
                    book.reduce(level_px, fill);
                }
            }
        }
    }

    void submit_new(const Command& c, EventBuffer& out) noexcept {
        const RejectReason r = validate_new(c);
        if (r != RejectReason::None) {
            emit_rejected(out, c.id, r);
            return;
        }
        out.push(base(EventType::Accepted, c.id));
        high_water_ = c.id;  // only an Accepted advances the mark

        if (c.order_type == OrderType::Fok && !fok_is_fillable(c)) {
            Event e  = base(EventType::Cancelled, c.id);
            e.cancel = CancelReason::Unfillable;
            e.qty    = c.qty;
            out.push(e);
            return;
        }

        Qty remaining = c.qty;

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

        switch (c.order_type) {
            case OrderType::Limit:
            case OrderType::PostOnly: {
                const Slot s = pool_.alloc();
                assert(s != kInvalidSlot && "capacity was checked during validation");
                Order& o    = pool_.at(s);
                o.id        = c.id;
                o.price     = c.price;
                o.remaining = remaining;
                o.side      = c.side;
                o.flags     = 0;

                const bool inserted = index_.insert(c.id, s);
                assert(inserted && "index rejected an id validation had cleared");
                static_cast<void>(inserted);

                if (c.side == Side::Buy) {
                    bids_.push_back(c.price, s, pool_);
                } else {
                    asks_.push_back(c.price, s, pool_);
                }
                return;  // rests; Accepted already conveyed that
            }

            case OrderType::Market:
            case OrderType::Ioc: {
                Event e = base(EventType::Cancelled, c.id);
                e.cancel =
                    (remaining == c.qty) ? CancelReason::NoLiquidity : CancelReason::IocRemainder;
                e.qty = remaining;
                out.push(e);
                return;
            }

            case OrderType::Fok:
                assert(false &&
                       "Fok reached the remainder branch: the pre-scan "
                       "disagreed with the match loop");
                return;
        }
    }

    void submit_cancel(const Command& c, EventBuffer& out) noexcept {
        const Slot s = index_.find(c.id);
        if (s == kInvalidSlot) {
            // Never existed (E18), already filled (E19) or already cancelled (E20).
            emit_rejected(out, c.id, RejectReason::UnknownOrderId);
            return;
        }
        const Order& o    = pool_.at(s);
        const Ticks  px   = o.price;
        const Side   side = o.side;

        const Qty removed =
            (side == Side::Buy) ? bids_.unlink(px, s, pool_) : asks_.unlink(px, s, pool_);
        const bool erased = index_.erase(c.id);
        assert(erased);
        static_cast<void>(erased);
        pool_.free(s);

        Event e  = base(EventType::Cancelled, c.id);
        e.cancel = CancelReason::UserRequested;
        e.qty    = removed;
        e.price  = px;
        out.push(e);
    }

    template <Side S>
    [[nodiscard]] InvariantResult check_side(const PriceLadder<S>& book) const {
        InvariantResult result = kInvariantsHold;
        book.for_each_level([&](Ticks px, const PriceLevel& lv) {
            if (lv.count == 0) {
                result = {false, "occupancy bitmap set for a level with no orders"};
                return false;
            }
            std::uint32_t walked = 0;
            QtySum        sum    = 0;
            Slot          prev   = kInvalidSlot;
            Slot          cur    = lv.head;
            while (cur != kInvalidSlot) {
                if (walked > lv.count) {
                    result = {false,
                              "level FIFO list is longer than its count, "
                              "which means it has a cycle"};
                    return false;
                }
                const Order& o = pool_.at(cur);
                if (o.id == 0) {
                    result = {false, "a freed slot is still linked into a level"};
                    return false;
                }
                if (o.price != px) {
                    result = {false, "an order is linked at a price it does not hold"};
                    return false;
                }
                if (o.prev != prev) {
                    result = {false, "level FIFO prev link is inconsistent"};
                    return false;
                }
                if (index_.find(o.id) != cur) {
                    result = {false, "id index does not map a resting order to its slot"};
                    return false;
                }
                sum += o.remaining;
                prev = cur;
                cur  = o.next;
                ++walked;
            }
            if (prev != lv.tail) {
                result = {false, "level tail is not the last node in its FIFO list"};
                return false;
            }
            if (walked != lv.count) {
                result = {false, "level count disagrees with its FIFO list length"};
                return false;
            }
            if (sum != lv.total) {
                result = {false, "level total disagrees with the sum of its orders"};
                return false;
            }
            return true;
        });
        return result;
    }

    OrderPool               pool_;
    IdIndex                 index_;
    PriceLadder<Side::Buy>  bids_;
    PriceLadder<Side::Sell> asks_;
    OrderId                 high_water_ = 0;
    Seq                     seq_        = 0;
};

static_assert(Engine<FastEngine>);
static_assert(Inspectable<FastEngine>);

}  // namespace ob
