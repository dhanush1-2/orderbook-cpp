#include <gtest/gtest.h>

#include <ob/order_pool.hpp>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {

TEST(Order, IsExactlyThirtyTwoBytes) {
    static_assert(sizeof(ob::Order) == 32, "Order must stay 32 bytes");
    static_assert(alignof(ob::Order) == 8);
    static_assert(std::is_trivially_copyable_v<ob::Order>);
    // 2 per 64-byte line on x86, 4 per 128-byte line on Apple Silicon.
    SUCCEED();
}

TEST(OrderPool, FreshPoolIsEmptyWithTheRequestedCapacity) {
    const ob::OrderPool p(16);
    EXPECT_EQ(p.capacity(), 16u);
    EXPECT_EQ(p.size(), 0u);
    EXPECT_FALSE(p.full());
    EXPECT_EQ(p.free_list_length(), 16u);
}

TEST(OrderPool, AllocReturnsDistinctSlotsUntilExhausted) {
    ob::OrderPool                p(64);
    std::unordered_set<ob::Slot> seen;
    for (std::size_t i = 0; i < 64; ++i) {
        const ob::Slot s = p.alloc();
        ASSERT_NE(s, ob::kInvalidSlot) << "ran out early at i=" << i;
        p.at(s).id = static_cast<ob::OrderId>(i + 1);  // claim it
        EXPECT_TRUE(seen.insert(s).second) << "slot " << s << " handed out twice";
    }
    EXPECT_EQ(p.size(), 64u);
    EXPECT_TRUE(p.full());
    EXPECT_EQ(p.alloc(), ob::kInvalidSlot);
    EXPECT_EQ(p.free_list_length(), 0u);
}

// Spec E39: exhaustion is a capacity condition, and freeing makes room again.
TEST(OrderPool, FreeingMakesCapacityAvailableAgain) {
    ob::OrderPool         p(4);
    std::vector<ob::Slot> slots;
    for (int i = 0; i < 4; ++i) {
        const ob::Slot s = p.alloc();
        p.at(s).id       = static_cast<ob::OrderId>(i + 1);
        slots.push_back(s);
    }
    ASSERT_EQ(p.alloc(), ob::kInvalidSlot);

    p.free(slots[2]);
    EXPECT_EQ(p.size(), 3u);
    const ob::Slot again = p.alloc();
    EXPECT_NE(again, ob::kInvalidSlot);
    EXPECT_EQ(p.size(), 4u);
}

TEST(OrderPool, StoredFieldsRoundTrip) {
    ob::OrderPool  p(8);
    const ob::Slot s = p.alloc();
    ob::Order&     o = p.at(s);
    o.id             = 12345;
    o.price          = 10050;
    o.remaining      = 300;
    o.side           = ob::Side::Sell;

    const ob::Order& r = p.at(s);
    EXPECT_EQ(r.id, 12345u);
    EXPECT_EQ(r.price, 10050);
    EXPECT_EQ(r.remaining, 300u);
    EXPECT_EQ(r.side, ob::Side::Sell);
}

TEST(OrderPool, ResetReturnsEverySlotToTheFreeList) {
    ob::OrderPool p(8);
    for (int i = 0; i < 8; ++i) {
        p.at(p.alloc()).id = static_cast<ob::OrderId>(i + 1);
    }
    ASSERT_TRUE(p.full());
    p.reset();
    EXPECT_EQ(p.size(), 0u);
    EXPECT_FALSE(p.full());
    EXPECT_EQ(p.free_list_length(), 8u);
    EXPECT_NE(p.alloc(), ob::kInvalidSlot);
}

// Construction must leave every page resident, because the harness relies on it
// instead of a prefault() call. A first pass over a large arena that paid page
// faults would show up as latency outliers attributable to the harness.
TEST(OrderPool, ConstructionLeavesEveryPageResident) {
    ob::OrderPool p(1 << 16);
    for (std::size_t i = 0; i < p.capacity(); ++i) {
        ASSERT_EQ(p.at(static_cast<ob::Slot>(i)).id, 0u) << "slot " << i;
    }
    EXPECT_EQ(p.size(), 0u);
}

// The free list must stay acyclic. A cycle means a double free, and the invariant
// checker relies on this helper to detect it.
TEST(OrderPool, FreeListLengthPlusLiveCountAlwaysCoversThePool) {
    ob::OrderPool         p(32);
    std::vector<ob::Slot> live;
    for (int i = 0; i < 20; ++i) {
        const ob::Slot s = p.alloc();
        p.at(s).id       = static_cast<ob::OrderId>(i + 1);
        live.push_back(s);
        ASSERT_EQ(p.free_list_length() + p.size(), p.capacity());
    }
    for (const ob::Slot s : live) {
        p.free(s);
        ASSERT_EQ(p.free_list_length() + p.size(), p.capacity());
    }
}

// A double free corrupts the free list into a cycle, and the symptom appears
// arbitrarily far away. Catching it at the call site is worth an assert.
TEST(OrderPoolDeathTest, DoubleFreeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ob::OrderPool  p(4);
    const ob::Slot s = p.alloc();
    p.at(s).id       = 1;
    p.free(s);
    EXPECT_DEATH(p.free(s), "");
}

TEST(OrderPoolDeathTest, OutOfRangeSlotAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ob::OrderPool p(4);
    // at() is [[nodiscard]], so the result must be explicitly discarded even
    // though this statement never returns.
    EXPECT_DEATH(static_cast<void>(p.at(99)), "");
}

}  // namespace
