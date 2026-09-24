#include <gtest/gtest.h>

#include <iterator>
#include <ob/level_bitmap.hpp>
#include <random>
#include <set>

namespace {

using ob::LevelBitmap;

TEST(LevelBitmap, GeometryMatchesTheLadder) {
    static_assert(LevelBitmap::kBits == ob::kLadderSize);
    static_assert(LevelBitmap::kBits % 64 == 0);
    static_assert((LevelBitmap::kBits / 64) % 64 == 0);
    EXPECT_EQ(LevelBitmap::kBits, 65536u);
}

TEST(LevelBitmap, StartsEmpty) {
    const LevelBitmap b;
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.popcount(), 0u);
    EXPECT_EQ(b.next_set_at_or_above(0), LevelBitmap::kNotFound);
    EXPECT_EQ(b.prev_set_at_or_below(LevelBitmap::kBits - 1), LevelBitmap::kNotFound);
}

TEST(LevelBitmap, SetTestClear) {
    LevelBitmap b;
    EXPECT_FALSE(b.test(1234));
    b.set(1234);
    EXPECT_TRUE(b.test(1234));
    EXPECT_FALSE(b.empty());
    EXPECT_EQ(b.popcount(), 1u);
    b.clear(1234);
    EXPECT_FALSE(b.test(1234));
    EXPECT_TRUE(b.empty()) << "clearing the last bit must collapse the whole hierarchy";
}

TEST(LevelBitmap, FindsASingleBitFromEitherDirection) {
    for (const std::uint32_t i :
         {0u, 1u, 63u, 64u, 65u, 4095u, 4096u, 4097u, 32768u, 65534u, 65535u}) {
        LevelBitmap b;
        b.set(i);
        EXPECT_EQ(b.next_set_at_or_above(0), i) << "bit " << i;
        EXPECT_EQ(b.next_set_at_or_above(i), i) << "bit " << i << " at-or-above is inclusive";
        EXPECT_EQ(b.prev_set_at_or_below(LevelBitmap::kBits - 1), i) << "bit " << i;
        EXPECT_EQ(b.prev_set_at_or_below(i), i) << "bit " << i << " at-or-below is inclusive";
        if (i + 1 < LevelBitmap::kBits) {
            EXPECT_EQ(b.next_set_at_or_above(i + 1), LevelBitmap::kNotFound) << "bit " << i;
        }
        if (i > 0) {
            EXPECT_EQ(b.prev_set_at_or_below(i - 1), LevelBitmap::kNotFound) << "bit " << i;
        }
    }
}

// The shift-by-64 trap. Every one of these indices puts a masking helper at a
// word boundary, which is where `~0ull << (b+1)` would be undefined behavior.
TEST(LevelBitmap, WordAndHierarchyBoundaries) {
    LevelBitmap b;
    b.set(63);
    b.set(64);
    b.set(4095);   // last bit of L1 word 0's coverage
    b.set(4096);   // first bit of L1 word 1's coverage
    b.set(65535);  // very last bit

    EXPECT_EQ(b.next_set_at_or_above(0), 63u);
    EXPECT_EQ(b.next_set_at_or_above(64), 64u);
    EXPECT_EQ(b.next_set_at_or_above(65), 4095u);
    EXPECT_EQ(b.next_set_at_or_above(4096), 4096u);
    EXPECT_EQ(b.next_set_at_or_above(4097), 65535u);
    EXPECT_EQ(b.next_set_at_or_above(65535), 65535u);

    EXPECT_EQ(b.prev_set_at_or_below(65535), 65535u);
    EXPECT_EQ(b.prev_set_at_or_below(65534), 4096u);
    EXPECT_EQ(b.prev_set_at_or_below(4095), 4095u);
    EXPECT_EQ(b.prev_set_at_or_below(4094), 64u);
    EXPECT_EQ(b.prev_set_at_or_below(63), 63u);
    EXPECT_EQ(b.prev_set_at_or_below(62), LevelBitmap::kNotFound);
}

TEST(LevelBitmap, OutOfRangeQueriesReturnNotFoundRatherThanReadingOutOfBounds) {
    LevelBitmap b;
    b.set(100);
    EXPECT_EQ(b.next_set_at_or_above(LevelBitmap::kBits), LevelBitmap::kNotFound);
    EXPECT_EQ(b.next_set_at_or_above(LevelBitmap::kBits + 1000), LevelBitmap::kNotFound);
}

TEST(LevelBitmap, AllBitsSetMakesEveryQueryTheIdentity) {
    LevelBitmap b;
    for (std::uint32_t i = 0; i < LevelBitmap::kBits; ++i) {
        b.set(i);
    }
    EXPECT_EQ(b.popcount(), LevelBitmap::kBits);
    for (const std::uint32_t i : {0u, 1u, 63u, 64u, 4095u, 4096u, 33333u, 65535u}) {
        EXPECT_EQ(b.next_set_at_or_above(i), i) << i;
        EXPECT_EQ(b.prev_set_at_or_below(i), i) << i;
    }
}

TEST(LevelBitmap, ClearingAllBitsRestoresTheEmptyState) {
    LevelBitmap b;
    for (std::uint32_t i = 0; i < LevelBitmap::kBits; i += 7) {
        b.set(i);
    }
    for (std::uint32_t i = 0; i < LevelBitmap::kBits; i += 7) {
        b.clear(i);
    }
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.popcount(), 0u);
    EXPECT_EQ(b.next_set_at_or_above(0), LevelBitmap::kNotFound);
}

TEST(LevelBitmap, ResetEmptiesEverything) {
    LevelBitmap b;
    b.set(10);
    b.set(50000);
    b.reset();
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.next_set_at_or_above(0), LevelBitmap::kNotFound);
}

// The strongest test: a std::set says what the answer is, for random set/clear
// sequences and random queries. Hierarchy-collapse bugs need a specific
// arrangement of bits to show up, which is exactly what randomization finds.
TEST(LevelBitmap, MatchesASetModelUnderRandomMutationAndQueries) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        LevelBitmap             b;
        std::set<std::uint32_t> model;
        std::mt19937_64         rng(seed);

        for (int op = 0; op < 4000; ++op) {
            const std::uint32_t i = static_cast<std::uint32_t>(rng() % LevelBitmap::kBits);
            if (rng() % 2 == 0) {
                b.set(i);
                model.insert(i);
            } else {
                b.clear(i);
                model.erase(i);
            }
            ASSERT_EQ(b.empty(), model.empty()) << "seed " << seed << " op " << op;
            ASSERT_EQ(b.popcount(), model.size()) << "seed " << seed << " op " << op;

            const std::uint32_t q = static_cast<std::uint32_t>(rng() % LevelBitmap::kBits);

            const auto          up        = model.lower_bound(q);
            const std::uint32_t want_next = (up == model.end()) ? LevelBitmap::kNotFound : *up;
            ASSERT_EQ(b.next_set_at_or_above(q), want_next)
                << "seed " << seed << " op " << op << " q " << q;

            const auto          after = model.upper_bound(q);
            const std::uint32_t want_prev =
                (after == model.begin()) ? LevelBitmap::kNotFound : *std::prev(after);
            ASSERT_EQ(b.prev_set_at_or_below(q), want_prev)
                << "seed " << seed << " op " << op << " q " << q;
        }
    }
}

}  // namespace
