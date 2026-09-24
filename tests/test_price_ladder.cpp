#include <ob/price_ladder.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <map>
#include <random>
#include <vector>

namespace {

using ob::OrderPool;
using ob::PriceLadder;
using ob::Side;
using ob::Slot;
using ob::Ticks;

// Allocates a slot and fills in the fields the ladder reads.
Slot make(OrderPool& pool, ob::OrderId id, Ticks px, ob::Qty qty, Side side) {
    const Slot s = pool.alloc();
    ob::Order& o = pool.at(s);
    o.id = id;
    o.price = px;
    o.remaining = qty;
    o.side = side;
    return s;
}

TEST(PriceLevel, IsExactlyTwentyFourBytes) {
    static_assert(sizeof(ob::PriceLevel) == 24, "PriceLevel must stay 24 bytes");
    SUCCEED();
}

TEST(PriceLadder, StartsEmpty) {
    const PriceLadder<Side::Buy> l;
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.best(), ob::kNoPrice);
    EXPECT_EQ(l.order_count(), 0u);
}

TEST(PriceLadder, PushBackEstablishesFifoOrder) {
    OrderPool pool(16);
    PriceLadder<Side::Sell> l;
    const Slot a = make(pool, 1, 10000, 10, Side::Sell);
    const Slot b = make(pool, 2, 10000, 20, Side::Sell);
    const Slot c = make(pool, 3, 10000, 30, Side::Sell);
    l.push_back(10000, a, pool);
    l.push_back(10000, b, pool);
    l.push_back(10000, c, pool);

    EXPECT_EQ(l.head(10000), a);
    EXPECT_EQ(pool.at(a).next, b);
    EXPECT_EQ(pool.at(b).next, c);
    EXPECT_EQ(pool.at(c).next, ob::kInvalidSlot);
    EXPECT_EQ(pool.at(a).prev, ob::kInvalidSlot);
    EXPECT_EQ(pool.at(c).prev, b);

    EXPECT_EQ(l.level(10000).total, 60u);
    EXPECT_EQ(l.level(10000).count, 3u);
    EXPECT_EQ(l.order_count(), 3u);
}

TEST(PriceLadder, BestIsTheHighestPriceForBidsAndLowestForAsks) {
    OrderPool pool(16);
    PriceLadder<Side::Buy> bids;
    PriceLadder<Side::Sell> asks;

    bids.push_back(10000, make(pool, 1, 10000, 10, Side::Buy), pool);
    bids.push_back(10010, make(pool, 2, 10010, 10, Side::Buy), pool);
    bids.push_back(9990, make(pool, 3, 9990, 10, Side::Buy), pool);
    EXPECT_EQ(bids.best(), 10010);

    asks.push_back(10100, make(pool, 4, 10100, 10, Side::Sell), pool);
    asks.push_back(10050, make(pool, 5, 10050, 10, Side::Sell), pool);
    asks.push_back(10200, make(pool, 6, 10200, 10, Side::Sell), pool);
    EXPECT_EQ(asks.best(), 10050);
}

TEST(PriceLadder, BothLadderExtremesWork) {
    OrderPool pool(8);
    PriceLadder<Side::Buy> bids;
    bids.push_back(ob::kMinTick, make(pool, 1, ob::kMinTick, 1, Side::Buy), pool);
    EXPECT_EQ(bids.best(), ob::kMinTick);
    bids.push_back(ob::kMaxTick, make(pool, 2, ob::kMaxTick, 1, Side::Buy), pool);
    EXPECT_EQ(bids.best(), ob::kMaxTick);
}

TEST(PriceLadder, UnlinkFromHeadMiddleAndTailKeepsTheRestIntact) {
    for (int victim = 0; victim < 3; ++victim) {
        OrderPool pool(16);
        PriceLadder<Side::Sell> l;
        Slot s[3];
        for (int i = 0; i < 3; ++i) {
            s[i] = make(pool, static_cast<ob::OrderId>(i + 1), 10000, 10, Side::Sell);
            l.push_back(10000, s[i], pool);
        }

        const ob::Qty removed = l.unlink(10000, s[victim], pool);
        EXPECT_EQ(removed, 10u) << "victim " << victim;
        EXPECT_EQ(l.level(10000).total, 20u) << "victim " << victim;
        EXPECT_EQ(l.level(10000).count, 2u) << "victim " << victim;

        std::vector<ob::OrderId> seen;
        for (Slot cur = l.head(10000); cur != ob::kInvalidSlot; cur = pool.at(cur).next) {
            seen.push_back(pool.at(cur).id);
        }
        std::vector<ob::OrderId> want;
        for (int i = 0; i < 3; ++i) {
            if (i != victim) {
                want.push_back(static_cast<ob::OrderId>(i + 1));
            }
        }
        EXPECT_EQ(seen, want) << "victim " << victim;
    }
}

TEST(PriceLadder, EmptyingALevelClearsItAndMovesBest) {
    OrderPool pool(16);
    PriceLadder<Side::Buy> l;
    const Slot hi = make(pool, 1, 10010, 10, Side::Buy);
    const Slot lo = make(pool, 2, 10000, 10, Side::Buy);
    l.push_back(10010, hi, pool);
    l.push_back(10000, lo, pool);
    ASSERT_EQ(l.best(), 10010);

    EXPECT_EQ(l.unlink(10010, hi, pool), 10u);
    EXPECT_EQ(l.best(), 10000);
    EXPECT_EQ(l.level(10010).count, 0u);
    EXPECT_EQ(l.level(10010).total, 0u);
    EXPECT_EQ(l.level(10010).head, ob::kInvalidSlot);
    EXPECT_EQ(l.level(10010).tail, ob::kInvalidSlot);

    EXPECT_EQ(l.unlink(10000, lo, pool), 10u);
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.best(), ob::kNoPrice);
}

TEST(PriceLadder, ReduceAdjustsTheLevelTotalForAPartialFill) {
    OrderPool pool(8);
    PriceLadder<Side::Sell> l;
    const Slot s = make(pool, 1, 10000, 100, Side::Sell);
    l.push_back(10000, s, pool);

    pool.at(s).remaining -= 30;
    l.reduce(10000, 30);
    EXPECT_EQ(l.level(10000).total, 70u);
    EXPECT_EQ(l.level(10000).count, 1u) << "a partial fill must not change the count";
    EXPECT_EQ(l.head(10000), s) << "a partially filled order keeps its place";
}

// The API hazard made explicit: unlink subtracts the order's CURRENT remaining,
// so it must be called before remaining is zeroed.
TEST(PriceLadder, UnlinkMustBeCalledBeforeZeroingRemaining) {
    OrderPool pool(8);
    PriceLadder<Side::Sell> l;
    const Slot s = make(pool, 1, 10000, 100, Side::Sell);
    l.push_back(10000, s, pool);
    ASSERT_EQ(l.level(10000).total, 100u);

    EXPECT_EQ(l.unlink(10000, s, pool), 100u);
    EXPECT_EQ(l.level(10000).total, 0u);
}

TEST(PriceLadder, ForEachVisitsLevelsBestToWorstAndFifoWithin) {
    OrderPool pool(32);
    PriceLadder<Side::Buy> l;
    // Deliberately inserted out of price order.
    l.push_back(10000, make(pool, 1, 10000, 10, Side::Buy), pool);
    l.push_back(10020, make(pool, 2, 10020, 10, Side::Buy), pool);
    l.push_back(10000, make(pool, 3, 10000, 10, Side::Buy), pool);
    l.push_back(10010, make(pool, 4, 10010, 10, Side::Buy), pool);

    std::vector<ob::OrderId> ids;
    std::vector<Ticks> prices;
    l.for_each(pool, [&](Ticks px, const ob::Order& o) {
        prices.push_back(px);
        ids.push_back(o.id);
    });
    // Bids: highest price first. Within 10000, id 1 arrived before id 3.
    EXPECT_EQ(prices, (std::vector<Ticks>{10020, 10010, 10000, 10000}));
    EXPECT_EQ(ids, (std::vector<ob::OrderId>{2, 4, 1, 3}));
}

TEST(PriceLadder, ForEachLevelCanStopEarly) {
    OrderPool pool(32);
    PriceLadder<Side::Sell> l;
    for (Ticks px = 10000; px < 10005; ++px) {
        l.push_back(px, make(pool, static_cast<ob::OrderId>(px), px, 10, Side::Sell), pool);
    }
    std::vector<Ticks> visited;
    l.for_each_level([&](Ticks px, const ob::PriceLevel&) {
        visited.push_back(px);
        return visited.size() < 3;  // stop after three
    });
    EXPECT_EQ(visited, (std::vector<Ticks>{10000, 10001, 10002}));
}

TEST(PriceLadder, ResetEmptiesEverything) {
    OrderPool pool(8);
    PriceLadder<Side::Buy> l;
    l.push_back(10000, make(pool, 1, 10000, 10, Side::Buy), pool);
    l.reset();
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.best(), ob::kNoPrice);
    EXPECT_EQ(l.order_count(), 0u);
}

// Property test against a std::map of deques. Intrusive-list bugs need a specific
// arrangement, which randomization finds and hand-written cases do not.
TEST(PriceLadder, MatchesAMapOfDequesUnderRandomPushAndUnlink) {
    for (std::uint64_t seed = 1; seed <= 15; ++seed) {
        OrderPool pool(4096);
        PriceLadder<Side::Sell> l;
        std::map<Ticks, std::deque<ob::OrderId>> model;
        std::mt19937_64 rng(seed);
        std::vector<std::tuple<Ticks, Slot, ob::OrderId>> live;
        ob::OrderId next_id = 1;

        for (int op = 0; op < 3000; ++op) {
            const bool do_push = live.empty() || (rng() % 3 != 0);
            if (do_push && pool.size() < pool.capacity()) {
                const Ticks px = 9950 + static_cast<Ticks>(rng() % 101);
                const ob::OrderId id = next_id++;
                const Slot s = make(pool, id, px, 10, Side::Sell);
                l.push_back(px, s, pool);
                model[px].push_back(id);
                live.emplace_back(px, s, id);
            } else if (!live.empty()) {
                const std::size_t k = static_cast<std::size_t>(rng() % live.size());
                const auto [px, s, id] = live[k];
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
                EXPECT_EQ(l.unlink(px, s, pool), 10u);
                pool.free(s);
                auto& dq = model[px];
                dq.erase(std::find(dq.begin(), dq.end(), id));
                if (dq.empty()) {
                    model.erase(px);
                }
            }

            const Ticks want_best = model.empty() ? ob::kNoPrice : model.begin()->first;
            ASSERT_EQ(l.best(), want_best) << "seed " << seed << " op " << op;
            ASSERT_EQ(l.order_count(), live.size()) << "seed " << seed << " op " << op;
        }

        // Full traversal must reproduce the model exactly, in order.
        std::vector<ob::OrderId> got;
        l.for_each(pool, [&](Ticks, const ob::Order& o) { got.push_back(o.id); });
        std::vector<ob::OrderId> want;
        for (const auto& [px, dq] : model) {
            want.insert(want.end(), dq.begin(), dq.end());
        }
        ASSERT_EQ(got, want) << "seed " << seed;
    }
}

}  // namespace
