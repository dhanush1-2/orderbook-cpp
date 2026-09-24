#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ob/seqlock.hpp>
#include <thread>
#include <vector>

namespace {

struct Payload {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
};

TEST(Seqlock, StartsAtAnEvenSequenceAndLoadsTheInitialValue) {
    const ob::Seqlock<Payload> s;
    EXPECT_EQ(s.sequence() % 2, 0u);
    Payload p{1, 2, 3};
    EXPECT_TRUE(s.try_load(p));
    EXPECT_EQ(p.a, 0u);
}

TEST(Seqlock, StoreThenLoadRoundTrips) {
    ob::Seqlock<Payload> s;
    s.store(Payload{7, 8, 9});
    Payload p{};
    ASSERT_TRUE(s.try_load(p));
    EXPECT_EQ(p.a, 7u);
    EXPECT_EQ(p.b, 8u);
    EXPECT_EQ(p.c, 9u);
}

TEST(Seqlock, SequenceAdvancesByTwoPerStore) {
    ob::Seqlock<Payload> s;
    const std::uint64_t  before = s.sequence();
    s.store(Payload{1, 1, 1});
    EXPECT_EQ(s.sequence(), before + 2);
    s.store(Payload{2, 2, 2});
    EXPECT_EQ(s.sequence(), before + 4);
}

// The property that matters: a reader NEVER observes a half-written payload. The
// writer only ever publishes triples where a == b == c, so any reader that sees
// them differ has observed a tear the protocol failed to reject.
TEST(Seqlock, ReadersNeverObserveATornPayload) {
    ob::Seqlock<Payload>       s;
    std::atomic<bool>          stop{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> reads{0};

    std::thread writer([&] {
        for (std::uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
            s.store(Payload{i, i, i});
        }
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            Payload p{};
            while (!stop.load(std::memory_order_relaxed)) {
                if (s.try_load(p)) {
                    reads.fetch_add(1, std::memory_order_relaxed);
                    if (p.a != p.b || p.b != p.c) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (std::thread& t : readers) {
        t.join();
    }

    EXPECT_GT(reads.load(), 1000u) << "readers barely ran; the test proved nothing";
    EXPECT_EQ(torn.load(), 0u) << "a reader observed a torn payload";
}

TEST(Seqlock, BlockingLoadEventuallySucceedsUnderContention) {
    ob::Seqlock<Payload> s;
    std::atomic<bool>    stop{false};
    std::thread          writer([&] {
        for (std::uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
            s.store(Payload{i, i, i});
        }
    });
    Payload              p{};
    for (int i = 0; i < 1000; ++i) {
        s.load(p);
        ASSERT_EQ(p.a, p.c);
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
}

}  // namespace
