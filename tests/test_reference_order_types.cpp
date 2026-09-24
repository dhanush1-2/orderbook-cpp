#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace {

using ob::CancelReason;
using ob::EventType;
using ob::OrderType;
using ob::RejectReason;
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

std::size_t count_trades(const std::vector<ob::Event>& ev) {
    std::size_t n = 0;
    for (const ob::Event& x : ev) {
        if (x.type == EventType::Trade) {
            ++n;
        }
    }
    return n;
}

// ---- Market ----------------------------------------------------------------

// E2: a market order into an empty book is a cancel, not an error and not a crash.
TEST(RefOrderTypes, MarketIntoEmptyBookCancelsWithNoLiquidity) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Market, ob::kNoPrice, 100));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[1].type, EventType::Cancelled);
    EXPECT_EQ(ev[1].cancel, CancelReason::NoLiquidity);
    EXPECT_EQ(ev[1].qty, 100u);
    EXPECT_EQ(e.live_order_count(), 0u);
}

TEST(RefOrderTypes, MarketIgnoresPriceAndSweepsEveryLevel) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 10));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 20000, 10));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 30000, 10));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Market, ob::kNoPrice, 30));
    EXPECT_EQ(count_trades(ev), 3u);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
}

TEST(RefOrderTypes, MarketNeverRestsAndReportsIocRemainderAfterAPartialFill) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 10));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Market, ob::kNoPrice, 50));
    ASSERT_EQ(ev.back().type, EventType::Cancelled);
    EXPECT_EQ(ev.back().cancel, CancelReason::IocRemainder);
    EXPECT_EQ(ev.back().qty, 40u);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice) << "a market order must never rest";
}

// ---- IOC -------------------------------------------------------------------

// E37
TEST(RefOrderTypes, IocFillsWhatItCanAndCancelsTheRest) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 30));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Ioc, 10000, 100));
    EXPECT_EQ(count_trades(ev), 1u);
    EXPECT_EQ(ev.back().type, EventType::Cancelled);
    EXPECT_EQ(ev.back().cancel, CancelReason::IocRemainder);
    EXPECT_EQ(ev.back().qty, 70u);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
}

TEST(RefOrderTypes, IocRespectsItsLimitPrice) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10010, 100));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Ioc, 10000, 100));
    EXPECT_EQ(count_trades(ev), 0u);
    EXPECT_EQ(ev.back().cancel, CancelReason::NoLiquidity);
    EXPECT_EQ(e.best_ask(), 10010) << "the resting ask must be untouched";
}

// ---- FOK -------------------------------------------------------------------

// E3
TEST(RefOrderTypes, FokIntoEmptyBookIsUnfillable) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Fok, 10000, 100));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[1].type, EventType::Cancelled);
    EXPECT_EQ(ev[1].cancel, CancelReason::Unfillable);
}

// E35
TEST(RefOrderTypes, FokFillsWhenExactlyFillable) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 60));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10010, 40));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Fok, 10010, 100));
    EXPECT_EQ(count_trades(ev), 2u);
    EXPECT_EQ(ev.back().type, EventType::Filled);
    EXPECT_EQ(ev.back().order_id, 9u);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
}

// E36: the highest-risk case. One unit short means ZERO mutation.
TEST(RefOrderTypes, FokOneUnitShortMutatesNothing) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 60));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10010, 39));  // 99 available

    const ob::Ticks ask_before = e.best_ask();
    const std::size_t live_before = e.live_order_count();

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Fok, 10010, 100));
    EXPECT_EQ(count_trades(ev), 0u) << "a partial fill here would be the worst bug possible";
    EXPECT_EQ(ev.back().cancel, CancelReason::Unfillable);
    EXPECT_EQ(e.best_ask(), ask_before);
    EXPECT_EQ(e.live_order_count(), live_before);

    // And the liquidity is still fully there afterwards.
    const auto after = run_one(e, ob::make_new(10, Side::Buy, OrderType::Limit, 10010, 99));
    EXPECT_EQ(count_trades(after), 2u);
}

TEST(RefOrderTypes, FokIgnoresLiquidityBeyondItsLimitPrice) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 50));
    // 20000 is inside the ladder but outside the Fok's limit price. Using a price
    // beyond kMaxTick here would be REJECTED outright, and the test would then
    // pass for the wrong reason.
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 20000, 50));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Fok, 10000, 100));
    EXPECT_EQ(count_trades(ev), 0u);
    EXPECT_EQ(ev.back().cancel, CancelReason::Unfillable);
}

// ---- PostOnly --------------------------------------------------------------

// E33: a would-cross PostOnly is Rejected, so Rejected is its ONLY event.
TEST(RefOrderTypes, PostOnlyThatWouldCrossIsRejectedWithNoAccepted) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::PostOnly, 10000, 100));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::WouldCross);
    EXPECT_EQ(e.best_ask(), 10000);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
}

// E34
TEST(RefOrderTypes, PostOnlyThatWouldNotCrossRestsNormally) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10010, 100));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::PostOnly, 10000, 100));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(e.best_bid(), 10000);
}

// A rejected PostOnly still retires nothing: its id stays reusable, because the
// command never got an Accepted.
TEST(RefOrderTypes, RejectedPostOnlyDoesNotRetireItsId) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    ASSERT_EQ(run_one(e, ob::make_new(9, Side::Buy, OrderType::PostOnly, 10000, 100))[0].reject,
              RejectReason::WouldCross);

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Limit, 9000, 100));
    EXPECT_EQ(ev[0].type, EventType::Accepted);
}

}  // namespace
