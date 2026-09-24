#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace {

using ob::EventType;
using ob::OrderType;
using ob::RejectReason;
using ob::Side;

std::vector<ob::Event> run_one(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<256> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

void feed(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<256> buf;
    e.submit(c, buf);
}

// E1
TEST(RefResting, LimitIntoEmptyBookRestsAndBecomesBest) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(e.best_bid(), 10000);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
    EXPECT_EQ(e.live_order_count(), 1u);
}

TEST(RefResting, BetterBidReplacesBest) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10010, 100));
    feed(e, ob::make_new(3, Side::Buy, OrderType::Limit, 9990, 100));
    EXPECT_EQ(e.best_bid(), 10010);
}

TEST(RefResting, BetterAskIsTheLowerPrice) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10100, 100));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10050, 100));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 10200, 100));
    EXPECT_EQ(e.best_ask(), 10050);
}

// E7, E8
TEST(RefResting, OrdersRestAtBothLadderExtremes) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, ob::kMinTick, 1));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, ob::kMaxTick, 1));
    EXPECT_EQ(e.best_bid(), ob::kMinTick);
    EXPECT_EQ(e.best_ask(), ob::kMaxTick);
}

// E31: one tick apart does not cross. The spread is minimal but uncrossed.
TEST(RefResting, OneTickApartDoesNotCross) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10001, 100));
    const auto ev = run_one(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_EQ(ev.size(), 1u) << "should rest, not trade";
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(e.best_bid(), 10000);
    EXPECT_EQ(e.best_ask(), 10001);
    EXPECT_LT(e.best_bid(), e.best_ask());
}

TEST(RefResting, ManyOrdersAtOnePriceAllRest) {
    ob::ReferenceEngine e;
    for (ob::OrderId id = 1; id <= 50; ++id) {
        feed(e, ob::make_new(id, Side::Buy, OrderType::Limit, 10000, 10));
    }
    EXPECT_EQ(e.best_bid(), 10000);
    EXPECT_EQ(e.live_order_count(), 50u);
}

// E39: capacity is a rejection, never a crash, and the engine stays usable.
TEST(RefResting, CapacityExhaustionRejectsAndTheEngineStaysUsable) {
    ob::ReferenceEngine e(3);
    for (ob::OrderId id = 1; id <= 3; ++id) {
        ASSERT_EQ(run_one(e, ob::make_new(id, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
                  EventType::Accepted);
    }
    const auto rejected = run_one(e, ob::make_new(4, Side::Buy, OrderType::Limit, 10000, 10));
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected[0].reject, RejectReason::EngineCapacity);
    EXPECT_EQ(e.live_order_count(), 3u);
    EXPECT_EQ(e.best_bid(), 10000);
}

TEST(RefResting, ResetEmptiesEverythingIncludingRetiredIds) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    e.reset();
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
    EXPECT_EQ(e.live_order_count(), 0u);
    // After reset, id 1 is reusable and the sequence restarts at 0.
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[0].seq, 0u);
}

}  // namespace
