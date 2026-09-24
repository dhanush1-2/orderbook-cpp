#pragma once

// Deterministic command-stream generator and a delta-debugging shrinker.
//
// The PRNG is hand-rolled on purpose: std::uniform_int_distribution is NOT
// specified to produce identical values across standard library implementations,
// so a golden file generated with it would differ between libstdc++ and libc++.
// A golden file that changes with the toolchain is worse than no golden file.

#include <ob/command.hpp>
#include <ob/engine_concept.hpp>
#include <ob/events.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace obtest {

class Xoshiro256ss {
public:
    explicit Xoshiro256ss(std::uint64_t seed) {
        // SplitMix64 to spread a single seed across the 256-bit state.
        for (std::uint64_t& w : s_) {
            seed += 0x9E37'79B9'7F4A'7C15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
            w = z ^ (z >> 31);
        }
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Unbiased enough for test generation, and identical on every platform.
    std::uint64_t bounded(std::uint64_t n) { return n == 0 ? 0 : next() % n; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
    std::uint64_t s_[4]{};
};

struct GenConfig {
    ob::Ticks     centre       = 10000;  // prices cluster around here
    ob::Ticks     half_width   = 25;     // +/- this many ticks
    ob::Qty       max_qty      = 100;
    std::uint64_t cancel_pct   = 30;     // share of commands that are cancels
    std::uint64_t market_pct   = 3;      // of New commands
    std::uint64_t ioc_pct      = 5;
    std::uint64_t fok_pct      = 3;
    std::uint64_t postonly_pct = 5;
};

// Generates `n` commands. Cancels always target an id the generator previously
// issued as a resting type, which is what makes the stream exercise the cancel
// path instead of producing a flood of UnknownOrderId rejections.
inline std::vector<ob::Command> generate_stream(std::uint64_t seed, std::size_t n,
                                               const GenConfig& cfg) {
    Xoshiro256ss rng(seed);
    std::vector<ob::Command> out;
    out.reserve(n);

    std::vector<ob::OrderId> cancellable;
    ob::OrderId next_id = 1;  // strictly increasing, as spec E48 requires

    for (std::size_t i = 0; i < n; ++i) {
        const bool do_cancel = !cancellable.empty() && rng.bounded(100) < cfg.cancel_pct;

        if (do_cancel) {
            const std::size_t k = static_cast<std::size_t>(rng.bounded(cancellable.size()));
            out.push_back(ob::make_cancel(cancellable[k]));
            cancellable.erase(cancellable.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }

        const ob::Side side = (rng.bounded(2) == 0) ? ob::Side::Buy : ob::Side::Sell;
        const std::uint64_t roll = rng.bounded(100);
        ob::OrderType type = ob::OrderType::Limit;
        if (roll < cfg.market_pct) {
            type = ob::OrderType::Market;
        } else if (roll < cfg.market_pct + cfg.ioc_pct) {
            type = ob::OrderType::Ioc;
        } else if (roll < cfg.market_pct + cfg.ioc_pct + cfg.fok_pct) {
            type = ob::OrderType::Fok;
        } else if (roll < cfg.market_pct + cfg.ioc_pct + cfg.fok_pct + cfg.postonly_pct) {
            type = ob::OrderType::PostOnly;
        }

        const ob::Ticks span = cfg.half_width * 2 + 1;
        const ob::Ticks px =
            cfg.centre - cfg.half_width +
            static_cast<ob::Ticks>(rng.bounded(static_cast<std::uint64_t>(span)));
        const ob::Qty qty =
            1 + static_cast<ob::Qty>(rng.bounded(static_cast<std::uint64_t>(cfg.max_qty)));

        out.push_back(ob::make_new(next_id, side, type, px, qty));
        if (type == ob::OrderType::Limit || type == ob::OrderType::PostOnly) {
            cancellable.push_back(next_id);
        }
        ++next_id;
    }
    return out;
}

// Runs a stream through a fresh engine and returns every event it produced.
template <class E>
std::vector<ob::Event> run_stream(const std::vector<ob::Command>& cmds) {
    E engine;
    std::vector<ob::Event> all;
    all.reserve(cmds.size() * 2);

    // Sized for the worst single-command sweep in a generated stream.
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    for (const ob::Command& c : cmds) {
        buf.clear();
        engine.submit(c, buf);
        all.insert(all.end(), buf.begin(), buf.end());
    }
    return all;
}

// Delta debugging. Repeatedly tries removing chunks, keeping any removal that
// preserves the failure, halving the chunk size when a pass makes no progress.
// A differential failure at operation 4,000,000 is unusable; the same failure
// reduced to six commands is a test you commit.
inline std::vector<ob::Command> shrink(
    std::vector<ob::Command> stream,
    const std::function<bool(const std::vector<ob::Command>&)>& still_fails) {
    std::size_t chunk = stream.size() / 2;
    while (chunk >= 1) {
        bool progressed = false;
        std::size_t i = 0;
        while (i < stream.size()) {
            const std::size_t take = std::min(chunk, stream.size() - i);
            std::vector<ob::Command> candidate;
            candidate.reserve(stream.size() - take);
            candidate.insert(candidate.end(), stream.begin(),
                             stream.begin() + static_cast<std::ptrdiff_t>(i));
            candidate.insert(candidate.end(),
                             stream.begin() + static_cast<std::ptrdiff_t>(i + take),
                             stream.end());
            if (!candidate.empty() && still_fails(candidate)) {
                stream = std::move(candidate);
                progressed = true;
            } else {
                i += take;
            }
        }
        if (!progressed) {
            if (chunk == 1) {
                break;
            }
            chunk /= 2;
        }
    }
    return stream;
}

}  // namespace obtest
