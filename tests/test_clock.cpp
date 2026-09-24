#include <gtest/gtest.h>

#include <cstdio>

#include "../bench/clock.hpp"

namespace {

using ob::bench::Clock;
using ob::bench::do_not_optimize;

TEST(Clock, CounterFrequencyIsPlausible) {
    const std::uint64_t hz = Clock::instance().counter_hz();
    // 1 MHz to 10 GHz. Anything outside that means calibration failed.
    EXPECT_GT(hz, 1'000'000u);
    EXPECT_LT(hz, 10'000'000'000u);
}

TEST(Clock, TickConversionIsConsistent) {
    const Clock& c = Clock::instance();
    EXPECT_DOUBLE_EQ(c.ticks_to_ns(0), 0.0);
    EXPECT_GT(c.ticks_to_ns(1000), c.ticks_to_ns(999));
    // ns_per_tick must agree with the reported frequency.
    EXPECT_NEAR(c.ns_per_tick(), 1e9 / static_cast<double>(c.counter_hz()), 1e-9);
}

TEST(Clock, RawIsMonotonicallyNonDecreasing) {
    std::uint64_t prev = Clock::raw();
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t now = Clock::raw();
        ASSERT_GE(now, prev) << "counter went backwards at i=" << i;
        prev = now;
    }
}

TEST(Clock, ResolutionIsMeasuredAndReported) {
    const Clock& c = Clock::instance();
    EXPECT_GT(c.resolution_ticks(), 0u);
    EXPECT_GT(c.resolution_min_ticks(), 0u);
    // Portable bounds; the real values get printed so a human sees them.
    EXPECT_GT(c.resolution_ns(), 0.05);
    EXPECT_LT(c.resolution_ns(), 1000.0);
}

// The minimum and the median disagree materially on this hardware (~17 ns vs
// ~42 ns). The median is what gets published, because the minimum is
// outlier-sensitive and quoting it would understate the harness's error bars.
TEST(Clock, MedianResolutionIsTheConservativeFigureAndIsUsedByDefault) {
    const Clock& c = Clock::instance();
    EXPECT_GE(c.resolution_ticks(), c.resolution_min_ticks())
        << "median must not be below the minimum";
    // resolution_ns() must be the MEDIAN, not the optimistic minimum.
    EXPECT_DOUBLE_EQ(c.resolution_ns(), c.ticks_to_ns(c.resolution_ticks()));
}

TEST(Clock, OverheadIsMeasuredAndSerializedCostsMore) {
    const Clock& c = Clock::instance();
    EXPECT_GT(c.overhead_ns_raw(), 0.0);
    EXPECT_GT(c.overhead_ns_serialized(), 0.0);
    EXPECT_LT(c.overhead_ns_serialized(), 500.0);
    // isb / rdtscp exists precisely to cost something. If the serialized read is
    // not more expensive, the barrier is not being emitted.
    EXPECT_GT(c.overhead_ns_serialized(), c.overhead_ns_raw());
}

// The guard test. If do_not_optimize stops working, every benchmark in this
// project silently starts reporting the cost of nothing, and no other test
// notices. 1e6 dependent adds cannot take less than 100 us.
TEST(Clock, DoNotOptimizeActuallyPreventsElision) {
    const std::uint64_t t0  = Clock::raw_serialized();
    std::uint64_t       acc = 0;
    for (int i = 0; i < 1'000'000; ++i) {
        acc += static_cast<std::uint64_t>(i) * 2654435761u;
        do_not_optimize(acc);
    }
    const std::uint64_t t1 = Clock::raw_serialized();
    do_not_optimize(acc);

    const double ns = Clock::instance().ticks_to_ns(t1 - t0);
    EXPECT_GT(ns, 100'000.0) << "1e6 barriered adds took " << ns
                             << " ns; the barrier is not working";
}

TEST(Clock, ReportPrintsSomethingUseful) {
    Clock::instance().print_report(stdout);
    SUCCEED();
}

}  // namespace
