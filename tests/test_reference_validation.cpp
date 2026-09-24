#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace {

using ob::EventType;
using ob::OrderType;
using ob::RejectReason;
using ob::Side;

// Submits one command and returns the events it produced.
std::vector<ob::Event> run_one(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<256> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

TEST(RefValidation, SatisfiesTheEngineConcept) {
    static_assert(ob::Engine<ob::ReferenceEngine>);
    SUCCEED();
}

TEST(RefValidation, ValidLimitOrderIsAccepted) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[0].order_id, 1u);
    EXPECT_EQ(ev[0].seq, 0u);
}

// E10
TEST(RefValidation, ZeroQuantityIsRejected) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 0));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::InvalidQuantity);
}

// E11
TEST(RefValidation, OversizeQuantityIsRejected) {
    ob::ReferenceEngine e;
    const auto ev =
        run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, ob::kMaxOrderQty + 1));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].reject, RejectReason::InvalidQuantity);
}

// E12, E13
TEST(RefValidation, PriceOutsideLadderIsRejected) {
    ob::ReferenceEngine e;
    for (const ob::Ticks bad : {ob::Ticks{0}, ob::Ticks{-1}, ob::kMinTick - 1,
                                ob::kMaxTick + 1, std::numeric_limits<ob::Ticks>::max()}) {
        const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, bad, 100));
        ASSERT_EQ(ev.size(), 1u) << "price " << bad;
        EXPECT_EQ(ev[0].reject, RejectReason::PriceOutOfRange) << "price " << bad;
    }
}

TEST(RefValidation, BothLadderBoundsAreAccepted) {
    ob::ReferenceEngine e;
    EXPECT_EQ(run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, ob::kMinTick, 1))[0].type,
              EventType::Accepted);
    EXPECT_EQ(run_one(e, ob::make_new(2, Side::Sell, OrderType::Limit, ob::kMaxTick, 1))[0].type,
              EventType::Accepted);
}

// E15: Market ignores the price field entirely; it is not validated.
TEST(RefValidation, MarketOrderPriceIsIgnoredNotValidated) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Market,
                                            std::numeric_limits<ob::Ticks>::max(), 100));
    ASSERT_GE(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
}

// E16, E17: IDs are retired permanently, so reuse is rejected whether the original
// is still live or long gone.
TEST(RefValidation, DuplicateOrderIdIsRejected) {
    ob::ReferenceEngine e;
    ASSERT_EQ(run_one(e, ob::make_new(7, Side::Buy, OrderType::Limit, 10000, 100))[0].type,
              EventType::Accepted);
    const auto ev = run_one(e, ob::make_new(7, Side::Sell, OrderType::Limit, 20000, 5));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].reject, RejectReason::DuplicateOrderId);
}

// E48: strictly increasing ids. An id at or below the high-water mark is a
// duplicate even if it was never used.
TEST(RefValidation, IdBelowHighWaterIsRejectedEvenIfNeverUsed) {
    ob::ReferenceEngine e;
    ASSERT_EQ(run_one(e, ob::make_new(100, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
              EventType::Accepted);
    const auto ev = run_one(e, ob::make_new(50, Side::Buy, OrderType::Limit, 10000, 10));
    EXPECT_EQ(ev[0].reject, RejectReason::DuplicateOrderId);
}

// E49: id 0 falls out of the same rule, because the mark starts at 0.
TEST(RefValidation, OrderIdZeroIsRejected) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(0, Side::Buy, OrderType::Limit, 10000, 10));
    EXPECT_EQ(ev[0].reject, RejectReason::DuplicateOrderId);
}

// Only an Accepted advances the mark, so a rejected id stays usable.
TEST(RefValidation, RejectedCommandDoesNotAdvanceTheHighWaterMark) {
    ob::ReferenceEngine e;
    ASSERT_EQ(run_one(e, ob::make_new(10, Side::Buy, OrderType::Limit, -1, 10))[0].reject,
              RejectReason::PriceOutOfRange);
    EXPECT_EQ(run_one(e, ob::make_new(10, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
              EventType::Accepted);
}

// Validation order: a command that is bad in two ways reports the FIRST failure in
// the fixed order (quantity, price, duplicate, capacity, would-cross).
TEST(RefValidation, QuantityIsCheckedBeforePrice) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, -5, 0));
    EXPECT_EQ(ev[0].reject, RejectReason::InvalidQuantity);
}

TEST(RefValidation, PriceIsCheckedBeforeDuplicateId) {
    ob::ReferenceEngine e;
    ASSERT_EQ(run_one(e, ob::make_new(7, Side::Buy, OrderType::Limit, 10000, 100))[0].type,
              EventType::Accepted);
    const auto ev = run_one(e, ob::make_new(7, Side::Buy, OrderType::Limit, -5, 100));
    EXPECT_EQ(ev[0].reject, RejectReason::PriceOutOfRange);
}

// E18: cancel of an ID that never existed.
TEST(RefValidation, CancelOfUnknownIdIsRejected) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_cancel(999));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::UnknownOrderId);
}

TEST(RefValidation, EveryCommandProducesAtLeastOneEvent) {
    ob::ReferenceEngine e;
    for (const ob::Command c : {ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100),
                                ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 0),
                                ob::make_cancel(12345),
                                ob::make_new(2, Side::Sell, OrderType::Market, 0, 50)}) {
        EXPECT_GE(run_one(e, c).size(), 1u);
    }
}

TEST(RefValidation, SequenceNumbersAreMonotonicAndGapFree) {
    ob::ReferenceEngine e;
    ob::Seq expected = 0;
    for (ob::OrderId id = 1; id <= 10; ++id) {
        for (const ob::Event& ev :
             run_one(e, ob::make_new(id, Side::Buy, OrderType::Limit, 10000, 10))) {
            EXPECT_EQ(ev.seq, expected++);
        }
    }
}

TEST(RefValidation, EmptyBookReportsNoPriceOnBothSides) {
    const ob::ReferenceEngine e;
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
}

TEST(RefValidation, RejectedCommandLeavesTheBookUnchanged) {
    ob::ReferenceEngine e;
    const std::size_t before = e.live_order_count();
    run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 0));
    EXPECT_EQ(e.live_order_count(), before);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
}

}  // namespace
