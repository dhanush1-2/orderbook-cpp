// bench/clock.hpp
#pragma once

// Timestamp source for the benchmark harness.
//
// MEASURED FACTS this file is built around (2026-09-22, Apple M4 Pro):
//   * the finest timestamp granularity available is 41.6667 ns
//   * CNTVCT_EL0 costs ~0.32 ns amortized; clock_gettime_nsec_np costs ~11.52 ns
//   * on macOS, CNTFRQ_EL0 reports 1 GHz, which is fiction: the same register read
//     inside a Linux VM on the same silicon reports the true 24 MHz. Converting
//     through CNTFRQ_EL0 is nevertheless correct on BOTH platforms, because macOS
//     scales the counter into nanosecond units even though it only advances every
//     41.67 ns. That is why this file never hardcodes a frequency.

#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#if defined(__aarch64__)
#define OB_ARCH_ARM64 1
#elif defined(__x86_64__)
#define OB_ARCH_X86_64 1
#include <x86intrin.h>
#else
#error "bench/clock.hpp supports arm64 and x86-64 only"
#endif

namespace ob::bench {

// Stops the optimizer deleting work whose result is never read.
template <class T>
[[gnu::always_inline]] inline void do_not_optimize(const T& value) noexcept {
    asm volatile("" : : "r,m"(value) : "memory");
}

[[gnu::always_inline]] inline void clobber_memory() noexcept {
    asm volatile("" : : : "memory");
}

class Clock {
public:
    // One instruction, NOT serialized: this read can float across surrounding
    // work. Correct for throughput loops, wrong for measuring one operation.
    [[gnu::always_inline]] static inline std::uint64_t raw() noexcept {
#if defined(OB_ARCH_ARM64)
        std::uint64_t v;
        asm volatile("mrs %0, cntvct_el0" : "=r"(v));
        return v;
#else
        return __rdtsc();
#endif
    }

    // Ordered against surrounding work. This is what a latency measurement uses.
    [[gnu::always_inline]] static inline std::uint64_t raw_serialized() noexcept {
#if defined(OB_ARCH_ARM64)
        std::uint64_t v;
        asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
        return v;
#else
        std::uint32_t       aux;
        const std::uint64_t v = __rdtscp(&aux);  // waits for earlier instructions
        _mm_lfence();                            // and blocks later ones
        return v;
#endif
    }

    static const Clock& instance() {
        static const Clock c;
        return c;
    }

    [[nodiscard]] std::uint64_t counter_hz() const noexcept { return hz_; }
    [[nodiscard]] double        ns_per_tick() const noexcept { return ns_per_tick_; }

    [[nodiscard]] double ticks_to_ns(std::uint64_t ticks) const noexcept {
        return static_cast<double>(ticks) * ns_per_tick_;
    }

    // MINIMUM observed delta. Outlier-sensitive and therefore optimistic: do not
    // quote it as "the resolution".
    [[nodiscard]] std::uint64_t resolution_min_ticks() const noexcept { return res_min_; }
    [[nodiscard]] double        resolution_min_ns() const noexcept { return ticks_to_ns(res_min_); }

    // MEDIAN observed delta. This is the practical floor and the figure that goes
    // next to every published percentile. On this hardware min is ~17 ns while the
    // median is ~42 ns, so quoting the minimum would understate the harness's own
    // error bars by more than 2x.
    [[nodiscard]] std::uint64_t resolution_ticks() const noexcept { return res_median_; }
    [[nodiscard]] double        resolution_ns() const noexcept { return ticks_to_ns(res_median_); }
    [[nodiscard]] double        overhead_ns_raw() const noexcept { return oh_raw_ns_; }
    [[nodiscard]] double        overhead_ns_serialized() const noexcept { return oh_ser_ns_; }

    void print_report(std::FILE* out) const {
        std::fprintf(out,
                     "clock: hz=%llu ns_per_tick=%.6f resolution_median=%.3f ns "
                     "resolution_min=%.3f ns overhead_raw=%.3f ns "
                     "overhead_serialized=%.3f ns\n",
                     static_cast<unsigned long long>(hz_), ns_per_tick_, resolution_ns(),
                     resolution_min_ns(), oh_raw_ns_, oh_ser_ns_);
    }

private:
    Clock() {
        hz_          = detect_hz();
        ns_per_tick_ = 1e9 / static_cast<double>(hz_);
        measure_resolution(res_min_, res_median_);
        oh_raw_ns_ = measure_overhead_raw_ns();
        oh_ser_ns_ = measure_overhead_serialized_ns();
    }

    static std::uint64_t detect_hz() {
#if defined(OB_ARCH_ARM64)
        std::uint64_t f;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
        return f;
#else
        // x86 has no frequency register, so calibrate against CLOCK_MONOTONIC.
        // 200 ms is long enough that the ~10 ns clock_gettime cost is noise.
        timespec a{}, b{};
        clock_gettime(CLOCK_MONOTONIC, &a);
        const std::uint64_t t0 = raw_serialized();
        const timespec      sleep_for{0, 200'000'000};
        nanosleep(&sleep_for, nullptr);
        const std::uint64_t t1 = raw_serialized();
        clock_gettime(CLOCK_MONOTONIC, &b);

        const double elapsed_ns = static_cast<double>(b.tv_sec - a.tv_sec) * 1e9 +
                                  static_cast<double>(b.tv_nsec - a.tv_nsec);
        return static_cast<std::uint64_t>(static_cast<double>(t1 - t0) * 1e9 / elapsed_ns);
#endif
    }

    // Distribution of the nonzero difference between two consecutive reads.
    //
    // Uses raw_serialized(), NOT raw(). Measuring with the unserialized read is
    // unsound: two bare `mrs` instructions can complete out of order relative to
    // each other, so the delta does not reflect elapsed time. That produced a
    // resolution that swung between 1 ns and 42 ns across runs of the same binary.
    // The measurement must use the same read the harness will use for timing.
    //
    // Reports BOTH the minimum and the median. The minimum is outlier-sensitive and
    // therefore optimistic; the median is what gets published. A harness quoting its
    // own resolution as the minimum would understate its error bars, which is
    // precisely the flattering-yourself mistake the methodology doc exists to stop.
    static void measure_resolution(std::uint64_t& out_min, std::uint64_t& out_median) {
        std::vector<std::uint64_t> deltas;
        deltas.reserve(200000);
        for (int i = 0; i < 200000; ++i) {
            const std::uint64_t a = raw_serialized();
            const std::uint64_t b = raw_serialized();
            if (b > a) {
                deltas.push_back(b - a);
            }
        }
        if (deltas.empty()) {
            out_min    = 1;
            out_median = 1;
            return;
        }
        std::sort(deltas.begin(), deltas.end());
        out_min    = deltas.front();
        out_median = deltas[deltas.size() / 2];
    }

    // Amortized cost of an unserialized read. This measures THROUGHPUT, not
    // latency: consecutive reads pipeline, so the number is lower than the cost
    // of one isolated read. Reported as-is, labelled as such.
    double measure_overhead_raw_ns() const {
        constexpr int       kN  = 200000;
        const std::uint64_t t0  = raw_serialized();
        std::uint64_t       acc = 0;
        for (int i = 0; i < kN; ++i) {
            acc += raw();
        }
        const std::uint64_t t1 = raw_serialized();
        do_not_optimize(acc);
        return ticks_to_ns(t1 - t0) / kN;
    }

    // Cost of a serialized read, which cannot pipeline. This is the number that
    // gets subtracted from latency measurements.
    double measure_overhead_serialized_ns() const {
        constexpr int       kN  = 200000;
        const std::uint64_t t0  = raw_serialized();
        std::uint64_t       acc = 0;
        for (int i = 0; i < kN; ++i) {
            acc += raw_serialized();
        }
        const std::uint64_t t1 = raw_serialized();
        do_not_optimize(acc);
        return ticks_to_ns(t1 - t0) / kN;
    }

    std::uint64_t hz_          = 0;
    double        ns_per_tick_ = 0.0;
    std::uint64_t res_min_     = 0;
    std::uint64_t res_median_  = 0;
    double        oh_raw_ns_   = 0.0;
    double        oh_ser_ns_   = 0.0;
};

}  // namespace ob::bench
