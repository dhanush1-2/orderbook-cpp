#include <gtest/gtest.h>

#include <ob/id_index.hpp>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

TEST(IdIndex, CapacityIsAPowerOfTwoAndAtLeastTwiceTheRequest) {
    const ob::IdIndex ix(100);
    EXPECT_GE(ix.capacity(), 200u);
    EXPECT_EQ(ix.capacity() & (ix.capacity() - 1), 0u) << "capacity must be 2^n";
    EXPECT_GE(ix.max_load(), 100u);
}

TEST(IdIndex, InsertThenFind) {
    ob::IdIndex ix(16);
    EXPECT_TRUE(ix.insert(42, 7));
    EXPECT_EQ(ix.find(42), 7u);
    EXPECT_EQ(ix.size(), 1u);
}

TEST(IdIndex, FindOfAnAbsentIdReturnsInvalidSlot) {
    ob::IdIndex ix(16);
    EXPECT_TRUE(ix.insert(42, 7));
    EXPECT_EQ(ix.find(43), ob::kInvalidSlot);
}

TEST(IdIndex, DuplicateInsertIsRefusedAndLeavesTheOriginal) {
    ob::IdIndex ix(16);
    ASSERT_TRUE(ix.insert(42, 7));
    EXPECT_FALSE(ix.insert(42, 9));
    EXPECT_EQ(ix.find(42), 7u);
    EXPECT_EQ(ix.size(), 1u);
}

TEST(IdIndex, EraseRemovesAndReportsWhetherItFoundAnything) {
    ob::IdIndex ix(16);
    EXPECT_TRUE(ix.insert(42, 7));
    EXPECT_TRUE(ix.erase(42));
    EXPECT_EQ(ix.find(42), ob::kInvalidSlot);
    EXPECT_EQ(ix.size(), 0u);
    EXPECT_FALSE(ix.erase(42)) << "erasing twice must report not-found";
}

// Spec E40: the load ceiling is a capacity rejection, never a rehash.
TEST(IdIndex, RefusesInsertAtTheLoadCeilingAndNeverRehashes) {
    ob::IdIndex       ix(8);
    const std::size_t cap     = ix.capacity();
    const std::size_t ceiling = ix.max_load();

    for (std::size_t i = 1; i <= ceiling; ++i) {
        ASSERT_TRUE(ix.insert(static_cast<ob::OrderId>(i), static_cast<ob::Slot>(i)))
            << "failed at " << i;
    }
    EXPECT_TRUE(ix.full());
    EXPECT_FALSE(ix.insert(999999, 1));
    EXPECT_EQ(ix.capacity(), cap) << "capacity changed, so it rehashed";
}

// THE test for backward-shift deletion. Build a real collision chain, remove an
// element from the middle of it, and confirm the rest are still reachable. With
// a naive "just clear the slot" deletion this fails, and with a buggy backward
// shift it fails.
TEST(IdIndex, CollisionChainSurvivesDeletionFromTheMiddle) {
    ob::IdIndex         ix(8);
    const std::uint64_t mask = ix.capacity() - 1;

    std::vector<ob::OrderId> colliding;
    const std::uint64_t      target = ob::IdIndex::hash(1) & mask;
    for (ob::OrderId id = 1; id < 2'000'000 && colliding.size() < 4; ++id) {
        if ((ob::IdIndex::hash(id) & mask) == target) {
            colliding.push_back(id);
        }
    }
    ASSERT_EQ(colliding.size(), 4u) << "could not construct a collision chain";

    for (std::size_t i = 0; i < colliding.size(); ++i) {
        ASSERT_TRUE(ix.insert(colliding[i], static_cast<ob::Slot>(100 + i)));
    }
    for (std::size_t i = 0; i < colliding.size(); ++i) {
        ASSERT_EQ(ix.find(colliding[i]), 100u + i);
    }

    ASSERT_TRUE(ix.erase(colliding[1]));
    EXPECT_EQ(ix.find(colliding[1]), ob::kInvalidSlot);
    EXPECT_EQ(ix.find(colliding[0]), 100u);
    EXPECT_EQ(ix.find(colliding[2]), 102u) << "backward shift broke the chain";
    EXPECT_EQ(ix.find(colliding[3]), 103u) << "backward shift broke the chain";
}

// Property test against std::unordered_map. Randomized insert and erase is how
// backward-shift bugs actually surface, because they need a specific arrangement.
TEST(IdIndex, MatchesAReferenceMapUnderRandomInsertAndErase) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        ob::IdIndex                               ix(512);
        std::unordered_map<ob::OrderId, ob::Slot> model;
        std::mt19937_64                           rng(seed);

        for (int op = 0; op < 20000; ++op) {
            const ob::OrderId id = 1 + (rng() % 2000);
            if (rng() % 2 == 0 && ix.size() < ix.max_load()) {
                const ob::Slot slot = static_cast<ob::Slot>(rng() % 100000);
                const bool     a    = ix.insert(id, slot);
                const bool     b    = model.emplace(id, slot).second;
                ASSERT_EQ(a, b) << "seed " << seed << " op " << op;
            } else {
                const bool a = ix.erase(id);
                const bool b = model.erase(id) != 0;
                ASSERT_EQ(a, b) << "seed " << seed << " op " << op;
            }
            ASSERT_EQ(ix.size(), model.size()) << "seed " << seed << " op " << op;
        }
        for (const auto& [id, slot] : model) {
            ASSERT_EQ(ix.find(id), slot) << "seed " << seed << " lost id " << id;
        }
    }
}

TEST(IdIndex, ResetEmptiesWithoutChangingCapacity) {
    ob::IdIndex ix(16);
    for (ob::OrderId id = 1; id <= 8; ++id) {
        EXPECT_TRUE(ix.insert(id, static_cast<ob::Slot>(id)));
    }
    const std::size_t cap = ix.capacity();
    ix.reset();
    EXPECT_EQ(ix.size(), 0u);
    EXPECT_EQ(ix.capacity(), cap);
    EXPECT_EQ(ix.find(1), ob::kInvalidSlot);
}

TEST(IdIndex, IdZeroIsNotStorable) {
    ob::IdIndex ix(16);
    // Id 0 is the empty marker. The engine rejects it before reaching here
    // (spec E49); this asserts the index itself does not silently accept it.
    EXPECT_FALSE(ix.insert(0, 1));
    EXPECT_EQ(ix.find(0), ob::kInvalidSlot);
}

}  // namespace
