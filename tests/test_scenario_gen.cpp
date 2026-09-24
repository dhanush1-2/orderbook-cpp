#include <gtest/gtest.h>

#include <array>
#include <ob/reference_engine.hpp>
#include <vector>

#include "model/scenario_gen.hpp"

namespace {

TEST(Xoshiro, IsDeterministicForAGivenSeed) {
    obtest::Xoshiro256ss a(12345), b(12345);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.next(), b.next());
    }
}

TEST(Xoshiro, DifferentSeedsDiverge) {
    obtest::Xoshiro256ss a(1), b(2);
    bool                 differed = false;
    for (int i = 0; i < 10; ++i) {
        if (a.next() != b.next()) {
            differed = true;
        }
    }
    EXPECT_TRUE(differed);
}

TEST(Xoshiro, BoundedStaysInRange) {
    obtest::Xoshiro256ss r(7);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_LT(r.bounded(10), 10u);
    }
    EXPECT_EQ(r.bounded(1), 0u);
}

TEST(Generator, IsReproducibleFromItsSeed) {
    const obtest::GenConfig cfg;
    const auto              a = obtest::generate_stream(99, 500, cfg);
    const auto              b = obtest::generate_stream(99, 500, cfg);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].type, b[i].type) << i;
        EXPECT_EQ(a[i].id, b[i].id) << i;
        EXPECT_EQ(a[i].price, b[i].price) << i;
        EXPECT_EQ(a[i].qty, b[i].qty) << i;
    }
}

TEST(Generator, ProducesEveryOrderTypeAndBothSidesAndCancels) {
    const obtest::GenConfig cfg;
    const auto              s = obtest::generate_stream(4, 5000, cfg);

    std::array<bool, 5> saw_type{};
    bool                saw_cancel = false, saw_buy = false, saw_sell = false;
    for (const ob::Command& c : s) {
        if (c.type == ob::CommandType::Cancel) {
            saw_cancel = true;
            continue;
        }
        saw_type[static_cast<std::size_t>(c.order_type)] = true;
        if (c.side == ob::Side::Buy) {
            saw_buy = true;
        } else {
            saw_sell = true;
        }
    }
    for (std::size_t i = 0; i < saw_type.size(); ++i) {
        EXPECT_TRUE(saw_type[i]) << "order type " << i << " never generated";
    }
    EXPECT_TRUE(saw_cancel);
    EXPECT_TRUE(saw_buy);
    EXPECT_TRUE(saw_sell);
}

// The generator must produce a workload that actually trades. A generator that
// silently degenerates into "everything rests" would make every later benchmark
// meaningless while looking fine.
TEST(Generator, ActuallyProducesTradesNotJustRestingOrders) {
    const auto s      = obtest::generate_stream(11, 5000, obtest::GenConfig{});
    const auto events = obtest::run_stream<ob::ReferenceEngine>(s);

    std::size_t trades = 0;
    for (const ob::Event& e : events) {
        if (e.type == ob::EventType::Trade) {
            ++trades;
        }
    }
    EXPECT_GT(trades, 100u) << "generated workload barely trades; it is not realistic";
}

TEST(Shrinker, ReducesToAMinimalFailingStream) {
    const auto stream = obtest::generate_stream(3, 200, obtest::GenConfig{});
    ASSERT_GE(stream.size(), 10u);

    // Predicate: "fails" iff the stream still contains the command at original
    // index 7's order id. The minimal failing input is therefore one command.
    const ob::OrderId target      = stream[7].id;
    const auto        still_fails = [target](const std::vector<ob::Command>& s) {
        for (const ob::Command& c : s) {
            if (c.id == target) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(still_fails(stream));

    const auto minimal = obtest::shrink(stream, still_fails);
    EXPECT_TRUE(still_fails(minimal));
    EXPECT_LT(minimal.size(), stream.size());
    EXPECT_LE(minimal.size(), 2u) << "shrinker did not reduce far enough";
}

TEST(Shrinker, LeavesAnAlreadyMinimalStreamAlone) {
    const std::vector<ob::Command> one{
        ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit, 10000, 10)};
    const auto minimal =
        obtest::shrink(one, [](const std::vector<ob::Command>& s) { return !s.empty(); });
    EXPECT_EQ(minimal.size(), 1u);
}

}  // namespace
