// bench/bench_latency.cpp
//
// Open-loop latency harness.
//
// Reports TWO distributions per scenario, because reporting one of them is how
// benchmarks mislead without lying:
//
//   service  = done - start     the engine's own cost
//   response = done - intended  what a client sees, including backlog
//
// The issue schedule is computed in advance and the driver does NOT wait for the
// previous command to finish before the next one becomes due. When the engine
// falls behind, `now > intended` and the queueing delay is counted. A closed-loop
// driver absorbs exactly that delay, which is coordinated omission.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ob/fast_engine.hpp>
#include <ob/reference_engine.hpp>
#include <ob/sanitizer.hpp>
#include <string>
#include <vector>

#include "alloc_counter.hpp"
#include "clock.hpp"
#include "histogram.hpp"
#include "scenarios.hpp"

namespace {

using namespace ob;
using namespace ob::bench;

constexpr std::size_t kWarmup = 100'000;  // discarded; the count is published

struct Options {
    std::size_t   ops            = 1'000'000;
    double        target_rate_hz = 0.0;  // 0 means "as fast as possible"
    std::uint64_t seed           = 20260922;
    bool          reference      = false;
    const char*   raw_dir        = nullptr;
};

// Busy-wait: sleeping would add scheduler latency far larger than what is being
// measured. This burns a core on purpose.
inline void spin_until(std::uint64_t deadline_ticks) {
    while (Clock::raw() < deadline_ticks) {
        // nothing
    }
}

template <class EngineT>
void run_scenario(Scenario sc, const Options& opt) {
    const auto stream = build(sc, opt.seed, opt.ops + kWarmup);

    // Event buffer sized from the scenario's measured worst case, with headroom.
    // worst_case_sweep legitimately emits hundreds of events for one command.
    const auto        shape   = measure_workload(build(sc, opt.seed, 20'000));
    const std::size_t buf_cap = shape.max_events_per_command * 8 + 1024;

    EngineT            engine{};
    std::vector<Event> storage(buf_cap);
    EventBuffer        buf(storage.data(), storage.size());

    Histogram service(opt.ops);
    Histogram response(opt.ops);
    double    clean_batched_ns = 0.0;

    // Warm-up: caches, branch predictor, and the first pass over the arena.
    for (std::size_t i = 0; i < kWarmup; ++i) {
        buf.clear();
        engine.submit(stream[i], buf);
        do_not_optimize(buf.size());
    }

    const Clock& clk = Clock::instance();
    const double period_ticks =
        opt.target_rate_hz > 0.0 ? static_cast<double>(clk.counter_hz()) / opt.target_rate_hz : 0.0;

    // CLEAN batched cost: one timestamp pair around a loop with NO per-operation
    // timestamping. This matters. Measuring the batched cost across the
    // distribution loop below would include that loop's own two serialized clock
    // reads, which cost ~9-19 ns each on this hardware. The first version of this
    // harness did exactly that and reported 66 ns/op where the engine costs ~30.
    // The gap between the two figures below IS the harness overhead, and it is
    // printed so a reader can see it rather than take the number on trust.
    {
        EngineT            warm{};
        std::vector<Event> s2(buf_cap);
        EventBuffer        b2(s2.data(), s2.size());
        for (std::size_t i = 0; i < kWarmup; ++i) {
            b2.clear();
            warm.submit(stream[i], b2);
            do_not_optimize(b2.size());
        }
        const std::uint64_t c0 = Clock::raw_serialized();
        for (std::size_t i = 0; i < opt.ops; ++i) {
            b2.clear();
            warm.submit(stream[kWarmup + i], b2);
            do_not_optimize(b2.size());
        }
        const std::uint64_t c1 = Clock::raw_serialized();
        clean_batched_ns = Clock::instance().ticks_to_ns(c1 - c0) / static_cast<double>(opt.ops);
    }

    reset_alloc_count();
    const std::uint64_t t_origin    = Clock::raw_serialized();
    const std::uint64_t batch_begin = t_origin;

    for (std::size_t i = 0; i < opt.ops; ++i) {
        std::uint64_t intended = 0;
        if (period_ticks > 0.0) {
            intended = t_origin + static_cast<std::uint64_t>(static_cast<double>(i) * period_ticks);
            spin_until(intended);
        }

        const std::uint64_t start = Clock::raw_serialized();
        buf.clear();
        engine.submit(stream[kWarmup + i], buf);
        do_not_optimize(buf.size());
        clobber_memory();
        const std::uint64_t done = Clock::raw_serialized();

        service.record(done - start);
        if (period_ticks > 0.0) {
            response.record(done > intended ? done - intended : 0);
        }
    }
    const std::uint64_t batch_end = Clock::raw_serialized();
    const std::size_t   allocs    = alloc_count();

    // Cost per op of the DISTRIBUTION loop, which includes this harness's own two
    // serialized clock reads. Reported next to clean_batched_ns so the difference
    // between them - the harness overhead - is visible instead of hidden.
    const double instrumented_ns =
        clk.ticks_to_ns(batch_end - batch_begin) / static_cast<double>(opt.ops);
    const double batched_ns = clean_batched_ns;

    const double oh = clk.overhead_ns_serialized();
    // Clamped at zero. When an operation completes inside a single clock tick the
    // raw delta is 0, and subtracting the clock's own overhead drives the corrected
    // value negative. A negative latency is not a measurement, it is an artifact of
    // measuring something faster than the clock can resolve, and printing it would
    // be worse than useless. The p50_below_clock_resolution flag and the
    // frac_below_clock_resolution figure are what tell the reader the low
    // percentiles are quantization rather than signal.
    const auto ns = [&](std::uint32_t ticks) {
        const double v = clk.ticks_to_ns(ticks) - oh;
        return v > 0.0 ? v : 0.0;
    };

    // A percentile below the clock's own resolution is measuring quantization, not
    // the engine. Rather than print it as though it meant something, count how many
    // samples land below the floor and flag it. On this hardware the floor is
    // ~41.67 ns and the median operation is faster than that, which is exactly why
    // batched_ns_per_op is the figure to trust for per-operation cost.
    const double floor_ns    = clk.resolution_ns();
    std::size_t  below_floor = 0;
    for (const std::uint32_t t : service.samples()) {
        if (clk.ticks_to_ns(t) < floor_ns) {
            ++below_floor;
        }
    }
    const double below_frac =
        static_cast<double>(below_floor) / static_cast<double>(service.count());
    const bool p50_unreliable = ns(service.percentile(50.0)) < floor_ns;

    std::printf(
        "{\"scenario\":\"%s\",\"engine\":\"%s\",\"ops\":%zu,\"warmup\":%zu,"
        "\"seed\":%llu,\"allocations\":%zu,\"saturated\":%zu,"
        "\"clock_resolution_ns\":%.4f,\"clock_overhead_ns\":%.4f,"
        "\"frac_below_clock_resolution\":%.4f,\"p50_below_clock_resolution\":%s,"
        "\"batched_ns_per_op\":%.2f,\"instrumented_ns_per_op\":%.2f,"
        "\"harness_overhead_ns_per_op\":%.2f,"
        "\"service_ns\":{\"p50\":%.1f,\"p90\":%.1f,\"p99\":%.1f,\"p99_9\":%.1f,"
        "\"p99_99\":%.1f,\"min\":%.1f,\"max\":%.1f}",
        name(sc), opt.reference ? "reference" : "fast", opt.ops, kWarmup,
        static_cast<unsigned long long>(opt.seed), allocs, service.saturated(), clk.resolution_ns(),
        oh, below_frac, p50_unreliable ? "true" : "false", batched_ns, instrumented_ns,
        instrumented_ns - batched_ns, ns(service.percentile(50.0)), ns(service.percentile(90.0)),
        ns(service.percentile(99.0)), ns(service.percentile(99.9)), ns(service.percentile(99.99)),
        ns(service.min()), ns(service.max()));

    if (period_ticks > 0.0) {
        std::printf(
            ",\"offered_rate_hz\":%.0f,\"response_ns\":{\"p50\":%.1f,"
            "\"p99\":%.1f,\"p99_9\":%.1f,\"max\":%.1f}",
            opt.target_rate_hz, ns(response.percentile(50.0)), ns(response.percentile(99.0)),
            ns(response.percentile(99.9)), ns(response.max()));
    }
    std::printf("}\n");

    // Binds on FastEngine ONLY. ReferenceEngine is supposed to allocate: std::map
    // plus std::list is 2 allocations per resting order, which is exactly what the
    // arena removes.
    if (!ob::kSanitizerBuild && !opt.reference && allocs != 0) {
        std::fprintf(stderr,
                     "FATAL: %zu allocations during the measured window for %s. "
                     "Every number above is contaminated.\n",
                     allocs, name(sc));
        std::exit(2);
    }
    if (p50_unreliable) {
        std::fprintf(stderr,
                     "NOTE: %s median (%.1f ns) is below the clock resolution "
                     "(%.1f ns), and %.1f%% of samples are. Those percentiles "
                     "measure quantization, not the engine. Use "
                     "batched_ns_per_op (%.2f ns) for per-operation cost.\n",
                     name(sc), ns(service.percentile(50.0)), floor_ns, below_frac * 100.0,
                     batched_ns);
    }
    if (service.saturated() != 0) {
        std::fprintf(stderr,
                     "WARNING: %zu samples exceeded uint32 ticks for %s; the tail "
                     "is clamped and this run should not be published.\n",
                     service.saturated(), name(sc));
    }

    if (opt.raw_dir != nullptr) {
        const std::string path = std::string(opt.raw_dir) + "/" + name(sc) + "_service.raw";
        service.write_raw(path.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            opt.ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            opt.target_rate_hz = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            opt.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--reference") == 0) {
            opt.reference = true;
        } else if (std::strcmp(argv[i], "--raw-dir") == 0 && i + 1 < argc) {
            opt.raw_dir = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: %s [--ops N] [--rate HZ] [--seed S] "
                         "[--reference] [--raw-dir DIR]\n",
                         argv[0]);
            return 1;
        }
    }

    Clock::instance().print_report(stderr);

    for (const Scenario sc : kAllScenarios) {
        if (opt.reference) {
            run_scenario<ReferenceEngine>(sc, opt);
        } else {
            run_scenario<FastEngine>(sc, opt);
        }
    }
    return 0;
}
