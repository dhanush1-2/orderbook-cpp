// bench/bench_throughput.cpp
//
// Saturation throughput: how many commands per second the engine sustains when
// nothing throttles it. Deliberately separate from the latency harness, because
// the two trade off against each other and reporting one number for both hides it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ob/fast_engine.hpp>
#include <ob/reference_engine.hpp>
#include <ob/sanitizer.hpp>
#include <type_traits>
#include <vector>

#include "alloc_counter.hpp"
#include "clock.hpp"
#include "scenarios.hpp"

namespace {

using namespace ob;
using namespace ob::bench;

// ReferenceEngine takes a plain size_t; FastEngine takes a Config.
template <class E>
auto make_config(std::size_t capacity) {
    if constexpr (std::is_same_v<E, FastEngine>) {
        return FastEngine::Config{capacity};
    } else {
        return capacity;
    }
}

constexpr std::size_t kWarmup = 100'000;

template <class EngineT>
void run(Scenario sc, std::size_t ops, std::uint64_t seed, bool reference, std::size_t capacity) {
    const auto        stream  = build(sc, seed, ops + kWarmup);
    const auto        shape   = measure_workload(build(sc, seed, 20'000));
    const std::size_t buf_cap = shape.max_events_per_command * 8 + 1024;

    // Capacity is a parameter so the memory-footprint hypothesis can be tested
    // directly: the id index is 16 bytes x next_pow2(2 * capacity), and SplitMix64
    // scatters sequential ids across all of it. If the engine is bound by that
    // footprint, shrinking capacity should speed it up measurably.
    EngineT            engine{make_config<EngineT>(capacity)};
    std::vector<Event> storage(buf_cap);
    EventBuffer        buf(storage.data(), storage.size());

    std::size_t events = 0;
    for (std::size_t i = 0; i < kWarmup; ++i) {
        buf.clear();
        engine.submit(stream[i], buf);
        do_not_optimize(buf.size());
    }

    reset_alloc_count();
    // ONE timestamp pair for the whole run: no per-operation clock cost, and the
    // 41.67 ns quantization is irrelevant across a multi-second interval.
    const std::uint64_t t0 = Clock::raw_serialized();
    for (std::size_t i = 0; i < ops; ++i) {
        buf.clear();
        engine.submit(stream[kWarmup + i], buf);
        events += buf.size();
        do_not_optimize(events);
    }
    const std::uint64_t t1     = Clock::raw_serialized();
    const std::size_t   allocs = alloc_count();

    const double secs = Clock::instance().ticks_to_ns(t1 - t0) / 1e9;
    std::printf(
        "{\"scenario\":\"%s\",\"engine\":\"%s\",\"ops\":%zu,"
        "\"seconds\":%.6f,\"ops_per_sec\":%.0f,\"events_per_sec\":%.0f,"
        "\"ns_per_op\":%.2f,\"allocations\":%zu,\"capacity\":%zu}\n",
        name(sc), reference ? "reference" : "fast", ops, secs, static_cast<double>(ops) / secs,
        static_cast<double>(events) / secs, secs * 1e9 / static_cast<double>(ops), allocs,
        capacity);

    // The zero-allocation assertion binds on FastEngine ONLY. ReferenceEngine is
    // SUPPOSED to allocate: it uses std::map and std::list, which is exactly 2
    // allocations per resting order (the map node and the list node). Reporting
    // that number is informative, because it is precisely what the arena removes.
    if (!ob::kSanitizerBuild && !reference && allocs != 0) {
        std::fprintf(stderr, "FATAL: %zu allocations during %s\n", allocs, name(sc));
        std::exit(2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t   ops       = 2'000'000;
    std::uint64_t seed      = 20260922;
    std::size_t   capacity  = 1'000'000;
    bool          reference = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--capacity") == 0 && i + 1 < argc) {
            capacity = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--reference") == 0) {
            reference = true;
        } else {
            std::fprintf(stderr, "usage: %s [--ops N] [--seed S] [--capacity N] [--reference]\n",
                         argv[0]);
            return 1;
        }
    }
    Clock::instance().print_report(stderr);

    for (const Scenario sc : kAllScenarios) {
        if (reference) {
            run<ReferenceEngine>(sc, ops, seed, true, capacity);
        } else {
            run<FastEngine>(sc, ops, seed, false, capacity);
        }
    }
    return 0;
}
