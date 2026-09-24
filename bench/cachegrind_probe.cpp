// bench/cachegrind_probe.cpp
//
// Fixed work per scenario, for deterministic instruction counting. Deliberately
// small: Cachegrind is roughly 50x slower than native, and determinism is what
// matters here, not scale.
//
// No timing and no clock calls at all. A clock read would make the instruction
// count depend on how many times a spin loop ran, which is the one thing that must
// not vary.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ob/fast_engine.hpp>
#include <vector>

#include "scenarios.hpp"

int main(int argc, char** argv) {
    using namespace ob;
    using namespace ob::bench;

    // TWO overheads had to be pushed out of this count before the gate could
    // measure the engine, and both were found by looking at the per-op figure and
    // not believing it:
    //
    // 1. Stream generation is O(ops) and, for cancel_heavy and mixed_realistic,
    //    runs a whole shadow engine. Generated once, before the counted loop.
    // 2. FastEngine construction value-initializes its pool, ladders and index.
    //    At the default 1M capacity that is ~69 MB of stores PER CONSTRUCTION. An
    //    earlier version built a fresh engine per repeat, 20 times, so the count
    //    was dominated by constructors: ~1540 instructions per "op" for an
    //    operation that costs ~30 ns. Now: one engine, capacity sized to the
    //    workload, amortized over many operations.
    std::size_t ops      = 200'000;
    std::size_t capacity = 1u << 18;
    const char* only     = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--capacity") == 0 && i + 1 < argc) {
            capacity = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            only = argv[++i];
        }
    }

    for (const Scenario sc : kAllScenarios) {
        if (only != nullptr && std::strcmp(only, name(sc)) != 0) {
            continue;
        }
        // Generated ONCE, outside the loop that dominates the count.
        const auto stream = build(sc, 20260922, ops);
        const auto shape  = measure_workload(build(sc, 20260922, 5'000));

        std::vector<Event> storage(shape.max_events_per_command * 8 + 1024);
        EventBuffer        buf(storage.data(), storage.size());

        FastEngine  engine(FastEngine::Config{capacity});
        std::size_t events = 0;
        for (const Command& c : stream) {
            buf.clear();
            engine.submit(c, buf);
            events += buf.size();
        }
        std::printf("%s ops=%zu events=%zu\n", name(sc), stream.size(), events);
    }
    return 0;
}
