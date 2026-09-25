#include <gtest/gtest.h>

#include <string>

#include "../tools/tui.hpp"

namespace {

ob::L2Snapshot two_sided() {
    ob::L2Snapshot s{};
    s.seq        = 12345;
    s.bid_levels = 2;
    s.ask_levels = 2;
    s.bids[0]    = ob::L2Level{10000, 3, 150};
    s.bids[1]    = ob::L2Level{9990, 1, 40};
    s.asks[0]    = ob::L2Level{10002, 2, 90};
    s.asks[1]    = ob::L2Level{10005, 5, 300};
    return s;
}

TEST(Tui, RendersBothSidesWithPricesAndQuantities) {
    const std::string f = ob::tui::render(two_sided(), 24, 100, 1000, 20, 5e6);
    EXPECT_NE(f.find("10000"), std::string::npos) << "best bid missing";
    EXPECT_NE(f.find("10002"), std::string::npos) << "best ask missing";
    EXPECT_NE(f.find("10005"), std::string::npos) << "second ask missing";
    EXPECT_NE(f.find("spread 2"), std::string::npos) << "spread missing";
    EXPECT_NE(f.find("12345"), std::string::npos) << "sequence missing";
    EXPECT_NE(f.find("ctrl-c"), std::string::npos);
}

TEST(Tui, AsksAppearAboveTheSpreadAndBidsBelow) {
    const std::string f      = ob::tui::render(two_sided(), 24, 100, 0, 0, 0);
    const std::size_t ask    = f.find("10002");
    const std::size_t spread = f.find("spread");
    const std::size_t bid    = f.find("10000");
    ASSERT_NE(ask, std::string::npos);
    ASSERT_NE(spread, std::string::npos);
    ASSERT_NE(bid, std::string::npos);
    EXPECT_LT(ask, spread) << "asks must be above the spread line";
    EXPECT_LT(spread, bid) << "bids must be below the spread line";
}

TEST(Tui, ClampsToATinyTerminalWithoutOverrunning) {
    // const auto& not const auto: GCC 13's -Wrange-loop-construct flags the copy a
    // structured binding by value makes here.
    for (const auto& [r, c] : {std::pair{1, 1}, std::pair{2, 10}, std::pair{3, 19}}) {
        const std::string f = ob::tui::render(two_sided(), r, c, 0, 0, 0);
        EXPECT_NE(f.find("too small"), std::string::npos) << r << "x" << c;
    }
    // Just big enough must render rather than refuse.
    const std::string ok = ob::tui::render(two_sided(), 6, 40, 0, 0, 0);
    EXPECT_EQ(ok.find("too small"), std::string::npos);
}

TEST(Tui, RendersAnEmptyBookWithoutCrashing) {
    const ob::L2Snapshot empty{};
    const std::string    f = ob::tui::render(empty, 24, 100, 0, 0, 0);
    EXPECT_NE(f.find("one side empty"), std::string::npos);
}

TEST(Tui, OneSidedBookReportsNoSpread) {
    ob::L2Snapshot s{};
    s.bid_levels        = 1;
    s.bids[0]           = ob::L2Level{10000, 1, 10};
    const std::string f = ob::tui::render(s, 24, 100, 0, 0, 0);
    EXPECT_NE(f.find("one side empty"), std::string::npos);
    EXPECT_NE(f.find("10000"), std::string::npos);
}

// A fixed "M ops/s" unit reads 0.00 at any rate a human can actually watch, which
// is the rate --rate exists to produce. The readout must stay informative.
TEST(Tui, RateReadoutUsesUnitsThatStayReadableAtHumanRates) {
    const ob::L2Snapshot s = two_sided();
    EXPECT_NE(ob::tui::render(s, 24, 100, 0, 0, 2.0e3).find("2.0 K ops/s"), std::string::npos);
    EXPECT_NE(ob::tui::render(s, 24, 100, 0, 0, 250.0).find("250 ops/s"), std::string::npos);
    EXPECT_NE(ob::tui::render(s, 24, 100, 0, 0, 3.4e7).find("34.00 M ops/s"), std::string::npos);
    // The thing that was wrong: a watchable rate must not render as 0.00.
    EXPECT_EQ(ob::tui::render(s, 24, 100, 0, 0, 2.0e3).find("0.00"), std::string::npos);
}

TEST(Tui, BarScalesToTheLargestLevelAndNeverExceedsTheWidth) {
    const ob::L2Snapshot s = two_sided();
    const std::string    f = ob::tui::render(s, 24, 100, 0, 0, 0);
    // Largest level (300) must produce the longest run of '#'.
    std::size_t longest = 0, cur = 0;
    for (const char ch : f) {
        cur     = (ch == '#') ? cur + 1 : 0;
        longest = cur > longest ? cur : longest;
    }
    EXPECT_GT(longest, 0u) << "no bars drawn at all";
    EXPECT_LE(longest, 100u - 46u) << "a bar exceeded the available width";
}

}  // namespace
