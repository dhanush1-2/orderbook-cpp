#include "model/scenario_gen.hpp"

#include <ob/fast_engine.hpp>
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kNoDivergence = static_cast<std::size_t>(-1);

const char* type_name(ob::OrderType t) {
    switch (t) {
        case ob::OrderType::Limit:    return "Limit";
        case ob::OrderType::Market:   return "Market";
        case ob::OrderType::Ioc:      return "Ioc";
        case ob::OrderType::Fok:      return "Fok";
        case ob::OrderType::PostOnly: return "PostOnly";
    }
    return "?";
}

// Runs a stream through both engines and returns the index of the first differing
// event, or kNoDivergence when they agree completely.
std::size_t first_divergence(const std::vector<ob::Command>& stream,
                            std::string* detail = nullptr) {
    constexpr std::size_t kCap = 64 * 1024;
    ob::ReferenceEngine ref(kCap);
    ob::FastEngine fast(ob::FastEngine::Config{kCap});

    std::vector<ob::Event> rs(1 << 17), fs(1 << 17);
    ob::EventBuffer rb(rs.data(), rs.size());
    ob::EventBuffer fb(fs.data(), fs.size());

    std::size_t event_index = 0;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        rb.clear();
        fb.clear();
        ref.submit(stream[i], rb);
        fast.submit(stream[i], fb);

        const std::size_t n = std::min(rb.size(), fb.size());
        for (std::size_t k = 0; k < n; ++k) {
            if (!(rb[k] == fb[k])) {
                if (detail != nullptr) {
                    std::ostringstream os;
                    os << "command " << i << ", event " << k << "\n  reference: " << rb[k]
                       << "\n  fast:      " << fb[k];
                    *detail = os.str();
                }
                return event_index + k;
            }
        }
        if (rb.size() != fb.size()) {
            if (detail != nullptr) {
                std::ostringstream os;
                os << "command " << i << ": reference emitted " << rb.size()
                   << " events, fast emitted " << fb.size();
                *detail = os.str();
            }
            return event_index + n;
        }
        event_index += rb.size();

        // Book state must agree too, not just the event stream. A divergence here
        // that the events do not show would surface much later and far away.
        if (ref.best_bid() != fast.best_bid() || ref.best_ask() != fast.best_ask() ||
            ref.live_order_count() != fast.live_order_count()) {
            if (detail != nullptr) {
                std::ostringstream os;
                os << "command " << i << ": book state diverged\n  reference: bid="
                   << ref.best_bid() << " ask=" << ref.best_ask()
                   << " live=" << ref.live_order_count() << "\n  fast:      bid="
                   << fast.best_bid() << " ask=" << fast.best_ask()
                   << " live=" << fast.live_order_count();
                *detail = os.str();
            }
            return event_index;
        }
    }
    return kNoDivergence;
}

bool diverges(const std::vector<ob::Command>& stream) {
    return first_divergence(stream) != kNoDivergence;
}

// Prints a minimal reproducer as compilable C++ so a divergence becomes a
// committed regression test in about a minute.
std::string as_cpp(const std::vector<ob::Command>& s) {
    std::ostringstream os;
    os << "\n// Minimal reproducer, paste into tests/cases/edge_cases.hpp:\n";
    for (const ob::Command& c : s) {
        if (c.type == ob::CommandType::Cancel) {
            os << "  cxl(" << c.id << "),\n";
        } else {
            os << "  " << (c.side == ob::Side::Buy ? "buy(" : "sell(") << c.id << ", "
               << c.price << ", " << c.qty << ", OrderType::" << type_name(c.order_type)
               << "),\n";
        }
    }
    return os.str();
}

// Seeds derived from the commit so coverage ACCUMULATES across commits rather
// than re-running the same streams forever. OB_DIFF_SEED_BASE is set by CI; the
// default keeps local runs reproducible.
std::uint64_t seed_base() {
    if (const char* env = std::getenv("OB_DIFF_SEED_BASE"); env != nullptr) {
        return std::strtoull(env, nullptr, 10);
    }
    return 20260922;
}

// Scales with OB_DIFF_OPS so a local run is seconds and CI is 10^7 operations.
std::size_t ops_target() {
    if (const char* env = std::getenv("OB_DIFF_OPS"); env != nullptr) {
        return std::strtoull(env, nullptr, 10);
    }
    return 200'000;
}

TEST(Differential, EnginesAgreeOnGeneratedStreams) {
    const std::uint64_t base = seed_base();
    const std::size_t target = ops_target();
    constexpr std::size_t kPerStream = 20'000;
    const std::size_t streams = std::max<std::size_t>(1, target / kPerStream);

    std::printf("differential: %zu streams x %zu ops, seed base %llu\n", streams,
                kPerStream, static_cast<unsigned long long>(base));

    for (std::size_t k = 0; k < streams; ++k) {
        const std::uint64_t seed = base + k;
        const auto stream = obtest::generate_stream(seed, kPerStream, obtest::GenConfig{});

        std::string detail;
        const std::size_t at = first_divergence(stream, &detail);
        if (at == kNoDivergence) {
            continue;
        }

        // Shrink before reporting. An unshrunk failure is not actionable.
        const auto minimal = obtest::shrink(stream, diverges);
        std::string min_detail;
        first_divergence(minimal, &min_detail);

        FAIL() << "engines diverged, seed " << seed << ", at event " << at << "\n"
               << detail << "\n\nshrunk from " << stream.size() << " to "
               << minimal.size() << " commands:\n"
               << min_detail << as_cpp(minimal);
    }
}

// Narrow books force deep sweeps and constant level emptying, which is where the
// bitmap hierarchy and the best-price lookup are most likely to disagree.
TEST(Differential, EnginesAgreeOnNarrowBooksWithDeepSweeps) {
    obtest::GenConfig cfg;
    cfg.half_width = 3;  // only 7 price levels: everything crosses
    cfg.max_qty = 500;
    cfg.cancel_pct = 15;

    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const auto stream = obtest::generate_stream(seed_base() + seed, 20'000, cfg);
        std::string detail;
        ASSERT_EQ(first_divergence(stream, &detail), kNoDivergence)
            << "seed " << seed << "\n" << detail;
    }
}

// The realistic shape: mostly cancels, wide book, little crossing.
TEST(Differential, EnginesAgreeOnCancelHeavyWideBooks) {
    obtest::GenConfig cfg;
    cfg.half_width = 400;
    cfg.cancel_pct = 88;

    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const auto stream = obtest::generate_stream(seed_base() + 1000 + seed, 20'000, cfg);
        std::string detail;
        ASSERT_EQ(first_divergence(stream, &detail), kNoDivergence)
            << "seed " << seed << "\n" << detail;
    }
}

// Both ladder extremes, where the bitmap's boundary masks are exercised.
TEST(Differential, EnginesAgreeAtLadderExtremes) {
    obtest::GenConfig low;
    low.centre = ob::kMinTick + 5;
    low.half_width = 5;
    obtest::GenConfig high;
    high.centre = ob::kMaxTick - 5;
    high.half_width = 5;

    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        for (const obtest::GenConfig& cfg : {low, high}) {
            const auto stream = obtest::generate_stream(seed_base() + 2000 + seed, 10'000, cfg);
            std::string detail;
            ASSERT_EQ(first_divergence(stream, &detail), kNoDivergence)
                << "seed " << seed << "\n" << detail;
        }
    }
}

// The shrinker has to work for the above failures to be actionable.
TEST(Differential, ShrinkerReducesAnInjectedDivergence) {
    const auto stream = obtest::generate_stream(77, 5000, obtest::GenConfig{});
    const auto pred = [](const std::vector<ob::Command>& s) {
        for (const ob::Command& c : s) {
            if (c.type == ob::CommandType::Cancel) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(pred(stream));
    const auto minimal = obtest::shrink(stream, pred);
    EXPECT_TRUE(pred(minimal));
    EXPECT_LE(minimal.size(), 2u);
}

}  // namespace
