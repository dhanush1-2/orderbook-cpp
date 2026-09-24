#include "model/scenario_gen.hpp"

#include <ob/invariants.hpp>
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace {

// Random streams with the invariants checked after EVERY operation. This is the
// test that finds what the hand-written cases did not.
TEST(Stress, InvariantsHoldAcrossManySeeds) {
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    for (std::uint64_t seed = 1; seed <= 25; ++seed) {
        ob::ReferenceEngine engine;
        const auto stream = obtest::generate_stream(seed, 4000, obtest::GenConfig{});

        for (std::size_t i = 0; i < stream.size(); ++i) {
            buf.clear();
            engine.submit(stream[i], buf);
            ASSERT_GE(buf.size(), 1u) << "seed " << seed << " op " << i;

            const auto r = ob::check_invariants(engine);
            ASSERT_TRUE(r.ok) << "seed " << seed << " op " << i << ": " << r.failure;
        }
    }
}

// E9: every tick in the ladder occupied. Too large for the edge-case table, so it
// lives here. Bids fill the low half and asks the high half so the book never
// crosses, which also exercises both ladder extremes.
TEST(Stress, EveryTickInARangeCanBeOccupiedIncludingBothExtremes) {
    ob::ReferenceEngine engine(200'000);
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    ob::OrderId id = 1;
    const ob::Ticks mid = ob::kMaxTick / 2;
    for (ob::Ticks px = ob::kMinTick; px <= mid; ++px) {
        buf.clear();
        engine.submit(ob::make_new(id++, ob::Side::Buy, ob::OrderType::Limit, px, 1), buf);
        ASSERT_EQ(buf[0].type, ob::EventType::Accepted) << "px " << px;
    }
    for (ob::Ticks px = mid + 1; px <= ob::kMaxTick; ++px) {
        buf.clear();
        engine.submit(ob::make_new(id++, ob::Side::Sell, ob::OrderType::Limit, px, 1), buf);
        ASSERT_EQ(buf[0].type, ob::EventType::Accepted) << "px " << px;
    }

    EXPECT_EQ(engine.best_bid(), mid);
    EXPECT_EQ(engine.best_ask(), mid + 1);
    EXPECT_EQ(engine.live_order_count(), static_cast<std::size_t>(ob::kMaxTick));

    const auto r = ob::check_invariants(engine);
    EXPECT_TRUE(r.ok) << r.failure;
}

}  // namespace
