// include/ob/price_ladder.hpp
#pragma once

// Flat array of price levels indexed by tick, plus a three-level occupancy bitmap.
//
// Memory: 65,536 levels x 24 B = 1.5 MB per side. The allocation does not fit in
// L2, but the TOUCHED set does: activity clusters within a few dozen ticks of the
// touch, so the hot footprint is a few kilobytes of contiguous memory that the
// prefetcher handles well. That is the actual argument for this layout, and it is
// the reason a tree keyed by price loses despite allocating far less.
//
// Templated on Side so best() becomes "highest occupied bit" or "lowest occupied
// bit" at compile time, with no runtime branch.

#include <cassert>
#include <ob/level_bitmap.hpp>
#include <ob/order_pool.hpp>
#include <vector>

namespace ob {

struct PriceLevel {
    Slot          head  = kInvalidSlot;  // 4  FIFO front: the oldest order
    Slot          tail  = kInvalidSlot;  // 4  FIFO back: the newest order
    QtySum        total = 0;             // 8  sum of remaining qty at this level
    std::uint32_t count = 0;             // 4  order count, for L2 output
    std::uint32_t pad   = 0;             // 4
};
static_assert(sizeof(PriceLevel) == 24, "PriceLevel must stay 24 bytes");

template <Side S>
class PriceLadder {
public:
    PriceLadder() : levels_(kLadderSize) {}

    void push_back(Ticks px, Slot s, OrderPool& pool) noexcept {
        assert(price_in_range(px) && "caller must range-check the price FIRST");
        PriceLevel& lv = levels_[idx(px)];
        Order&      o  = pool.at(s);

        o.next = kInvalidSlot;
        o.prev = lv.tail;
        if (lv.tail != kInvalidSlot) {
            pool.at(lv.tail).next = s;
        } else {
            lv.head = s;
        }
        lv.tail = s;

        lv.total += o.remaining;
        ++lv.count;
        ++orders_;
        occupied_.set(idx32(px));
    }

    // Removes `s` from its level and returns the quantity removed.
    //
    // MUST be called while pool.at(s).remaining still holds the quantity being
    // removed. Zeroing remaining first would subtract nothing and leave the level
    // total permanently too high, and nothing else would notice until an invariant
    // check. The return value exists so the caller cannot quietly ignore it.
    [[nodiscard]] Qty unlink(Ticks px, Slot s, OrderPool& pool) noexcept {
        assert(price_in_range(px));
        PriceLevel& lv      = levels_[idx(px)];
        Order&      o       = pool.at(s);
        const Qty   removed = o.remaining;

        if (o.prev != kInvalidSlot) {
            pool.at(o.prev).next = o.next;
        } else {
            lv.head = o.next;
        }
        if (o.next != kInvalidSlot) {
            pool.at(o.next).prev = o.prev;
        } else {
            lv.tail = o.prev;
        }
        o.next = kInvalidSlot;
        o.prev = kInvalidSlot;

        assert(lv.total >= removed && "level total underflow");
        lv.total -= removed;
        assert(lv.count > 0);
        --lv.count;
        --orders_;
        if (lv.count == 0) {
            assert(lv.head == kInvalidSlot && lv.tail == kInvalidSlot);
            assert(lv.total == 0);
            occupied_.clear(idx32(px));
        }
        return removed;
    }

    // Partial fill: the order stays where it is and keeps its time priority.
    void reduce(Ticks px, Qty by) noexcept {
        assert(price_in_range(px));
        PriceLevel& lv = levels_[idx(px)];
        assert(lv.total >= by && "level total underflow");
        lv.total -= by;
    }

    [[nodiscard]] Slot head(Ticks px) const noexcept {
        assert(price_in_range(px));
        return levels_[idx(px)].head;
    }

    [[nodiscard]] const PriceLevel& level(Ticks px) const noexcept {
        assert(price_in_range(px));
        return levels_[idx(px)];
    }

    // kNoPrice when this side is empty. Two or three word loads, no branch on Side.
    [[nodiscard]] Ticks best() const noexcept {
        std::uint32_t i;
        if constexpr (S == Side::Buy) {
            i = occupied_.prev_set_at_or_below(LevelBitmap::kBits - 1);
        } else {
            i = occupied_.next_set_at_or_above(0);
        }
        return i == LevelBitmap::kNotFound ? kNoPrice : px_of(i);
    }

    [[nodiscard]] bool        empty() const noexcept { return occupied_.empty(); }
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_; }

    // Visits (price, level) best-to-worst, stopping early when `fn` returns false.
    // Used by the FOK pre-scan (which needs level totals, not individual orders)
    // and by the L2 publisher in Phase 3.
    template <class Fn>
    void for_each_level(Fn&& fn) const {
        std::uint32_t i = first_index();
        while (i != LevelBitmap::kNotFound) {
            if (!fn(px_of(i), levels_[i])) {
                return;
            }
            i = next_index(i);
        }
    }

    // Visits every resting order: levels best-to-worst, FIFO within each level.
    // Used by the invariant checker and the L2 publisher.
    template <class Fn>
    void for_each(const OrderPool& pool, Fn&& fn) const {
        std::uint32_t i = first_index();
        while (i != LevelBitmap::kNotFound) {
            const Ticks px = px_of(i);
            for (Slot cur = levels_[i].head; cur != kInvalidSlot; cur = pool.at(cur).next) {
                fn(px, pool.at(cur));
            }
            i = next_index(i);
        }
    }

    void reset() noexcept {
        for (PriceLevel& lv : levels_) {
            lv = PriceLevel{};
        }
        occupied_.reset();
        orders_ = 0;
    }

private:
    [[nodiscard]] std::uint32_t first_index() const noexcept {
        if constexpr (S == Side::Buy) {
            return occupied_.prev_set_at_or_below(LevelBitmap::kBits - 1);
        } else {
            return occupied_.next_set_at_or_above(0);
        }
    }
    [[nodiscard]] std::uint32_t next_index(std::uint32_t i) const noexcept {
        if constexpr (S == Side::Buy) {
            return i == 0 ? LevelBitmap::kNotFound : occupied_.prev_set_at_or_below(i - 1);
        } else {
            return i + 1 >= LevelBitmap::kBits ? LevelBitmap::kNotFound
                                               : occupied_.next_set_at_or_above(i + 1);
        }
    }

    static std::size_t   idx(Ticks px) noexcept { return static_cast<std::size_t>(px - kMinTick); }
    static std::uint32_t idx32(Ticks px) noexcept {
        return static_cast<std::uint32_t>(px - kMinTick);
    }
    static Ticks px_of(std::uint32_t i) noexcept { return static_cast<Ticks>(i) + kMinTick; }

    std::vector<PriceLevel> levels_;
    LevelBitmap             occupied_;
    std::size_t             orders_ = 0;
};

}  // namespace ob
