#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

#include "../bench/histogram.hpp"

namespace {

using ob::bench::Histogram;

// 1..100 makes every nearest-rank answer checkable by hand.
Histogram one_to_hundred() {
    Histogram h(100);
    for (std::uint32_t v = 1; v <= 100; ++v) {
        h.record(v);
    }
    return h;
}

TEST(Histogram, StartsEmpty) {
    const Histogram h(16);
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.capacity(), 16u);
    EXPECT_EQ(h.saturated(), 0u);
}

TEST(Histogram, NearestRankPercentilesOnAKnownDistribution) {
    Histogram h = one_to_hundred();
    ASSERT_EQ(h.count(), 100u);
    EXPECT_EQ(h.percentile(1.0), 1u);
    EXPECT_EQ(h.percentile(50.0), 50u);
    EXPECT_EQ(h.percentile(90.0), 90u);
    EXPECT_EQ(h.percentile(99.0), 99u);
    EXPECT_EQ(h.percentile(100.0), 100u);
}

TEST(Histogram, MinMaxMean) {
    Histogram h = one_to_hundred();
    EXPECT_EQ(h.min(), 1u);
    EXPECT_EQ(h.max(), 100u);
    EXPECT_NEAR(h.mean(), 50.5, 1e-9);
}

TEST(Histogram, PercentileIsOrderIndependent) {
    std::vector<std::uint32_t> v(100);
    std::iota(v.begin(), v.end(), 1u);
    std::mt19937_64 rng(4);
    std::shuffle(v.begin(), v.end(), rng);

    Histogram h(100);
    for (const std::uint32_t x : v) {
        h.record(x);
    }
    EXPECT_EQ(h.percentile(50.0), 50u);
    EXPECT_EQ(h.percentile(99.0), 99u);
}

TEST(Histogram, RecordingAfterReadingStillWorks) {
    Histogram h(200);
    for (std::uint32_t v = 1; v <= 100; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.percentile(50.0), 50u);  // sorts internally
    for (std::uint32_t v = 101; v <= 200; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.percentile(50.0), 100u) << "cached sort was not invalidated";
    EXPECT_EQ(h.max(), 200u);
}

// A single sample means p99 and max are the same sample. The harness prints the
// count next to every percentile precisely so this is visible.
TEST(Histogram, SingleSample) {
    Histogram h(1);
    h.record(7);
    EXPECT_EQ(h.percentile(50.0), 7u);
    EXPECT_EQ(h.percentile(99.9), 7u);
    EXPECT_EQ(h.min(), 7u);
    EXPECT_EQ(h.max(), 7u);
}

TEST(Histogram, ValuesTooLargeForUint32SaturateAndAreCounted) {
    Histogram h(4);
    h.record(10);
    h.record(std::uint64_t{1} << 40);
    EXPECT_EQ(h.saturated(), 1u);
    EXPECT_EQ(h.max(), std::numeric_limits<std::uint32_t>::max());
    // The saturation count is the signal that a run is not trustworthy.
}

TEST(Histogram, MergeCombinesBothSetsOfSamples) {
    Histogram a(4), b(4);
    a.record(1);
    a.record(3);
    b.record(2);
    b.record(4);
    a.merge(b);
    EXPECT_EQ(a.count(), 4u);
    EXPECT_EQ(a.percentile(100.0), 4u);
    EXPECT_EQ(a.min(), 1u);
}

TEST(Histogram, ClearResetsCountAndSaturationButKeepsCapacity) {
    Histogram h(8);
    h.record(1);
    h.record(std::uint64_t{1} << 40);
    h.clear();
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.saturated(), 0u);
    EXPECT_EQ(h.capacity(), 8u);
}

// record() must never reallocate: it runs inside the measured loop, and an
// allocation there would show up as a latency spike caused by the harness.
TEST(HistogramDeathTest, RecordingBeyondCapacityAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Histogram h(1);
    h.record(1);
    EXPECT_DEATH(h.record(2), "");
}

}  // namespace
