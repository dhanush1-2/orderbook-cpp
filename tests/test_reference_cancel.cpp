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

TEST(RefCancel, CancelRemovesTheOrderAndReportsTheQuantityRemoved) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));

    const auto ev = run_one(e, ob::make_cancel(1));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Cancelled);
    EXPECT_EQ(ev[0].order_id, 1u);
    EXPECT_EQ(ev[0].cancel, CancelReason::UserRequested);
    EXPECT_EQ(ev[0].qty, 100u);

    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
    EXPECT_EQ(e.live_order_count(), 0u);
}

// E21: only the remaining quantity is removed; prior fills stand.
TEST(RefCancel, CancelOfAPartiallyFilledOrderRemovesOnlyTheRemainder) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 40));  // fills 40 of id 1

    const auto ev = run_one(e, ob::make_cancel(1));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].qty, 60u);
}

// E19
TEST(RefCancel, CancelOfAFullyFilledOrderIsUnknown) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));

    const auto ev = run_one(e, ob::make_cancel(1));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::UnknownOrderId);
}

// E20: double cancel is an error, deliberately not idempotent.
TEST(RefCancel, DoubleCancelIsRejected) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_EQ(run_one(e, ob::make_cancel(1))[0].type, EventType::Cancelled);

    const auto ev = run_one(e, ob::make_cancel(1));
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::UnknownOrderId);
}

// E17: a cancelled id is retired and cannot be reused.
TEST(RefCancel, CancelledIdCannotBeReused) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    feed(e, ob::make_cancel(1));

    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    EXPECT_EQ(ev[0].reject, RejectReason::DuplicateOrderId);
}

// E22, E23, E25: cancel from head, tail and middle of a level, and confirm the
// surviving FIFO order by draining the level afterwards.
TEST(RefCancel, CancelFromHeadMiddleAndTailPreservesRemainingFifoOrder) {
    for (const ob::OrderId victim : {ob::OrderId{1}, ob::OrderId{2}, ob::OrderId{3}}) {
        ob::ReferenceEngine e;
        for (ob::OrderId id = 1; id <= 3; ++id) {
            feed(e, ob::make_new(id, Side::Sell, OrderType::Limit, 10000, 10));
        }
        ASSERT_EQ(run_one(e, ob::make_cancel(victim))[0].type, EventType::Cancelled)
            << "victim " << victim;

        const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10000, 20));
        std::vector<ob::OrderId> makers;
        for (const ob::Event& x : ev) {
            if (x.type == EventType::Trade) {
                makers.push_back(x.maker_id);
            }
        }
        std::vector<ob::OrderId> expected;
        for (ob::OrderId id = 1; id <= 3; ++id) {
            if (id != victim) {
                expected.push_back(id);
            }
        }
        EXPECT_EQ(makers, expected) << "victim " << victim;
    }
}

// E24: cancelling the only order at a level removes the level and moves the best.
TEST(RefCancel, CancellingTheOnlyOrderAtTheBestLevelMovesTheBest) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10010, 10));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 10));
    ASSERT_EQ(e.best_bid(), 10010);

    feed(e, ob::make_cancel(1));
    EXPECT_EQ(e.best_bid(), 10000);
}

// E39 continued: freeing an order makes capacity available again.
TEST(RefCancel, CancelFreesCapacity) {
    ob::ReferenceEngine e(2);
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 10));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 10));
    ASSERT_EQ(run_one(e, ob::make_new(3, Side::Buy, OrderType::Limit, 10000, 10))[0].reject,
              RejectReason::EngineCapacity);

    feed(e, ob::make_cancel(1));
    EXPECT_EQ(run_one(e, ob::make_new(4, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
              EventType::Accepted);
}

TEST(RefCancel, RejectedCancelLeavesTheBookBitForBitUnchanged) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    const ob::Ticks bid = e.best_bid();
    const std::size_t n = e.live_order_count();

    run_one(e, ob::make_cancel(4242));
    EXPECT_EQ(e.best_bid(), bid);
    EXPECT_EQ(e.live_order_count(), n);
}

}  // namespace
