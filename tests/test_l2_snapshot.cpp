#include <gtest/gtest.h>

#include <ob/fast_engine.hpp>
#include <ob/l2_snapshot.hpp>
#include <ob/reference_engine.hpp>
#include <ob/seqlock.hpp>
#include <type_traits>

namespace {

using ob::OrderType;
using ob::Side;

template <class E>
void feed(E& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

TEST(L2Snapshot, IsSeqlockCompatible) {
    static_assert(std::is_trivially_copyable_v<ob::L2Snapshot>);
    static_assert(sizeof(ob::L2Snapshot) % sizeof(std::uint64_t) == 0);
    SUCCEED();
}

TEST(L2Snapshot, EmptyBookProducesAnEmptySnapshot) {
    const ob::FastEngine e;
    ob::L2Snapshot       s{};
    e.snapshot_l2(s);
    EXPECT_EQ(s.bid_levels, 0u);
    EXPECT_EQ(s.ask_levels, 0u);
    EXPECT_EQ(s.best_bid(), ob::kNoPrice);
    EXPECT_EQ(s.best_ask(), ob::kNoPrice);
    EXPECT_EQ(s.spread(), ob::kNoPrice) << "no spread without two sides";
}

TEST(L2Snapshot, AggregatesQuantityAndOrderCountPerLevel) {
    ob::FastEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 10));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 25));
    feed(e, ob::make_new(3, Side::Buy, OrderType::Limit, 9990, 5));

    ob::L2Snapshot s{};
    e.snapshot_l2(s);
    ASSERT_EQ(s.bid_levels, 2u);
    EXPECT_EQ(s.bids[0].price, 10000);
    EXPECT_EQ(s.bids[0].qty, 35u);
    EXPECT_EQ(s.bids[0].orders, 2u);
    EXPECT_EQ(s.bids[1].price, 9990);
    EXPECT_EQ(s.bids[1].qty, 5u);
    EXPECT_EQ(s.total_bid_qty(), 40u);
}

TEST(L2Snapshot, LevelsAreOrderedBestFirstOnBothSides) {
    ob::FastEngine e;
    // Ids must strictly increase (spec E48), so they are issued from a counter
    // rather than derived from the price. Deriving them from a DESCENDING price is
    // exactly the mistake this comment exists to prevent: every order after the
    // first would be rejected as a duplicate and the test would assert on a
    // one-level book.
    ob::OrderId id = 1;
    for (ob::Ticks px = 9990; px <= 9999; ++px) {
        feed(e, ob::make_new(id++, Side::Buy, OrderType::Limit, px, 1));
    }
    for (ob::Ticks px = 10010; px >= 10001; --px) {
        feed(e, ob::make_new(id++, Side::Sell, OrderType::Limit, px, 1));
    }
    ob::L2Snapshot s{};
    e.snapshot_l2(s);
    ASSERT_GT(s.bid_levels, 1u);
    ASSERT_GT(s.ask_levels, 1u);
    for (std::uint32_t i = 1; i < s.bid_levels; ++i) {
        EXPECT_LT(s.bids[i].price, s.bids[i - 1].price) << "bids must descend";
    }
    for (std::uint32_t i = 1; i < s.ask_levels; ++i) {
        EXPECT_GT(s.asks[i].price, s.asks[i - 1].price) << "asks must ascend";
    }
    EXPECT_LT(s.best_bid(), s.best_ask());
    EXPECT_EQ(s.spread(), s.best_ask() - s.best_bid());
}

TEST(L2Snapshot, TruncatesAtKDepthWithoutOverrunning) {
    ob::FastEngine e;
    for (std::uint32_t i = 0; i < ob::L2Snapshot::kDepth * 2; ++i) {
        const ob::Ticks px = 10000 - static_cast<ob::Ticks>(i);
        feed(e, ob::make_new(static_cast<ob::OrderId>(i + 1), Side::Buy, OrderType::Limit, px, 1));
    }
    ob::L2Snapshot s{};
    e.snapshot_l2(s);
    EXPECT_EQ(s.bid_levels, ob::L2Snapshot::kDepth);
    EXPECT_EQ(s.bids[0].price, 10000) << "truncation must keep the BEST levels";
}

// Both engines must agree, for the same reason their event streams must.
TEST(L2Snapshot, BothEnginesProduceIdenticalSnapshots) {
    ob::FastEngine      fast;
    ob::ReferenceEngine ref;
    for (ob::OrderId id = 1; id <= 200; ++id) {
        const ob::Command c = ob::make_new(id, id % 2 ? Side::Buy : Side::Sell, OrderType::Limit,
                                           id % 2 ? 10000 - static_cast<ob::Ticks>(id % 20)
                                                  : 10010 + static_cast<ob::Ticks>(id % 20),
                                           static_cast<ob::Qty>(id));
        feed(fast, c);
        feed(ref, c);
    }
    ob::L2Snapshot a{}, b{};
    fast.snapshot_l2(a);
    ref.snapshot_l2(b);
    ASSERT_EQ(a.bid_levels, b.bid_levels);
    ASSERT_EQ(a.ask_levels, b.ask_levels);
    for (std::uint32_t i = 0; i < a.bid_levels; ++i) {
        EXPECT_EQ(a.bids[i].price, b.bids[i].price) << "bid level " << i;
        EXPECT_EQ(a.bids[i].qty, b.bids[i].qty) << "bid level " << i;
        EXPECT_EQ(a.bids[i].orders, b.bids[i].orders) << "bid level " << i;
    }
    for (std::uint32_t i = 0; i < a.ask_levels; ++i) {
        EXPECT_EQ(a.asks[i].price, b.asks[i].price) << "ask level " << i;
        EXPECT_EQ(a.asks[i].qty, b.asks[i].qty) << "ask level " << i;
        EXPECT_EQ(a.asks[i].orders, b.asks[i].orders) << "ask level " << i;
    }
}

TEST(L2Snapshot, RoundTripsThroughASeqlock) {
    ob::FastEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 42));
    ob::L2Snapshot s{};
    e.snapshot_l2(s);

    ob::Seqlock<ob::L2Snapshot> lock;
    lock.store(s);
    ob::L2Snapshot got{};
    ASSERT_TRUE(lock.try_load(got));
    ASSERT_EQ(got.bid_levels, 1u);
    EXPECT_EQ(got.bids[0].price, 10000);
    EXPECT_EQ(got.bids[0].qty, 42u);
}

}  // namespace
