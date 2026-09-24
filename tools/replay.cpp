// tools/replay.cpp
//
// Drives a scenario through FastEngine and publishes an L2 snapshot into a seqlock
// as it goes. With --tui, a second thread renders the depth ladder from whatever
// the newest published snapshot happens to be, dropping anything it missed.
//
// The rate limiter exists so a human can watch: unthrottled, the engine finishes
// two million orders before a single frame is drawn.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ob/fast_engine.hpp>
#include <ob/l2_snapshot.hpp>
#include <ob/seqlock.hpp>
#include <thread>
#include <vector>

#include "scenarios.hpp"
#include "tui.hpp"

namespace {

using namespace ob;
using namespace ob::bench;

struct Options {
    Scenario      scenario      = Scenario::MixedRealistic;
    std::size_t   ops           = 200'000;
    double        rate_hz       = 0.0;  // 0 = unthrottled
    std::uint64_t seed          = 20260922;
    bool          tui           = false;
    std::size_t   publish_every = 1;
};

bool parse_scenario(const char* s, Scenario& out) {
    for (const Scenario sc : kAllScenarios) {
        if (std::strcmp(s, name(sc)) == 0) {
            out = sc;
            return true;
        }
    }
    return false;
}

// Published state the render thread reads. The engine thread is the only writer.
struct Published {
    Seqlock<L2Snapshot>        book;
    std::atomic<std::uint64_t> commands{0};
    std::atomic<std::uint64_t> trades{0};
    std::atomic<double>        ops_per_sec{0.0};
    std::atomic<bool>          done{false};
};

void render_loop(const Published& pub) {
    ob::tui::Terminal term;
    L2Snapshot        snap{};
    while (!pub.done.load(std::memory_order_relaxed) &&
           !ob::tui::g_interrupted.load(std::memory_order_relaxed)) {
        // try_load, not load: if the writer is mid-update we would rather draw the
        // previous frame than spin. Dropping frames is the correct behaviour for a
        // viewer; blocking the writer would not be, and cannot happen here anyway.
        static_cast<void>(pub.book.try_load(snap));
        int rows = 24, cols = 80;
        ob::tui::Terminal::size(rows, cols);
        ob::tui::Terminal::draw(ob::tui::render(snap, rows, cols,
                                                pub.commands.load(std::memory_order_relaxed),
                                                pub.trades.load(std::memory_order_relaxed),
                                                pub.ops_per_sec.load(std::memory_order_relaxed)));
        std::this_thread::sleep_for(std::chrono::milliseconds(33));  // ~30 fps
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            if (!parse_scenario(argv[++i], opt.scenario)) {
                std::fprintf(stderr, "unknown scenario '%s'\n", argv[i]);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            opt.ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            opt.rate_hz = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            opt.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--publish-every") == 0 && i + 1 < argc) {
            opt.publish_every = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--tui") == 0) {
            opt.tui = true;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--scenario NAME] [--ops N] [--rate HZ] [--seed S]\n"
                         "          [--publish-every N] [--tui]\n",
                         argv[0]);
            return 1;
        }
    }
    if (opt.publish_every == 0) {
        opt.publish_every = 1;
    }

    const auto        stream  = build(opt.scenario, opt.seed, opt.ops);
    const auto        shape   = measure_workload(build(opt.scenario, opt.seed, 20'000));
    const std::size_t buf_cap = shape.max_events_per_command * 8 + 1024;

    FastEngine         engine;
    std::vector<Event> storage(buf_cap);
    EventBuffer        buf(storage.data(), storage.size());

    Published   pub;
    std::thread renderer;
    if (opt.tui) {
        renderer = std::thread(render_loop, std::cref(pub));
    }

    L2Snapshot snap{};
    const auto t0     = std::chrono::steady_clock::now();
    const auto period = opt.rate_hz > 0.0
                            ? std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / opt.rate_hz))
                            : std::chrono::nanoseconds(0);

    std::uint64_t commands = 0, trades = 0;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        if (ob::tui::g_interrupted.load(std::memory_order_relaxed)) {
            break;
        }
        if (period.count() > 0) {
            const auto due = t0 + period * static_cast<std::int64_t>(i);
            while (std::chrono::steady_clock::now() < due) {
                std::this_thread::yield();
            }
        }

        buf.clear();
        engine.submit(stream[i], buf);
        ++commands;
        for (const Event& e : buf) {
            if (e.type == EventType::Trade) {
                ++trades;
            }
        }

        if (i % opt.publish_every == 0) {
            engine.snapshot_l2(snap);
            pub.book.store(snap);
            pub.commands.store(commands, std::memory_order_relaxed);
            pub.trades.store(trades, std::memory_order_relaxed);
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            pub.ops_per_sec.store(secs > 0 ? static_cast<double>(commands) / secs : 0.0,
                                  std::memory_order_relaxed);
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    pub.done.store(true, std::memory_order_relaxed);
    if (renderer.joinable()) {
        renderer.join();
    }

    engine.snapshot_l2(snap);
    std::printf("scenario          %s\n", name(opt.scenario));
    std::printf("commands          %llu\n", static_cast<unsigned long long>(commands));
    std::printf("trades            %llu\n", static_cast<unsigned long long>(trades));
    std::printf("seconds           %.3f\n", secs);
    std::printf("ops/sec           %.0f\n", secs > 0 ? static_cast<double>(commands) / secs : 0.0);
    std::printf("resting orders    %zu\n", engine.live_order_count());
    std::printf("best bid / ask    %d / %d\n", snap.best_bid(), snap.best_ask());
    std::printf("depth published   %u bid levels, %u ask levels\n", snap.bid_levels,
                snap.ask_levels);
    const auto inv = engine.check_internal_invariants();
    std::printf("invariants        %s\n", inv.ok ? "hold" : inv.failure);
    return inv.ok ? 0 : 1;
}
