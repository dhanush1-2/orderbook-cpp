// tests/test_spsc_ring.cpp
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ob/spsc_ring.hpp>
#include <thread>
#include <type_traits>

namespace {

using ob::SpscRing;

template <std::size_t N>
concept ValidCapacity = requires { typename SpscRing<std::uint64_t, N>; };

TEST(SpscRing, CapacityMustBeAPowerOfTwo) {
    static_assert(ValidCapacity<2>);
    static_assert(ValidCapacity<1024>);
    static_assert(!ValidCapacity<0>);
    static_assert(!ValidCapacity<1>);
    static_assert(!ValidCapacity<3>);
    static_assert(!ValidCapacity<1000>);
    SUCCEED();
}

// Removing the alignas on the indices makes this fail, which is the false-sharing guard.
TEST(SpscRing, IsAlignedToACacheLine) {
    static_assert(alignof(SpscRing<std::uint64_t, 8>) >= ob::detail::kCacheLine);
    SUCCEED();
}

TEST(SpscRing, PopFromEmptyFailsAndLeavesTheOutputUntouched) {
    SpscRing<std::uint64_t, 8> r;
    EXPECT_TRUE(r.empty());
    std::uint64_t out = 42;
    EXPECT_FALSE(r.try_pop(out));
    EXPECT_EQ(out, 42u);
}

TEST(SpscRing, EverySlotIsUsableThenPushFails) {
    SpscRing<std::uint64_t, 8> r;
    for (std::uint64_t i = 0; i < 8; ++i) {
        EXPECT_TRUE(r.try_push(i)) << "slot " << i;
    }
    EXPECT_EQ(r.size_approx(), 8u);
    EXPECT_FALSE(r.try_push(99));

    std::uint64_t out = 0;
    ASSERT_TRUE(r.try_pop(out));
    EXPECT_EQ(out, 0u) << "a rejected push must not overwrite the oldest item";
    EXPECT_TRUE(r.try_push(99)) << "popping one must free one slot";
}

TEST(SpscRing, PreservesFifoOrderAcrossManyWraparounds) {
    SpscRing<std::uint64_t, 4> r;
    std::uint64_t              next_in  = 0;
    std::uint64_t              next_out = 0;
    for (int round = 0; round < 10'000; ++round) {
        const int burst = 1 + round % 4;
        for (int i = 0; i < burst; ++i) {
            ASSERT_TRUE(r.try_push(next_in++));
        }
        for (int i = 0; i < burst; ++i) {
            std::uint64_t out = 0;
            ASSERT_TRUE(r.try_pop(out));
            ASSERT_EQ(out, next_out++);
        }
        ASSERT_TRUE(r.empty());
    }
}

// This is the test that makes TSan meaningful for the ring. Weakening either index to
// relaxed makes TSan report here on every run; the Message test below passes TSan with
// the same defect, so it cannot stand in for this one. Keep the payload a word, keep the
// value used, and do not add a yield.
TEST(SpscRing, TwoThreadsTransferWordsForTheRaceDetector) {
    constexpr std::uint64_t     kItems = 2'000'000;
    SpscRing<std::uint64_t, 16> r;

    std::thread producer([&] {
        for (std::uint64_t seq = 0; seq < kItems;) {
            if (r.try_push(seq)) {
                ++seq;
            }
        }
    });

    std::uint64_t bad = 0;
    for (std::uint64_t seq = 0; seq < kItems;) {
        std::uint64_t out = 0;
        if (r.try_pop(out)) {
            if (out != seq) {
                ++bad;
            }
            ++seq;
        }
    }
    producer.join();
    EXPECT_EQ(bad, 0u);
}

// Every word derives from the sequence number, so a torn or stale slot is detectable.
struct Message {
    std::array<std::uint64_t, 8> words;
};
static_assert(std::is_trivially_copyable_v<Message>);

Message make_message(std::uint64_t seq) {
    Message m{};
    for (std::size_t i = 0; i < m.words.size(); ++i) {
        m.words[i] = seq ^ (0x9E37'79B9'7F4A'7C15ull * (i + 1));
    }
    return m;
}

// A tiny capacity forces constant full and empty transitions, which is where a
// stale cached index shows up as a missing, repeated or torn message.
TEST(SpscRing, TwoThreadsTransferEveryMessageIntactAndInOrder) {
    constexpr std::uint64_t kItems = 2'000'000;
    SpscRing<Message, 16>   r;

    std::thread producer([&] {
        for (std::uint64_t seq = 0; seq < kItems;) {
            if (r.try_push(make_message(seq))) {
                ++seq;
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::uint64_t bad = 0;
    for (std::uint64_t seq = 0; seq < kItems;) {
        Message m{};
        if (!r.try_pop(m)) {
            std::this_thread::yield();
            continue;
        }
        if (m.words != make_message(seq).words) {
            ++bad;
        }
        ++seq;
    }
    producer.join();

    EXPECT_EQ(bad, 0u);
    EXPECT_TRUE(r.empty());
}

}  // namespace
