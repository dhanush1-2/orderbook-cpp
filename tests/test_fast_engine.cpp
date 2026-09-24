#include "model/scenario_gen.hpp"

#include <ob/fast_engine.hpp>
#include <ob/invariants.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace {

using ob::OrderType;
using ob::Side;

std::vector<ob::Event> run_one(ob::FastEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

void feed(ob::FastEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

TEST(FastEngine, SatisfiesTheEngineAndInspectableConcepts) {
    static_assert(ob::Engine<ob::FastEngine>);
    static_assert(ob::Inspectable<ob::FastEngine>);
    static_assert(ob::FastEngine::kTracksArrival == false);
    SUCCEED();
}

TEST(FastEngine, TheSpecWorkedExample) {
    ob::FastEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10050, 300));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10050, 100));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 10100, 200));

    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10050, 350));
    ASSERT_EQ(ev.size(), 5u);
    EXPECT_EQ(ev[1].maker_id, 1u);
    EXPECT_EQ(ev[1].price, 10050);
    EXPECT_EQ(ev[1].qty, 300u);
    EXPECT_EQ(ev[3].maker_id, 2u);
    EXPECT_EQ(ev[3].qty, 50u);
    EXPECT_EQ(e.best_ask(), 10050);
}

// The generic invariants, which FastEngine must satisfy exactly as the reference
// does. kTracksArrival == false means the FIFO-by-arrival check is compiled out;
// FIFO is covered by the internal structural check and by differential testing.
TEST(FastEngine, GenericInvariantsHoldAcrossRandomStreams) {
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    for (std::uint64_t seed = 1; seed <= 15; ++seed) {
        ob::FastEngine e(ob::FastEngine::Config{4096});
        const auto stream = obtest::generate_stream(seed, 3000, obtest::GenConfig{});
        for (std::size_t i = 0; i < stream.size(); ++i) {
            buf.clear();
            e.submit(stream[i], buf);
            ASSERT_GE(buf.size(), 1u) << "seed " << seed << " op " << i;
            const auto r = ob::check_invariants(e);
            ASSERT_TRUE(r.ok) << "seed " << seed << " op " << i << ": " << r.failure;
        }
    }
}

// The structural checks only FastEngine can make: intrusive list integrity, the
// bitmap agreeing with the levels, the index agreeing with the pool, and the free
// list being acyclic and disjoint from the live set.
TEST(FastEngine, InternalInvariantsHoldAcrossRandomStreams) {
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    for (std::uint64_t seed = 1; seed <= 15; ++seed) {
        ob::FastEngine e(ob::FastEngine::Config{4096});
        const auto stream = obtest::generate_stream(seed, 3000, obtest::GenConfig{});
        for (std::size_t i = 0; i < stream.size(); ++i) {
            buf.clear();
            e.submit(stream[i], buf);
            const auto r = e.check_internal_invariants();
            ASSERT_TRUE(r.ok) << "seed " << seed << " op " << i << ": " << r.failure;
        }
    }
}

// Spec E39 on the real structures: exhaustion rejects, and a cancel frees a slot.
TEST(FastEngine, CapacityExhaustionRejectsThenRecovers) {
    ob::FastEngine e(ob::FastEngine::Config{4});
    for (ob::OrderId id = 1; id <= 4; ++id) {
        ASSERT_EQ(run_one(e, ob::make_new(id, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
                  ob::EventType::Accepted);
    }
    const auto full = run_one(e, ob::make_new(5, Side::Buy, OrderType::Limit, 10000, 10));
    ASSERT_EQ(full.size(), 1u);
    EXPECT_EQ(full[0].reject, ob::RejectReason::EngineCapacity);

    feed(e, ob::make_cancel(1));
    EXPECT_EQ(run_one(e, ob::make_new(6, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
              ob::EventType::Accepted);
}

// A Market/Ioc/Fok taker must never consume a pool slot: it never rests.
TEST(FastEngine, NonRestingOrderTypesDoNotConsumePoolSlots) {
    ob::FastEngine e(ob::FastEngine::Config{2});
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 10));
    ASSERT_EQ(e.live_order_count(), 1u);

    feed(e, ob::make_new(2, Side::Buy, OrderType::Market, ob::kNoPrice, 10));
    EXPECT_EQ(e.live_order_count(), 0u);

    feed(e, ob::make_new(3, Side::Buy, OrderType::Ioc, 10000, 10));
    EXPECT_EQ(e.live_order_count(), 0u);
}

TEST(FastEngine, ResetMakesItIndistinguishableFromAFreshEngine) {
    const auto stream = obtest::generate_stream(9, 2000, obtest::GenConfig{});

    ob::FastEngine reused(ob::FastEngine::Config{4096});
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());
    for (const ob::Command& c : stream) {
        buf.clear();
        reused.submit(c, buf);
    }
    reused.reset();

    std::vector<ob::Event> after;
    for (const ob::Command& c : stream) {
        buf.clear();
        reused.submit(c, buf);
        after.insert(after.end(), buf.begin(), buf.end());
    }

    ob::FastEngine fresh(ob::FastEngine::Config{4096});
    std::vector<ob::Event> want;
    for (const ob::Command& c : stream) {
        buf.clear();
        fresh.submit(c, buf);
        want.insert(want.end(), buf.begin(), buf.end());
    }

    ASSERT_EQ(after.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        ASSERT_EQ(after[i], want[i]) << "divergence at event " << i;
    }
}

// The FOK pre-scan sums level totals rather than walking orders, so it takes a
// different code path from the reference. Same answer, cheaper. Spec E36.
TEST(FastEngine, FokPreScanOneUnitShortMutatesNothing) {
    ob::FastEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 60));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10010, 39));

    const auto before_ask = e.best_ask();
    const auto before_live = e.live_order_count();

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Fok, 10010, 100));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[1].cancel, ob::CancelReason::Unfillable);
    EXPECT_EQ(e.best_ask(), before_ask);
    EXPECT_EQ(e.live_order_count(), before_live);

    const auto r = e.check_internal_invariants();
    EXPECT_TRUE(r.ok) << r.failure;
}

// The internal checker must be able to fail, like the generic one.
TEST(FastEngine, InternalCheckerPassesOnACleanBook) {
    ob::FastEngine e;
    for (ob::OrderId id = 1; id <= 100; ++id) {
        feed(e, ob::make_new(id, id % 2 ? Side::Buy : Side::Sell, OrderType::Limit,
                             id % 2 ? 9000 : 11000, 10));
    }
    const auto r = e.check_internal_invariants();
    EXPECT_TRUE(r.ok) << r.failure;
    EXPECT_EQ(e.live_order_count(), 100u);
}

}  // namespace
