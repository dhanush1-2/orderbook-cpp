#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

#include "../bench/scenarios.hpp"

namespace {

using ob::bench::build;
using ob::bench::kAllScenarios;
using ob::bench::measure_workload;
using ob::bench::Scenario;

TEST(Scenarios, EveryScenarioIsReproducibleFromItsSeed) {
    for (const Scenario s : kAllScenarios) {
        const auto a = build(s, 42, 5000);
        const auto b = build(s, 42, 5000);
        ASSERT_EQ(a.size(), b.size()) << ob::bench::name(s);
        for (std::size_t i = 0; i < a.size(); ++i) {
            ASSERT_EQ(a[i].id, b[i].id) << ob::bench::name(s) << " at " << i;
            ASSERT_EQ(a[i].price, b[i].price) << ob::bench::name(s) << " at " << i;
            ASSERT_EQ(a[i].qty, b[i].qty) << ob::bench::name(s) << " at " << i;
            ASSERT_EQ(a[i].type, b[i].type) << ob::bench::name(s) << " at " << i;
        }
    }
}

TEST(Scenarios, NoScenarioProducesManyRejections) {
    // A scenario full of rejected commands measures the validation path, not the
    // matching path, and would flatter the engine enormously.
    for (const Scenario s : kAllScenarios) {
        const auto   stats = measure_workload(build(s, 7, 20000));
        const double reject_rate =
            static_cast<double>(stats.rejects) / static_cast<double>(stats.commands);
        EXPECT_LT(reject_rate, 0.02) << ob::bench::name(s) << " reject rate " << reject_rate;
    }
}

TEST(Scenarios, RestOnlyNeverCrosses) {
    const auto stats = measure_workload(build(Scenario::RestOnly, 1, 20000));
    EXPECT_EQ(stats.trades, 0u) << "rest_only traded, so it is not rest-only";
    EXPECT_GT(stats.rests, 19000u);
}

TEST(Scenarios, CrossShallowTradesConstantlyAtShallowDepth) {
    const auto stats = measure_workload(build(Scenario::CrossShallow, 1, 20000));
    EXPECT_GT(stats.trades, 5000u);
    EXPECT_LT(stats.max_sweep_levels, 3u) << "cross_shallow swept deeply";
}

TEST(Scenarios, CrossDeepSweepsManyLevels) {
    const auto stats = measure_workload(build(Scenario::CrossDeep, 1, 20000));
    EXPECT_GT(stats.trades, 5000u);
    EXPECT_GT(stats.max_sweep_levels, 8u) << "cross_deep did not sweep deeply";
}

// 50% is a HARD CEILING, not a tuning target: every cancel consumes one resting
// order and every new order creates at most one, so cancels <= rests <= creates
// and the cancel share of commands cannot exceed one half. A "90% cancel"
// workload is arithmetically impossible when each cancel hits a distinct live
// order; what is ~90:10 in real markets is cancels per TRADE.
TEST(Scenarios, CancelHeavySitsAtTheFiftyPercentCeiling) {
    const auto   stats = measure_workload(build(Scenario::CancelHeavy, 1, 20000));
    const double cancel_rate =
        static_cast<double>(stats.cancels) / static_cast<double>(stats.commands);
    EXPECT_GT(cancel_rate, 0.45) << "cancel rate " << cancel_rate;
    EXPECT_LE(cancel_rate, 0.50) << "cancel rate exceeded the arithmetic ceiling";
    EXPECT_EQ(stats.unknown_cancels, 0u)
        << "a cancel missed a live order, so this measures rejection not cancel";
    EXPECT_EQ(stats.trades, 0u) << "cancel_heavy must not trade: nothing crosses";
}

TEST(Scenarios, MixedRealisticUsesEveryOrderTypeAndTrades) {
    const auto stats = measure_workload(build(Scenario::MixedRealistic, 1, 40000));
    for (std::size_t t = 0; t < 5; ++t) {
        EXPECT_GT(stats.by_type[t], 0u) << "order type " << t << " never generated";
    }
    EXPECT_GT(stats.trades, 1000u);
    const double cancel_rate =
        static_cast<double>(stats.cancels) / static_cast<double>(stats.commands);
    EXPECT_GT(cancel_rate, 0.20);
    EXPECT_LT(cancel_rate, 0.50);
}

TEST(Scenarios, WorstCaseSweepContainsAnEnormousSweep) {
    const auto stats = measure_workload(build(Scenario::WorstCaseSweep, 1, 20000));
    EXPECT_GT(stats.max_sweep_levels, 100u) << "worst_case_sweep never actually swept the book";
    EXPECT_GT(stats.max_events_per_command, 200u)
        << "the harness's event buffer sizing depends on this being large";
}

// The geometric property lives in the OFFSET, and rest_only exposes it directly
// because its price is kMid +/- (offset + 1). mixed_realistic shifts by -2 so that
// half the orders cross, which folds the distribution and makes
// distance-from-mid non-monotone: offset 0 (probability 1/2) lands at distance 2.
// Testing the fold rather than the distribution was a bug in the test, not the
// generator.
TEST(Scenarios, PriceOffsetsDecayGeometricallyFromTheTouch) {
    const auto  cmds = build(Scenario::RestOnly, 3, 200000);
    std::size_t bucket[6]{};
    for (const ob::Command& c : cmds) {
        if (c.type != ob::CommandType::New) {
            continue;
        }
        const std::size_t d = static_cast<std::size_t>(std::abs(c.price - ob::bench::kMid));
        ASSERT_GE(d, 1u) << "rest_only must never price at the touch";
        if (d <= 5) {
            ++bucket[d];
        }
    }
    // P(offset = k) = 2^-(k+1), so each distance must be strictly rarer than the
    // last, and roughly half as common.
    for (std::size_t d = 1; d < 5; ++d) {
        EXPECT_GT(bucket[d], bucket[d + 1]) << "distance " << d << " vs " << (d + 1);
        const double ratio = static_cast<double>(bucket[d]) / static_cast<double>(bucket[d + 1]);
        EXPECT_GT(ratio, 1.5) << "distance " << d << " ratio " << ratio;
        EXPECT_LT(ratio, 3.0) << "distance " << d << " ratio " << ratio;
    }
}

}  // namespace
