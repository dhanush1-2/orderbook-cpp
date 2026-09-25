// The test that justifies attaching a viewer to a latency-sensitive engine at all.
// Without it, "the writer never blocks" is a claim in a comment.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ob/fast_engine.hpp>
#include <ob/l2_snapshot.hpp>
#include <ob/sanitizer.hpp>
#include <ob/seqlock.hpp>
#include <thread>
#include <vector>

#include "model/scenario_gen.hpp"

namespace {

enum class Reader { None, Hot, StopsMidway };

// Returns ops/sec for the engine while publishing a snapshot per command.
double measure(Reader mode, std::size_t ops) {
    const auto stream = obtest::generate_stream(4242, ops, obtest::GenConfig{});

    ob::FastEngine              engine(ob::FastEngine::Config{1 << 16});
    std::vector<ob::Event>      storage(8192);
    ob::EventBuffer             buf(storage.data(), storage.size());
    ob::Seqlock<ob::L2Snapshot> lock;

    std::atomic<bool>          stop{false};
    std::atomic<bool>          reader_should_die{false};
    std::atomic<std::uint64_t> reads{0};
    std::thread                reader;

    if (mode != Reader::None) {
        reader = std::thread([&] {
            ob::L2Snapshot s{};
            while (!stop.load(std::memory_order_relaxed)) {
                if (reader_should_die.load(std::memory_order_relaxed)) {
                    return;  // vanishes mid-run, holding nothing
                }
                if (lock.try_load(s)) {
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    ob::L2Snapshot snap{};
    const auto     t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < stream.size(); ++i) {
        buf.clear();
        engine.submit(stream[i], buf);
        engine.snapshot_l2(snap);
        lock.store(snap);
        // Kill the reader a quarter of the way in, mid-publication.
        if (mode == Reader::StopsMidway && i == stream.size() / 4) {
            reader_should_die.store(true, std::memory_order_relaxed);
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    stop.store(true, std::memory_order_relaxed);
    if (reader.joinable()) {
        reader.join();
    }
    if (mode == Reader::Hot) {
        EXPECT_GT(reads.load(), ob::kSanitizerBuild ? 5u : 100u)
            << "the hot reader barely ran; the test proved nothing";
    }
    return static_cast<double>(ops) / secs;
}

// THE claim: a reader that stops entirely, possibly mid-publication, cannot slow the
// writer. If the writer took a lock the reader held, this is where it would hang or
// crater.
TEST(PublisherIsolation, AStalledReaderDoesNotSlowTheWriter) {
    constexpr std::size_t kOps = 400'000;
    const double          none = measure(Reader::None, kOps);
    const double          dead = measure(Reader::StopsMidway, kOps);
    const double          hot  = measure(Reader::Hot, kOps);

    std::printf("publisher isolation: no reader %.2f M ops/s, stalled %.2f, hot %.2f\n", none / 1e6,
                dead / 1e6, hot / 1e6);

    // THE correctness property - the run completed at all, with no hang and no
    // deadlock - holds everywhere. If the writer took a lock the reader held, these
    // measurements would never have returned.
    EXPECT_GT(none, 0.0);
    EXPECT_GT(dead, 0.0);
    EXPECT_GT(hot, 0.0);

    // The RATIOS are performance assertions and are skipped under a sanitizer.
    // TSan slows execution 20-50x and perturbs thread scheduling on purpose; a CI
    // runner also has fewer cores than the machine these bounds were chosen on. A
    // throughput ratio measured there describes the sanitizer, not the seqlock, and
    // asserting on it produces a flaky gate - which is worse than no gate, because
    // a flaky gate gets disabled.
    if constexpr (!ob::kSanitizerBuild) {
        // A reader that died must be indistinguishable from never having existed.
        // Generous bound because this runs on a shared machine, but a lock-based
        // publisher would fail this by orders of magnitude or hang outright.
        EXPECT_GT(dead, none * 0.6) << "a stalled reader slowed the writer";

        // A hot reader legitimately consumes a core, so some slowdown is expected
        // and fine. What must not happen is collapse.
        EXPECT_GT(hot, none * 0.3) << "a busy reader collapsed writer throughput";
    }
}

TEST(PublisherIsolation, PublicationCostIsBoundedAndSnapshotsStayConsistent) {
    constexpr std::size_t kOps   = 200'000;
    const auto            stream = obtest::generate_stream(77, kOps, obtest::GenConfig{});

    ob::FastEngine         engine(ob::FastEngine::Config{1 << 16});
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer        buf(storage.data(), storage.size());

    // Without publishing.
    auto t0 = std::chrono::steady_clock::now();
    for (const ob::Command& c : stream) {
        buf.clear();
        engine.submit(c, buf);
    }
    const double bare =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // With a snapshot published per command.
    engine.reset();
    ob::Seqlock<ob::L2Snapshot> lock;
    ob::L2Snapshot              snap{};
    t0 = std::chrono::steady_clock::now();
    for (const ob::Command& c : stream) {
        buf.clear();
        engine.submit(c, buf);
        engine.snapshot_l2(snap);
        lock.store(snap);
    }
    const double published =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("publication cost: bare %.3f s, publishing every command %.3f s (%.1fx)\n", bare,
                published, published / bare);

    // Publishing a 15-level snapshot per command is not free, but it must not be
    // catastrophic. Real feeds publish on change, not on every command. Skipped
    // under a sanitizer for the same reason as the ratios above.
    if constexpr (!ob::kSanitizerBuild) {
        EXPECT_LT(published, bare * 12.0) << "publication dominates the engine";
    }

    // And whatever was last published must be self-consistent.
    ob::L2Snapshot got{};
    ASSERT_TRUE(lock.try_load(got));
    if (got.bid_levels > 0 && got.ask_levels > 0) {
        EXPECT_LT(got.best_bid(), got.best_ask()) << "published a crossed book";
    }
    for (std::uint32_t i = 1; i < got.bid_levels; ++i) {
        EXPECT_LT(got.bids[i].price, got.bids[i - 1].price);
    }
    for (std::uint32_t i = 1; i < got.ask_levels; ++i) {
        EXPECT_GT(got.asks[i].price, got.asks[i - 1].price);
    }
}

}  // namespace
