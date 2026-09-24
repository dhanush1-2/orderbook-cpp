#include "model/scenario_gen.hpp"

#include <ob/invariants.hpp>
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

namespace {

// E43: the same input replayed gives a byte-identical event stream.
TEST(Determinism, ReplayingTheSameStreamProducesIdenticalEvents) {
    const auto stream = obtest::generate_stream(20260922, 20000, obtest::GenConfig{});
    const auto a = obtest::run_stream<ob::ReferenceEngine>(stream);
    const auto b = obtest::run_stream<ob::ReferenceEngine>(stream);

    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i], b[i]) << "divergence at event " << i;
    }
}

TEST(Determinism, ResetMakesAnEngineIndistinguishableFromAFreshOne) {
    const auto stream = obtest::generate_stream(7, 2000, obtest::GenConfig{});

    ob::ReferenceEngine reused;
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());
    for (const ob::Command& c : stream) {
        buf.clear();
        reused.submit(c, buf);
    }
    reused.reset();

    std::vector<ob::Event> after_reset;
    for (const ob::Command& c : stream) {
        buf.clear();
        reused.submit(c, buf);
        after_reset.insert(after_reset.end(), buf.begin(), buf.end());
    }
    const auto fresh = obtest::run_stream<ob::ReferenceEngine>(stream);

    ASSERT_EQ(after_reset.size(), fresh.size());
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        ASSERT_EQ(after_reset[i], fresh[i]) << "divergence at event " << i;
    }
}

// E44 in part: a fingerprint of the event stream, printed so CI can compare it
// across compilers and optimization levels (Task 13 wires up that comparison).
TEST(Determinism, EventStreamFingerprintIsStable) {
    const auto stream = obtest::generate_stream(20260922, 50000, obtest::GenConfig{});
    const auto events = obtest::run_stream<ob::ReferenceEngine>(stream);

    // FNV-1a over the meaningful fields. Not cryptographic; just a stable digest.
    std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
    const auto mix = [&h](std::uint64_t v) {
        for (int b = 0; b < 8; ++b) {
            h ^= (v >> (b * 8)) & 0xFF;
            h *= 0x0000'0100'0000'01B3ULL;
        }
    };
    for (const ob::Event& e : events) {
        mix(e.seq);
        mix(static_cast<std::uint64_t>(e.type));
        mix(e.order_id);
        mix(e.maker_id);
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.price)));
        mix(e.qty);
        mix(static_cast<std::uint64_t>(e.reject));
        mix(static_cast<std::uint64_t>(e.cancel));
    }

    std::printf("FINGERPRINT events=%zu digest=%016llx\n", events.size(),
                static_cast<unsigned long long>(h));
    EXPECT_GT(events.size(), 50000u);
    // The digest is compared ACROSS builds by CI, not pinned here: pinning it in
    // source would just encode one toolchain's result as if it were the spec.
}

}  // namespace
