#include <ob/types.hpp>

#include <gtest/gtest.h>

#include <limits>

namespace {

TEST(Types, OppositeSideFlips) {
    EXPECT_EQ(ob::opposite(ob::Side::Buy), ob::Side::Sell);
    EXPECT_EQ(ob::opposite(ob::Side::Sell), ob::Side::Buy);
}

TEST(Types, PriceRangeIsInclusiveOfBothBounds) {
    EXPECT_TRUE(ob::price_in_range(ob::kMinTick));
    EXPECT_TRUE(ob::price_in_range(ob::kMaxTick));
    EXPECT_FALSE(ob::price_in_range(ob::kMinTick - 1));
    EXPECT_FALSE(ob::price_in_range(ob::kMaxTick + 1));
}

// Edge cases E12, E13: the ladder is a flat array, so an out-of-range price is an
// out-of-bounds write unless it is rejected first. These two assertions are the
// first line of that defence.
TEST(Types, ExtremePricesAreOutOfRange) {
    EXPECT_FALSE(ob::price_in_range(std::numeric_limits<ob::Ticks>::min()));
    EXPECT_FALSE(ob::price_in_range(std::numeric_limits<ob::Ticks>::max()));
    EXPECT_FALSE(ob::price_in_range(0));      // tick 0 is deliberately not valid
    EXPECT_FALSE(ob::price_in_range(-1));
}

TEST(Types, LadderSizeCoversExactlyTheValidTickRange) {
    EXPECT_EQ(ob::kLadderSize, static_cast<std::size_t>(ob::kMaxTick - ob::kMinTick + 1));
}

// Edge cases E10, E11.
TEST(Types, QuantityValidityRejectsZeroAndOversize) {
    EXPECT_FALSE(ob::qty_valid(0));
    EXPECT_TRUE(ob::qty_valid(1));
    EXPECT_TRUE(ob::qty_valid(ob::kMaxOrderQty));
    EXPECT_FALSE(ob::qty_valid(ob::kMaxOrderQty + 1));
}

TEST(Types, NoPriceSentinelIsNotAValidPrice) {
    EXPECT_FALSE(ob::price_in_range(ob::kNoPrice));
}

TEST(Types, EnumsAreOneByte) {
    static_assert(sizeof(ob::Side) == 1);
    static_assert(sizeof(ob::OrderType) == 1);
    static_assert(sizeof(ob::RejectReason) == 1);
    static_assert(sizeof(ob::CancelReason) == 1);
    SUCCEED();
}

}  // namespace
