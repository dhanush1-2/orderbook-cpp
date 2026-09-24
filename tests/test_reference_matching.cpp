#include <gtest/gtest.h>

#include <ob/reference_engine.hpp>
#include <vector>

namespace {

using ob::EventType;
using ob::OrderType;
using ob::Side;

std::vector<ob::Event> run_one(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

void feed(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

// Builds the book from the spec's worked example:
//   asks: 100.50 -> [300 (id 2)][100 (id 3)],  101.00 -> [200 (id 4)]
void build_example_book(ob::ReferenceEngine& e) {
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10050, 300));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 10050, 100));
    feed(e, ob::make_new(4, Side::Sell, OrderType::Limit, 10100, 200));
}

// The spec's worked example, verbatim.
TEST(RefMatching, WorkedExampleFromTheSpec) {
    ob::ReferenceEngine e;
    build_example_book(e);

    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10050, 350));

    ASSERT_EQ(ev.size(), 5u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[0].order_id, 99u);

    EXPECT_EQ(ev[1].type, EventType::Trade);
    EXPECT_EQ(ev[1].order_id, 99u);
    EXPECT_EQ(ev[1].maker_id, 2u);  // id 2 arrived first, so it fills first
    EXPECT_EQ(ev[1].price, 10050);  // the MAKER's price
    EXPECT_EQ(ev[1].qty, 300u);

    EXPECT_EQ(ev[2].type, EventType::Filled);
    EXPECT_EQ(ev[2].order_id, 2u);  // maker fully consumed

    EXPECT_EQ(ev[3].type, EventType::Trade);
    EXPECT_EQ(ev[3].maker_id, 3u);
    EXPECT_EQ(ev[3].qty, 50u);

    EXPECT_EQ(ev[4].type, EventType::Filled);
    EXPECT_EQ(ev[4].order_id, 99u);  // taker fully filled

    // 101.00 was never touched; id 3 keeps 50 and its time priority.
    EXPECT_EQ(e.best_ask(), 10050);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
}

// E32: the same order, larger, sweeps the level and rests the remainder.
TEST(RefMatching, LargerOrderSweepsThenRestsRemainder) {
    ob::ReferenceEngine e;
    build_example_book(e);

    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10050, 1000));

    // Accepted, Trade(2)+Filled(2), Trade(3)+Filled(3) = 5 events; no Filled(99).
    ASSERT_EQ(ev.size(), 5u);
    EXPECT_EQ(ev[4].type, EventType::Filled);
    EXPECT_EQ(ev[4].order_id, 3u);

    EXPECT_EQ(e.best_bid(), 10050);  // 600 remainder rested
    EXPECT_EQ(e.best_ask(), 10100);  // 101.00 untouched
    EXPECT_LT(e.best_bid(), e.best_ask());
}

// E30: crossing is inclusive of equality.
TEST(RefMatching, EqualPriceCrosses) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    const auto ev = run_one(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_GE(ev.size(), 2u);
    EXPECT_EQ(ev[1].type, EventType::Trade);
    EXPECT_EQ(ev[1].price, 10000);
}

// E26
TEST(RefMatching, ExactQuantityMatchLeavesNothing) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    run_one(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
    EXPECT_EQ(e.live_order_count(), 0u);
}

// E27: a partially filled resting order keeps its place at the front of the queue.
TEST(RefMatching, PartiallyFilledMakerKeepsTimePriority) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));   // first
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10000, 100));   // second
    run_one(e, ob::make_new(3, Side::Buy, OrderType::Limit, 10000, 30));  // takes 30 of id 1

    // id 1 has 70 left and must still fill before id 2.
    const auto ev = run_one(e, ob::make_new(4, Side::Buy, OrderType::Limit, 10000, 70));
    ASSERT_GE(ev.size(), 2u);
    EXPECT_EQ(ev[1].maker_id, 1u);
}

// E28
TEST(RefMatching, FifoOrderWithinAPriceLevel) {
    ob::ReferenceEngine e;
    for (ob::OrderId id = 1; id <= 4; ++id) {
        feed(e, ob::make_new(id, Side::Sell, OrderType::Limit, 10000, 10));
    }
    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10000, 40));

    std::vector<ob::OrderId> makers;
    for (const ob::Event& x : ev) {
        if (x.type == EventType::Trade) {
            makers.push_back(x.maker_id);
        }
    }
    EXPECT_EQ(makers, (std::vector<ob::OrderId>{1, 2, 3, 4}));
}

// E29: levels are consumed best-price-first, and each trade prints at its own
// maker's price, not at one blended price.
TEST(RefMatching, SweepsLevelsInPriceOrderAtEachMakersPrice) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10020, 10));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10000, 10));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 10010, 10));

    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10020, 30));

    std::vector<ob::Ticks> prices;
    for (const ob::Event& x : ev) {
        if (x.type == EventType::Trade) {
            prices.push_back(x.price);
        }
    }
    EXPECT_EQ(prices, (std::vector<ob::Ticks>{10000, 10010, 10020}));
}

// The book must never be observably crossed.
TEST(RefMatching, BookIsNeverCrossedAfterAnyOperation) {
    ob::ReferenceEngine e;
    ob::OrderId         id = 1;
    for (int i = 0; i < 200; ++i) {
        const Side      s  = (i % 2 == 0) ? Side::Buy : Side::Sell;
        const ob::Ticks px = 10000 + static_cast<ob::Ticks>(i % 7) - 3;
        feed(e, ob::make_new(id++, s, OrderType::Limit, px, 10));
        if (e.best_bid() != ob::kNoPrice && e.best_ask() != ob::kNoPrice) {
            ASSERT_LT(e.best_bid(), e.best_ask()) << "crossed at i=" << i;
        }
    }
}

// E4, E5: emptying a side must produce the sentinel, not a stale or zero price.
TEST(RefMatching, EmptyingASideYieldsTheNoPriceSentinel) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    ASSERT_EQ(e.best_ask(), 10000);
    run_one(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
}

}  // namespace
