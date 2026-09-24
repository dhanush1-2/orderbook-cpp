#include <gtest/gtest.h>

#include <array>
#include <ob/engine_concept.hpp>

namespace {

ob::Event mk(ob::Seq s, ob::EventType t) {
    ob::Event e{};
    e.seq  = s;
    e.type = t;
    return e;
}

TEST(EventBuffer, StartsEmpty) {
    std::array<ob::Event, 4> storage{};
    const ob::EventBuffer    buf(storage.data(), storage.size());
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_EQ(buf.capacity(), 4u);
}

TEST(EventBuffer, PushAppendsInOrder) {
    std::array<ob::Event, 4> storage{};
    ob::EventBuffer          buf(storage.data(), storage.size());
    buf.push(mk(1, ob::EventType::Accepted));
    buf.push(mk(2, ob::EventType::Trade));

    ASSERT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf[0].seq, 1u);
    EXPECT_EQ(buf[1].type, ob::EventType::Trade);
}

TEST(EventBuffer, ClearResetsSizeButNotCapacity) {
    std::array<ob::Event, 4> storage{};
    ob::EventBuffer          buf(storage.data(), storage.size());
    buf.push(mk(1, ob::EventType::Accepted));
    buf.clear();
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.capacity(), 4u);
}

// Edge case E46: a buffer sized exactly to the event count must work.
TEST(EventBuffer, FillingToExactCapacitySucceeds) {
    std::array<ob::Event, 2> storage{};
    ob::EventBuffer          buf(storage.data(), storage.size());
    buf.push(mk(1, ob::EventType::Accepted));
    buf.push(mk(2, ob::EventType::Filled));
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf.view().size(), 2u);
}

TEST(EventBuffer, ViewIsIterableAndRangeBased) {
    std::array<ob::Event, 4> storage{};
    ob::EventBuffer          buf(storage.data(), storage.size());
    buf.push(mk(7, ob::EventType::Accepted));
    buf.push(mk(8, ob::EventType::Filled));

    ob::Seq total = 0;
    for (const ob::Event& e : buf) {
        total += e.seq;
    }
    EXPECT_EQ(total, 15u);
}

TEST(FixedEventBuffer, OwnsItsStorageAndReportsCapacity) {
    ob::FixedEventBuffer<8> buf;
    EXPECT_EQ(buf.capacity(), 8u);
    buf.push(mk(1, ob::EventType::Accepted));
    EXPECT_EQ(buf.size(), 1u);
}

// Edge case E47: overflow is a programming error and must abort, not truncate.
// A truncating buffer would silently violate "every command produces at least
// one event", which is the contract the whole test suite leans on.
TEST(EventBufferDeathTest, OverflowAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::array<ob::Event, 1> storage{};
    ob::EventBuffer          buf(storage.data(), storage.size());
    buf.push(mk(1, ob::EventType::Accepted));
    EXPECT_DEATH(buf.push(mk(2, ob::EventType::Filled)), "");
}

}  // namespace
