# Phase 2: Fast Engine and the Measurement Story — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `FastEngine` on flat, allocation-free data structures; prove it behaves identically to `ReferenceEngine` over more than 10^7 generated operations plus coverage-guided fuzzing; then measure it honestly, profile it, optimize what the profile says, and gate the result against regression.

**Architecture:** Flat price ladder indexed by tick, with a three-level occupancy bitmap so best-price lookup is two or three `ctz` instructions instead of a tree walk. Orders live in a pre-allocated arena, linked into per-level FIFO queues by 32-bit indices rather than pointers. Cancel is O(1) through an open-addressed `OrderId -> Slot` index that never rehashes. Nothing allocates, throws, or makes a syscall after construction. The measurement harness is purpose-built because the alternatives report means rather than tail percentiles and are closed-loop by construction.

**Tech Stack:** C++20, CMake >= 3.25 + Ninja, GoogleTest v1.15.2 (test-only), libFuzzer (clang), Valgrind/Cachegrind and `perf` via Docker, GitHub Actions.

**Spec:** [`docs/superpowers/specs/2026-09-22-order-book-matching-engine-design.md`](../specs/2026-09-22-order-book-matching-engine-design.md)

**Prerequisite:** Phase 1 complete, including its Task 13 Step 2 check that two optimization levels produce the same event-stream fingerprint. **Do not start this plan until that prints `1`.** Optimizing an engine whose behavior already depends on optimization level wastes the entire differential-testing apparatus.

## Global Constraints

See [`README.md`](README.md). The ones this plan lives and dies by:

- **`FastEngine` and everything it touches: no allocation, no exceptions, no syscalls, no virtual dispatch, no `std::map`/`std::list`/`std::unordered_map`/`new`/`malloc` after construction.** One of these is enforced by a test that counts through a replaced `operator new`.
- `static_assert(sizeof(Order) == 32)` and `static_assert(sizeof(PriceLevel) == 24)`.
- Cache-line padding uses `std::hardware_destructive_interference_size`, never a hardcoded 64. **This machine's line is 128 bytes.**
- **Validation order is fixed** and identical to `ReferenceEngine`: quantity, price range, duplicate ID (`id <= high_water_`), capacity, PostOnly-would-cross.
- **The event ordering contract from Phase 1 is binding.** Byte-identical output to `ReferenceEngine` is the acceptance criterion for every task from Task 7 onward.
- **Per-operation cost is measured in batches; distributions state the 41.6667 ns resolution floor.** Never convert ticks to nanoseconds inside a measured region.
- **The CI performance gate is Cachegrind instruction count, threshold 2%.** Never wall-clock.
- Conventional Commits. Nothing pushed without explicit approval.

## Measured platform facts this plan depends on

Verified on 2026-09-22, not assumed. Re-verify if the machine changes.

| Fact | Value | Where it bites |
|---|---|---|
| Timestamp granularity, macOS host | **41.6667 ns** (`mach_timebase` 125/3) | Task 1, Task 11 |
| `CNTFRQ_EL0`, macOS | Reports 1 GHz, which is fiction; counter advances every 41.67 ns | Task 1: convert via `CNTFRQ_EL0` and it is correct anyway |
| `CNTFRQ_EL0`, Docker aarch64 | Reports the true **24 MHz** = 41.6667 ns/tick | Task 1 self-test, Task 13 |
| `CNTVCT_EL0` read cost | 0.32 ns amortized, versus 11.52 ns for `clock_gettime_nsec_np` | Task 1: `CNTVCT_EL0` is the only viable source |
| Hardware PMU | **Absent everywhere**: host, Docker VM, GitHub runners | Task 13: no real cache-miss counts. Cachegrind simulation instead |
| `perf` in Docker | **Works.** perf 6.6.31, `perf record -F 999 -e cpu-clock -g` verified | Task 13: local flamegraphs |
| Cachegrind in Docker | Valgrind 3.23.0 on aarch64. Two runs of one binary: 1,842,733 vs 1,842,734 I refs | Task 15: determinism to ~1 part in 2e6, so a 2% gate has ~40,000x margin |
| Cache line / L1d / L2 | 128 B / 64 KB / 4 MB | Task 6: ladder sized to stay in L2 |

## Pre-verified code in this plan

The two pieces with real algorithmic subtlety were **compiled and tested before this
plan shipped**, rather than being written out and hoped over:

| Piece | Verification | Result |
|---|---|---|
| `LevelBitmap` (Task 5) | Every boundary case in the task's test, plus 80,000 randomized set/clear/query operations against a `std::set` model across 20 seeds | Passed |
| `IdIndex` backward-shift deletion (Task 4) | The collision-chain test, plus 400,000 randomized insert/erase operations against `std::unordered_map` across 20 seeds | Passed |
| Both, under ASan + UBSan | `-fsanitize=undefined,address -fno-sanitize-recover=all` | Clean, so the shift-by-64 mask guards are correct |

Practical consequence for whoever executes this: **if `LevelBitmap` or `IdIndex` tests
fail, the cause is a transcription error, not a flaw in the algorithm.** Diff against
the plan before debugging the logic. Nothing else in this plan carries that guarantee.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `bench/clock.hpp` | Timestamp source, `CNTFRQ`-based conversion, self-measured resolution and overhead, optimizer barriers | 1 |
| `bench/histogram.hpp` | Exact-percentile recorder over raw samples, nearest-rank, saturation accounting | 2 |
| `include/ob/order.hpp` | The 32-byte `Order` slot | 3 |
| `include/ob/order_pool.hpp` | Arena with an index free list threaded through `Order::next` | 3 |
| `include/ob/id_index.hpp` | Open-addressed `OrderId -> Slot`, linear probing, backward-shift deletion, never rehashes | 4 |
| `include/ob/level_bitmap.hpp` | Three-level occupancy bitmap over 65,536 levels | 5 |
| `include/ob/price_ladder.hpp` | Flat level array plus the bitmap; per-level FIFO operations | 6 |
| `include/ob/fast_engine.hpp` | The matcher, plus `check_internal_invariants()` | 7 |
| `tests/test_differential.cpp` | Reference vs Fast over generated streams, with the shrinker wired in | 8 |
| `fuzz/fuzz_differential.cpp` | libFuzzer target: decode input as commands, run both engines, assert equality | 9 |
| `bench/scenarios.hpp` | The six workloads plus counters that prove each generated what it claimed | 10 |
| `bench/bench_latency.cpp` | Open-loop latency driver | 11 |
| `bench/bench_throughput.cpp` | Saturation throughput, median-of-5 | 12 |
| `bench/cachegrind_probe.cpp` | Fixed-work binary for deterministic instruction counting | 15 |
| `scripts/profile.sh`, `scripts/cachegrind.sh`, `scripts/run_bench.sh`, `scripts/check_regression.py` | Docker-based profiling and the regression gate | 13, 15 |
| `docs/OPTIMIZATION-LOG.md`, `docs/BENCHMARKS.md` | One entry per optimization attempt including failures; published results | 13, 14, 15 |

---

## Task 1: The clock

**Files:**
- Create: `bench/clock.hpp`, `bench/CMakeLists.txt`, `tests/test_clock.cpp`
- Modify: `CMakeLists.txt` (add `bench` subdirectory behind `OB_BUILD_BENCH`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `ob::bench::do_not_optimize(const T&)`, `ob::bench::clobber_memory()`, `ob::bench::Clock` with `static raw()`, `static raw_serialized()`, `static Clock& instance()`, `ns_per_tick()`, `ticks_to_ns(std::uint64_t) -> double`, `counter_hz()`, `resolution_ticks()`, `resolution_ns()`, `overhead_ns_serialized()`, `overhead_ns_raw()`, `print_report(std::FILE*)`.

Two reads, deliberately: `raw()` is one instruction and can float across surrounding work; `raw_serialized()` inserts `isb` (or `rdtscp`+`lfence`) so it cannot. **Latency measurement must use the serialized form and throughput loops must not**, because the `isb` cost would then be inside every measured interval.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_clock.cpp
#include "../bench/clock.hpp"

#include <gtest/gtest.h>

namespace {

using ob::bench::Clock;
using ob::bench::do_not_optimize;

TEST(Clock, CounterFrequencyIsPlausible) {
    const std::uint64_t hz = Clock::instance().counter_hz();
    // 1 MHz to 10 GHz. Anything outside that means calibration failed.
    EXPECT_GT(hz, 1'000'000u);
    EXPECT_LT(hz, 10'000'000'000u);
}

TEST(Clock, TickConversionIsConsistent) {
    const Clock& c = Clock::instance();
    EXPECT_DOUBLE_EQ(c.ticks_to_ns(0), 0.0);
    EXPECT_GT(c.ticks_to_ns(1000), c.ticks_to_ns(999));
    // ns_per_tick must agree with the reported frequency.
    EXPECT_NEAR(c.ns_per_tick(), 1e9 / static_cast<double>(c.counter_hz()), 1e-9);
}

TEST(Clock, RawIsMonotonicallyNonDecreasing) {
    std::uint64_t prev = Clock::raw();
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t now = Clock::raw();
        ASSERT_GE(now, prev) << "counter went backwards at i=" << i;
        prev = now;
    }
}

TEST(Clock, ResolutionIsMeasuredAndReported) {
    const Clock& c = Clock::instance();
    EXPECT_GT(c.resolution_ticks(), 0u);
    // On this hardware the true answer is ~41.67 ns. Accept 0.1 ns to 1 us so the
    // test is portable, but the value gets printed so a human sees the real one.
    EXPECT_GT(c.resolution_ns(), 0.05);
    EXPECT_LT(c.resolution_ns(), 1000.0);
}

TEST(Clock, OverheadIsMeasuredAndSerializedCostsMore) {
    const Clock& c = Clock::instance();
    EXPECT_GT(c.overhead_ns_raw(), 0.0);
    EXPECT_GT(c.overhead_ns_serialized(), 0.0);
    EXPECT_LT(c.overhead_ns_serialized(), 500.0);
    // isb / rdtscp exists precisely to cost something. If the serialized read is
    // not more expensive, the barrier is not being emitted.
    EXPECT_GT(c.overhead_ns_serialized(), c.overhead_ns_raw());
}

// The guard test. If do_not_optimize stops working, every benchmark in this
// project silently starts reporting the cost of nothing, and no other test
// notices. 1e6 dependent adds cannot take less than 100 us.
TEST(Clock, DoNotOptimizeActuallyPreventsElision) {
    const std::uint64_t t0 = Clock::raw_serialized();
    std::uint64_t acc = 0;
    for (int i = 0; i < 1'000'000; ++i) {
        acc += static_cast<std::uint64_t>(i) * 2654435761u;
        do_not_optimize(acc);
    }
    const std::uint64_t t1 = Clock::raw_serialized();
    do_not_optimize(acc);

    const double ns = Clock::instance().ticks_to_ns(t1 - t0);
    EXPECT_GT(ns, 100'000.0) << "1e6 barriered adds took " << ns
                             << " ns; the barrier is not working";
}

TEST(Clock, ReportPrintsSomethingUseful) {
    Clock::instance().print_report(stdout);
    SUCCEED();
}

}  // namespace
```

- [ ] **Step 2: Wire up the build, run, verify it fails**

In the root `CMakeLists.txt`, after the `ob` target:

```cmake
if(OB_BUILD_BENCH)
    add_subdirectory(bench)
endif()
```

Create `bench/CMakeLists.txt`:

```cmake
# Header-only for now; executables arrive in Tasks 11, 12 and 15.
add_library(ob_bench INTERFACE)
target_include_directories(ob_bench INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_bench INTERFACE ob)
```

Add `test_clock.cpp` to `ob_tests` sources. Then:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_WARNINGS_AS_ERRORS=ON -DOB_BUILD_BENCH=ON
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'../bench/clock.hpp' file not found`.

- [ ] **Step 3: Write `bench/clock.hpp`**

```cpp
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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <time.h>

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
        std::uint32_t aux;
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
    [[nodiscard]] double ns_per_tick() const noexcept { return ns_per_tick_; }

    [[nodiscard]] double ticks_to_ns(std::uint64_t ticks) const noexcept {
        return static_cast<double>(ticks) * ns_per_tick_;
    }

    [[nodiscard]] std::uint64_t resolution_ticks() const noexcept { return res_ticks_; }
    [[nodiscard]] double resolution_ns() const noexcept {
        return ticks_to_ns(res_ticks_);
    }
    [[nodiscard]] double overhead_ns_raw() const noexcept { return oh_raw_ns_; }
    [[nodiscard]] double overhead_ns_serialized() const noexcept { return oh_ser_ns_; }

    void print_report(std::FILE* out) const {
        std::fprintf(out,
                     "clock: hz=%llu ns_per_tick=%.6f resolution=%llu ticks (%.3f ns) "
                     "overhead_raw=%.3f ns overhead_serialized=%.3f ns\n",
                     static_cast<unsigned long long>(hz_), ns_per_tick_,
                     static_cast<unsigned long long>(res_ticks_), resolution_ns(),
                     oh_raw_ns_, oh_ser_ns_);
    }

private:
    Clock() {
        hz_ = detect_hz();
        ns_per_tick_ = 1e9 / static_cast<double>(hz_);
        res_ticks_ = measure_resolution_ticks();
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
        timespec sleep_for{0, 200'000'000};
        nanosleep(&sleep_for, nullptr);
        const std::uint64_t t1 = raw_serialized();
        clock_gettime(CLOCK_MONOTONIC, &b);

        const double elapsed_ns =
            static_cast<double>(b.tv_sec - a.tv_sec) * 1e9 +
            static_cast<double>(b.tv_nsec - a.tv_nsec);
        return static_cast<std::uint64_t>(static_cast<double>(t1 - t0) * 1e9 /
                                         elapsed_ns);
#endif
    }

    // Smallest nonzero difference between two consecutive reads. On this hardware
    // the answer is 1 tick, and one tick is 41.67 ns.
    static std::uint64_t measure_resolution_ticks() {
        std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
        for (int i = 0; i < 200000; ++i) {
            const std::uint64_t a = raw();
            const std::uint64_t b = raw();
            if (b > a) {
                best = std::min(best, b - a);
            }
        }
        return best == std::numeric_limits<std::uint64_t>::max() ? 1 : best;
    }

    // Amortized cost of an unserialized read. This measures THROUGHPUT, not
    // latency: consecutive reads pipeline, so the number is lower than the cost
    // of one isolated read. Reported as-is, labelled as such.
    double measure_overhead_raw_ns() const {
        constexpr int kN = 200000;
        const std::uint64_t t0 = raw_serialized();
        std::uint64_t acc = 0;
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
        constexpr int kN = 200000;
        const std::uint64_t t0 = raw_serialized();
        std::uint64_t acc = 0;
        for (int i = 0; i < kN; ++i) {
            acc += raw_serialized();
        }
        const std::uint64_t t1 = raw_serialized();
        do_not_optimize(acc);
        return ticks_to_ns(t1 - t0) / kN;
    }

    std::uint64_t hz_ = 0;
    double        ns_per_tick_ = 0.0;
    std::uint64_t res_ticks_ = 0;
    double        oh_raw_ns_ = 0.0;
    double        oh_ser_ns_ = 0.0;
};

}  // namespace ob::bench
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build -R Clock --output-on-failure
```

Expected: PASS, 7 tests. Read the printed report and confirm it matches the measured facts: on this machine `ns_per_tick` should be `1.000000` with `resolution=42 ticks` (macOS reports the counter in nanosecond units), and in Docker it should be `ns_per_tick=41.666667` with `resolution=1 ticks`. **Both must give `resolution_ns()` ~= 41.67.** That cross-platform agreement is the real assertion; write the two numbers into the commit message.

- [ ] **Step 5: Confirm the same header behaves identically in Docker**

```bash
docker run --rm -v "$PWD":/w -w /w alpine:3.20 sh -c \
  'apk add --no-cache cmake ninja g++ git musl-dev >/dev/null 2>&1 &&
   cmake -S . -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_BUILD_BENCH=ON &&
   cmake --build /tmp/b >/dev/null &&
   /tmp/b/tests/ob_tests --gtest_filter=Clock.*'
```

Expected: PASS, and the printed `resolution` in nanoseconds matches the host's to within a tick. If the two disagree, the `CNTFRQ_EL0` conversion is wrong on one platform and every later number is suspect.

- [ ] **Step 6: Commit**

```bash
git add bench/clock.hpp bench/CMakeLists.txt CMakeLists.txt tests/test_clock.cpp tests/CMakeLists.txt
git commit -m "feat(bench): timestamp source with self-measured resolution and overhead

Converts through CNTFRQ_EL0 so the same header is correct on macOS arm64
(counter scaled to ns units, advances every 41.67 ns) and Linux arm64 (true
24 MHz). Provides serialized and unserialized reads because latency and
throughput measurement need different ones. Includes a guard test that fails
if do_not_optimize stops working, which would otherwise silently make every
benchmark in the project measure nothing."
```

---

## Task 2: Exact-percentile histogram

**Files:**
- Create: `bench/histogram.hpp`, `tests/test_histogram.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `ob::bench::Histogram` with `explicit Histogram(std::size_t capacity)`, `record(std::uint64_t) noexcept`, `count()`, `capacity()`, `saturated()`, `percentile(double) -> std::uint32_t`, `min()`, `max()`, `mean()`, `merge(const Histogram&)`, `clear()`, `write_raw(const char*) const`, `samples() -> std::span<const std::uint32_t>`.

Exact rather than bucketed: 10^7 samples at 4 bytes is 40 MB, which is free, and approximation buys nothing when memory is not the constraint. Values are stored unit-agnostic (the harness records **ticks** and converts only at report time, because converting inside a measured region would put a floating-point multiply in the hot path).

Percentile uses **nearest-rank**: `index = ceil(p/100 * N) - 1`, clamped. Stated explicitly because ambiguous percentile definitions are a real source of disagreement between benchmark tools.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_histogram.cpp
#include "../bench/histogram.hpp"

#include <gtest/gtest.h>

#include <numeric>
#include <random>

namespace {

using ob::bench::Histogram;

// 1..100 makes every nearest-rank answer checkable by hand.
Histogram one_to_hundred() {
    Histogram h(100);
    for (std::uint32_t v = 1; v <= 100; ++v) {
        h.record(v);
    }
    return h;
}

TEST(Histogram, StartsEmpty) {
    const Histogram h(16);
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.capacity(), 16u);
    EXPECT_EQ(h.saturated(), 0u);
}

TEST(Histogram, NearestRankPercentilesOnAKnownDistribution) {
    Histogram h = one_to_hundred();
    ASSERT_EQ(h.count(), 100u);
    EXPECT_EQ(h.percentile(1.0), 1u);
    EXPECT_EQ(h.percentile(50.0), 50u);
    EXPECT_EQ(h.percentile(90.0), 90u);
    EXPECT_EQ(h.percentile(99.0), 99u);
    EXPECT_EQ(h.percentile(100.0), 100u);
}

TEST(Histogram, MinMaxMean) {
    Histogram h = one_to_hundred();
    EXPECT_EQ(h.min(), 1u);
    EXPECT_EQ(h.max(), 100u);
    EXPECT_NEAR(h.mean(), 50.5, 1e-9);
}

TEST(Histogram, PercentileIsOrderIndependent) {
    std::vector<std::uint32_t> v(100);
    std::iota(v.begin(), v.end(), 1u);
    std::mt19937_64 rng(4);
    std::shuffle(v.begin(), v.end(), rng);

    Histogram h(100);
    for (const std::uint32_t x : v) {
        h.record(x);
    }
    EXPECT_EQ(h.percentile(50.0), 50u);
    EXPECT_EQ(h.percentile(99.0), 99u);
}

TEST(Histogram, RecordingAfterReadingStillWorks) {
    Histogram h(200);
    for (std::uint32_t v = 1; v <= 100; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.percentile(50.0), 50u);  // sorts internally
    for (std::uint32_t v = 101; v <= 200; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.percentile(50.0), 100u) << "cached sort was not invalidated";
    EXPECT_EQ(h.max(), 200u);
}

// A single sample means p99 and max are the same sample. The harness prints the
// count next to every percentile precisely so this is visible.
TEST(Histogram, SingleSample) {
    Histogram h(1);
    h.record(7);
    EXPECT_EQ(h.percentile(50.0), 7u);
    EXPECT_EQ(h.percentile(99.9), 7u);
    EXPECT_EQ(h.min(), 7u);
    EXPECT_EQ(h.max(), 7u);
}

TEST(Histogram, ValuesTooLargeForUint32SaturateAndAreCounted) {
    Histogram h(4);
    h.record(10);
    h.record(std::uint64_t{1} << 40);
    EXPECT_EQ(h.saturated(), 1u);
    EXPECT_EQ(h.max(), std::numeric_limits<std::uint32_t>::max());
    // The saturation count is the signal that a run is not trustworthy.
}

TEST(Histogram, MergeCombinesBothSetsOfSamples) {
    Histogram a(4), b(4);
    a.record(1);
    a.record(3);
    b.record(2);
    b.record(4);
    a.merge(b);
    EXPECT_EQ(a.count(), 4u);
    EXPECT_EQ(a.percentile(100.0), 4u);
    EXPECT_EQ(a.min(), 1u);
}

TEST(Histogram, ClearResetsCountAndSaturationButKeepsCapacity) {
    Histogram h(8);
    h.record(1);
    h.record(std::uint64_t{1} << 40);
    h.clear();
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.saturated(), 0u);
    EXPECT_EQ(h.capacity(), 8u);
}

// record() must never reallocate: it runs inside the measured loop, and an
// allocation there would show up as a latency spike caused by the harness.
TEST(HistogramDeathTest, RecordingBeyondCapacityAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Histogram h(1);
    h.record(1);
    EXPECT_DEATH(h.record(2), "");
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'../bench/histogram.hpp' file not found`.

- [ ] **Step 3: Write `bench/histogram.hpp`**

```cpp
// bench/histogram.hpp
#pragma once

// Exact-percentile recorder.
//
// Stores raw samples rather than bucketing them: 10^7 samples at 4 bytes is 40 MB,
// which costs nothing here, and approximation buys nothing when memory is not the
// constraint. HdrHistogram exists for the case where it is.
//
// Values are UNIT-AGNOSTIC. The latency harness records raw counter TICKS and
// converts to nanoseconds only at report time, because a floating-point multiply
// inside the measured region would be measuring the harness.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace ob::bench {

class Histogram {
public:
    explicit Histogram(std::size_t capacity) { samples_.reserve(capacity); }

    // Never reallocates: capacity is reserved up front and exceeding it is a
    // caller error, exactly as with ob::EventBuffer.
    void record(std::uint64_t value) noexcept {
        assert(samples_.size() < samples_.capacity() &&
               "Histogram overflow: reserve the full sample count up front");
        constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
        if (value > kMax) {
            ++saturated_;
            value = kMax;
        }
        samples_.push_back(static_cast<std::uint32_t>(value));
        sorted_ = false;
    }

    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return samples_.capacity(); }
    [[nodiscard]] std::size_t saturated() const noexcept { return saturated_; }
    [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }

    // Nearest-rank: index = ceil(p/100 * N) - 1, clamped to [0, N-1]. Stated
    // explicitly because tools disagree on this and the disagreement is invisible.
    [[nodiscard]] std::uint32_t percentile(double p) {
        assert(!samples_.empty() && "percentile of an empty histogram");
        ensure_sorted();
        const double n = static_cast<double>(samples_.size());
        double rank = std::ceil(p / 100.0 * n) - 1.0;
        rank = std::clamp(rank, 0.0, n - 1.0);
        return samples_[static_cast<std::size_t>(rank)];
    }

    [[nodiscard]] std::uint32_t min() {
        ensure_sorted();
        return samples_.front();
    }
    [[nodiscard]] std::uint32_t max() {
        ensure_sorted();
        return samples_.back();
    }

    [[nodiscard]] double mean() const {
        if (samples_.empty()) {
            return 0.0;
        }
        const std::uint64_t sum =
            std::accumulate(samples_.begin(), samples_.end(), std::uint64_t{0},
                            [](std::uint64_t a, std::uint32_t b) { return a + b; });
        return static_cast<double>(sum) / static_cast<double>(samples_.size());
    }

    void merge(const Histogram& other) {
        samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
        saturated_ += other.saturated_;
        sorted_ = false;
    }

    void clear() noexcept {
        samples_.clear();
        saturated_ = 0;
        sorted_ = false;
    }

    // Raw samples are committed alongside published figures so a reader can
    // recompute any percentile themselves rather than trusting the summary.
    void write_raw(const char* path) const {
        std::FILE* f = std::fopen(path, "wb");
        if (f == nullptr) {
            return;
        }
        std::fwrite(samples_.data(), sizeof(std::uint32_t), samples_.size(), f);
        std::fclose(f);
    }

    [[nodiscard]] std::span<const std::uint32_t> samples() const noexcept {
        return {samples_.data(), samples_.size()};
    }

private:
    void ensure_sorted() {
        if (!sorted_) {
            std::sort(samples_.begin(), samples_.end());
            sorted_ = true;
        }
    }

    std::vector<std::uint32_t> samples_;
    std::size_t saturated_ = 0;
    bool        sorted_ = false;
};

}  // namespace ob::bench
```

Add `#include <cmath>` for `std::ceil`.

Note `min()`, `max()` and `percentile()` are non-const because they sort lazily. That is deliberate: the alternative is sorting a copy, which would allocate.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build -R Histogram --output-on-failure
```

Expected: PASS, 11 tests.

- [ ] **Step 5: Commit**

```bash
git add bench/histogram.hpp tests/test_histogram.cpp tests/CMakeLists.txt
git commit -m "feat(bench): exact-percentile histogram with nearest-rank and saturation accounting"
```

---

## Task 3: The order slot and the arena

**Files:**
- Create: `include/ob/order.hpp`, `include/ob/order_pool.hpp`, `tests/test_order_pool.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/types.hpp`.
- Produces: `ob::Order` (exactly 32 bytes); `ob::OrderPool` with `explicit OrderPool(std::size_t capacity)`, `alloc() -> Slot`, `free(Slot)`, `at(Slot) -> Order&`, `at(Slot) const -> const Order&`, `capacity()`, `size()`, `full()`, `reset()`.

Three decisions worth being able to defend:

1. **Indices, not pointers.** `Slot` is `uint32_t`. The struct is smaller, the arena can be relocated or serialized without pointer fixups, and a bad index trips a bounds assert where a bad pointer is undefined behavior.
2. **The free list is threaded through `Order::next`**, the same field the per-level FIFO uses when the slot is live. One field, two meanings, disjoint lifetimes, zero extra memory.
3. **`id == 0` marks a free slot.** Order ID 0 is already invalid (spec E49), so this costs nothing and turns double-free and use-after-free into debug assertions.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_order_pool.cpp
#include <ob/order_pool.hpp>

#include <gtest/gtest.h>

#include <unordered_set>

namespace {

TEST(Order, IsExactlyThirtyTwoBytes) {
    static_assert(sizeof(ob::Order) == 32, "Order must stay 32 bytes");
    static_assert(alignof(ob::Order) == 8);
    static_assert(std::is_trivially_copyable_v<ob::Order>);
    // 2 per 64-byte line on x86, 4 per 128-byte line on Apple Silicon.
    SUCCEED();
}

TEST(OrderPool, FreshPoolIsEmptyWithTheRequestedCapacity) {
    const ob::OrderPool p(16);
    EXPECT_EQ(p.capacity(), 16u);
    EXPECT_EQ(p.size(), 0u);
    EXPECT_FALSE(p.full());
}

TEST(OrderPool, AllocReturnsDistinctSlotsUntilExhausted) {
    ob::OrderPool p(64);
    std::unordered_set<ob::Slot> seen;
    for (std::size_t i = 0; i < 64; ++i) {
        const ob::Slot s = p.alloc();
        ASSERT_NE(s, ob::kInvalidSlot) << "ran out early at i=" << i;
        p.at(s).id = static_cast<ob::OrderId>(i + 1);  // claim it
        EXPECT_TRUE(seen.insert(s).second) << "slot " << s << " handed out twice";
    }
    EXPECT_EQ(p.size(), 64u);
    EXPECT_TRUE(p.full());
    EXPECT_EQ(p.alloc(), ob::kInvalidSlot);
}

// Spec E39: exhaustion is a capacity condition, and freeing makes room again.
TEST(OrderPool, FreeingMakesCapacityAvailableAgain) {
    ob::OrderPool p(4);
    std::vector<ob::Slot> slots;
    for (int i = 0; i < 4; ++i) {
        const ob::Slot s = p.alloc();
        p.at(s).id = static_cast<ob::OrderId>(i + 1);
        slots.push_back(s);
    }
    ASSERT_EQ(p.alloc(), ob::kInvalidSlot);

    p.free(slots[2]);
    EXPECT_EQ(p.size(), 3u);
    const ob::Slot again = p.alloc();
    EXPECT_NE(again, ob::kInvalidSlot);
    EXPECT_EQ(p.size(), 4u);
}

TEST(OrderPool, StoredFieldsRoundTrip) {
    ob::OrderPool p(8);
    const ob::Slot s = p.alloc();
    ob::Order& o = p.at(s);
    o.id = 12345;
    o.price = 10050;
    o.remaining = 300;
    o.side = ob::Side::Sell;
    o.next = ob::kInvalidSlot;
    o.prev = ob::kInvalidSlot;

    const ob::Order& r = p.at(s);
    EXPECT_EQ(r.id, 12345u);
    EXPECT_EQ(r.price, 10050);
    EXPECT_EQ(r.remaining, 300u);
    EXPECT_EQ(r.side, ob::Side::Sell);
}

TEST(OrderPool, ResetReturnsEverySlotToTheFreeList) {
    ob::OrderPool p(8);
    for (int i = 0; i < 8; ++i) {
        p.at(p.alloc()).id = static_cast<ob::OrderId>(i + 1);
    }
    ASSERT_TRUE(p.full());
    p.reset();
    EXPECT_EQ(p.size(), 0u);
    EXPECT_FALSE(p.full());
    EXPECT_NE(p.alloc(), ob::kInvalidSlot);
}

// Construction must leave every page resident, because the harness relies on it
// instead of a prefault() call. A first pass over a large arena that paid page
// faults would show up as latency outliers attributable to the harness.
TEST(OrderPool, ConstructionLeavesEveryPageResident) {
    ob::OrderPool p(1 << 16);
    // Every slot is readable and free immediately, with no faulting pass needed.
    for (std::size_t i = 0; i < p.capacity(); ++i) {
        ASSERT_EQ(p.at(static_cast<ob::Slot>(i)).id, 0u) << "slot " << i;
    }
    EXPECT_EQ(p.size(), 0u);
}

// A double free corrupts the free list into a cycle, and the symptom appears
// arbitrarily far away. Catching it at the call site is worth an assert.
TEST(OrderPoolDeathTest, DoubleFreeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ob::OrderPool p(4);
    const ob::Slot s = p.alloc();
    p.at(s).id = 1;
    p.free(s);
    EXPECT_DEATH(p.free(s), "");
}

TEST(OrderPoolDeathTest, OutOfRangeSlotAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ob::OrderPool p(4);
    // at() is [[nodiscard]], so the result must be explicitly discarded even
    // though this statement never returns. Without the cast this fails the
    // build under -Werror with -Wunused-result. Found during execution.
    EXPECT_DEATH(static_cast<void>(p.at(99)), "");
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

Add `test_order_pool.cpp` to `ob_tests`, then:

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/order_pool.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/order.hpp`**

```cpp
// include/ob/order.hpp
#pragma once

#include <ob/types.hpp>

#include <type_traits>

namespace ob {

// Exactly 32 bytes: 2 per 64-byte cache line on x86, 4 per 128-byte line on Apple
// Silicon. Every field is load-bearing and there is no room for anything else,
// which is why the arrival sequence used by the invariant checker lives only in
// ReferenceEngine (see Phase 1's kTracksArrival).
//
// `next` does double duty: the per-level FIFO link while the slot is live, and the
// free-list link while it is not. The two lifetimes are disjoint.
struct Order {
    OrderId       id        = 0;             // 8.  0 means this slot is free.
    Ticks         price     = kNoPrice;      // 4
    Qty           remaining = 0;             // 4
    Slot          next      = kInvalidSlot;  // 4
    Slot          prev      = kInvalidSlot;  // 4
    Side          side      = Side::Buy;     // 1
    std::uint8_t  flags     = 0;             // 1.  reserved: participant / STP
    std::uint16_t pad       = 0;             // 2
};

static_assert(sizeof(Order) == 32, "Order must stay 32 bytes");
static_assert(alignof(Order) == 8);
static_assert(std::is_trivially_copyable_v<Order>);

}  // namespace ob
```

- [ ] **Step 4: Write `include/ob/order_pool.hpp`**

```cpp
// include/ob/order_pool.hpp
#pragma once

// Pre-allocated arena of order slots with an O(1) index free list.
//
// Allocates exactly once, in the constructor. After that, alloc() and free() are
// a handful of instructions and touch no allocator. Exhaustion returns
// kInvalidSlot, which the engine turns into Rejected(EngineCapacity) (spec E39).

#include <ob/order.hpp>

#include <cassert>
#include <cstddef>
#include <vector>

namespace ob {

class OrderPool {
public:
    explicit OrderPool(std::size_t capacity) : slots_(capacity) {
        assert(capacity > 0 && capacity < kInvalidSlot &&
               "capacity must be positive and leave kInvalidSlot free as a sentinel");
        build_free_list();
    }

    // kInvalidSlot when full. The returned slot's id is 0 until the caller sets it.
    [[nodiscard]] Slot alloc() noexcept {
        if (free_head_ == kInvalidSlot) {
            return kInvalidSlot;
        }
        const Slot s = free_head_;
        assert(slots_[s].id == 0 && "free-list slot was still live");
        free_head_ = slots_[s].next;
        slots_[s].next = kInvalidSlot;
        slots_[s].prev = kInvalidSlot;
        ++live_;
        return s;
    }

    void free(Slot s) noexcept {
        assert(s < slots_.size() && "slot out of range");
        // id == 0 means already free. A double free would splice a cycle into the
        // free list and the symptom would surface arbitrarily far from the cause.
        assert(slots_[s].id != 0 && "double free of an order slot");
        slots_[s].id = 0;
        slots_[s].next = free_head_;
        free_head_ = s;
        --live_;
    }

    [[nodiscard]] Order& at(Slot s) noexcept {
        assert(s < slots_.size() && "slot out of range");
        return slots_[s];
    }
    [[nodiscard]] const Order& at(Slot s) const noexcept {
        assert(s < slots_.size() && "slot out of range");
        return slots_[s];
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return live_; }
    [[nodiscard]] bool full() const noexcept { return free_head_ == kInvalidSlot; }

    // No prefault() here on purpose. std::vector<Order> slots_(capacity)
    // value-initializes every element, which WRITES every byte, which faults in
    // every page. The arena is therefore resident from construction and a separate
    // prefault would be a no-op wearing a reassuring name. Cache warming is a
    // different problem, handled by the harness's discarded warm-up iterations.
    void reset() noexcept {
        for (Order& o : slots_) {
            o = Order{};
        }
        live_ = 0;
        build_free_list();
    }

private:
    void build_free_list() noexcept {
        const std::size_t n = slots_.size();
        for (std::size_t i = 0; i + 1 < n; ++i) {
            slots_[i].next = static_cast<Slot>(i + 1);
        }
        slots_[n - 1].next = kInvalidSlot;
        free_head_ = 0;
    }

    std::vector<Order> slots_;
    Slot        free_head_ = kInvalidSlot;
    std::size_t live_ = 0;
};

}  // namespace ob
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build -R "Order" --output-on-failure
```

Expected: PASS, 10 tests. If `sizeof(Order) == 32` fails, the field order was changed; restore it rather than relaxing the assertion, because the packing is the point.

- [ ] **Step 6: Commit**

```bash
git add include/ob/order.hpp include/ob/order_pool.hpp tests/test_order_pool.cpp tests/CMakeLists.txt
git commit -m "feat: 32-byte Order slot and allocation-free arena with index free list"
```

---

## Task 4: The order-ID index

**Files:**
- Create: `include/ob/id_index.hpp`, `tests/test_id_index.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/types.hpp`.
- Produces: `ob::IdIndex` with `explicit IdIndex(std::size_t min_entries)`, `insert(OrderId, Slot) -> bool`, `find(OrderId) const -> Slot`, `erase(OrderId) -> bool`, `size()`, `capacity()`, `max_load()`, `full()`, `reset()`, `static hash(OrderId) -> std::uint64_t`.

This is what makes cancel O(1). Design notes worth defending:

- **Open addressing with linear probing**, because the probe sequence is contiguous and therefore cache- and prefetch-friendly, which is the whole reason not to use a chained map.
- **Backward-shift deletion, not tombstones.** Tombstones accumulate under the 90%-cancel workload that is the realistic case, and probe lengths grow without bound until a rehash, which is forbidden. Backward-shift keeps the table tombstone-free so `find` can stop at the first empty slot forever.
- **Never rehashes.** Capacity is a power of two, at least twice `min_entries`, and `full()` reports the load ceiling so the engine can turn it into `Rejected(EngineCapacity)` (spec E40).
- **`id == 0` is the empty marker**, consistent with `OrderPool` and with order ID 0 being invalid.
- **SplitMix64 finalizer as the hash.** Identity hashing would actually be *faster* for the sequential IDs a real sequencer produces, and Task 14 measures exactly that as a candidate optimization. It is not the starting point because the fuzzer supplies scattered IDs and the structure should not degrade under them.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_id_index.cpp
#include <ob/id_index.hpp>

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

namespace {

TEST(IdIndex, CapacityIsAPowerOfTwoAndAtLeastTwiceTheRequest) {
    const ob::IdIndex ix(100);
    EXPECT_GE(ix.capacity(), 200u);
    EXPECT_EQ(ix.capacity() & (ix.capacity() - 1), 0u) << "capacity must be 2^n";
    EXPECT_GE(ix.max_load(), 100u);
}

TEST(IdIndex, InsertThenFind) {
    ob::IdIndex ix(16);
    EXPECT_TRUE(ix.insert(42, 7));
    EXPECT_EQ(ix.find(42), 7u);
    EXPECT_EQ(ix.size(), 1u);
}

TEST(IdIndex, FindOfAnAbsentIdReturnsInvalidSlot) {
    ob::IdIndex ix(16);
    ix.insert(42, 7);
    EXPECT_EQ(ix.find(43), ob::kInvalidSlot);
}

TEST(IdIndex, DuplicateInsertIsRefusedAndLeavesTheOriginal) {
    ob::IdIndex ix(16);
    ASSERT_TRUE(ix.insert(42, 7));
    EXPECT_FALSE(ix.insert(42, 9));
    EXPECT_EQ(ix.find(42), 7u);
    EXPECT_EQ(ix.size(), 1u);
}

TEST(IdIndex, EraseRemovesAndReportsWhetherItFoundAnything) {
    ob::IdIndex ix(16);
    ix.insert(42, 7);
    EXPECT_TRUE(ix.erase(42));
    EXPECT_EQ(ix.find(42), ob::kInvalidSlot);
    EXPECT_EQ(ix.size(), 0u);
    EXPECT_FALSE(ix.erase(42)) << "erasing twice must report not-found";
}

// Spec E40: the load ceiling is a capacity rejection, never a rehash.
TEST(IdIndex, RefusesInsertAtTheLoadCeilingAndNeverRehashes) {
    ob::IdIndex ix(8);
    const std::size_t cap = ix.capacity();
    const std::size_t ceiling = ix.max_load();

    for (std::size_t i = 1; i <= ceiling; ++i) {
        ASSERT_TRUE(ix.insert(static_cast<ob::OrderId>(i), static_cast<ob::Slot>(i)))
            << "failed at " << i;
    }
    EXPECT_TRUE(ix.full());
    EXPECT_FALSE(ix.insert(999999, 1));
    EXPECT_EQ(ix.capacity(), cap) << "capacity changed, so it rehashed";
}

// THE test for backward-shift deletion. Build a real collision chain, remove an
// element from the middle of it, and confirm the rest are still reachable. With
// tombstones this passes trivially; with a naive "just clear the slot" deletion
// it fails, and with a buggy backward shift it fails.
TEST(IdIndex, CollisionChainSurvivesDeletionFromTheMiddle) {
    ob::IdIndex ix(8);
    const std::uint64_t mask = ix.capacity() - 1;

    // Find four ids that all hash to the same bucket.
    std::vector<ob::OrderId> colliding;
    const std::uint64_t target = ob::IdIndex::hash(1) & mask;
    for (ob::OrderId id = 1; id < 2'000'000 && colliding.size() < 4; ++id) {
        if ((ob::IdIndex::hash(id) & mask) == target) {
            colliding.push_back(id);
        }
    }
    ASSERT_EQ(colliding.size(), 4u) << "could not construct a collision chain";

    for (std::size_t i = 0; i < colliding.size(); ++i) {
        ASSERT_TRUE(ix.insert(colliding[i], static_cast<ob::Slot>(100 + i)));
    }
    for (std::size_t i = 0; i < colliding.size(); ++i) {
        ASSERT_EQ(ix.find(colliding[i]), 100u + i);
    }

    ASSERT_TRUE(ix.erase(colliding[1]));
    EXPECT_EQ(ix.find(colliding[1]), ob::kInvalidSlot);
    EXPECT_EQ(ix.find(colliding[0]), 100u);
    EXPECT_EQ(ix.find(colliding[2]), 102u) << "backward shift broke the chain";
    EXPECT_EQ(ix.find(colliding[3]), 103u) << "backward shift broke the chain";
}

// Property test against std::unordered_map. Randomized insert and erase is how
// backward-shift bugs actually surface, because they need a specific arrangement.
TEST(IdIndex, MatchesAReferenceMapUnderRandomInsertAndErase) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        ob::IdIndex ix(512);
        std::unordered_map<ob::OrderId, ob::Slot> model;
        std::mt19937_64 rng(seed);

        for (int op = 0; op < 20000; ++op) {
            const ob::OrderId id = 1 + (rng() % 2000);
            if (rng() % 2 == 0 && ix.size() < ix.max_load()) {
                const ob::Slot slot = static_cast<ob::Slot>(rng() % 100000);
                const bool a = ix.insert(id, slot);
                const bool b = model.emplace(id, slot).second;
                ASSERT_EQ(a, b) << "seed " << seed << " op " << op;
            } else {
                const bool a = ix.erase(id);
                const bool b = model.erase(id) != 0;
                ASSERT_EQ(a, b) << "seed " << seed << " op " << op;
            }
            ASSERT_EQ(ix.size(), model.size()) << "seed " << seed << " op " << op;
        }
        // Every surviving entry must still be reachable.
        for (const auto& [id, slot] : model) {
            ASSERT_EQ(ix.find(id), slot) << "seed " << seed << " lost id " << id;
        }
    }
}

TEST(IdIndex, ResetEmptiesWithoutChangingCapacity) {
    ob::IdIndex ix(16);
    for (ob::OrderId id = 1; id <= 8; ++id) {
        ix.insert(id, static_cast<ob::Slot>(id));
    }
    const std::size_t cap = ix.capacity();
    ix.reset();
    EXPECT_EQ(ix.size(), 0u);
    EXPECT_EQ(ix.capacity(), cap);
    EXPECT_EQ(ix.find(1), ob::kInvalidSlot);
}

TEST(IdIndex, IdZeroIsNotStorable) {
    ob::IdIndex ix(16);
    // Id 0 is the empty marker. The engine rejects it before reaching here
    // (spec E49); this asserts the index itself does not silently accept it.
    EXPECT_FALSE(ix.insert(0, 1));
    EXPECT_EQ(ix.find(0), ob::kInvalidSlot);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/id_index.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/id_index.hpp`**

```cpp
// include/ob/id_index.hpp
#pragma once

// Open-addressed OrderId -> Slot map. This is what makes cancel O(1).
//
// Linear probing, because the probe sequence is contiguous and therefore
// cache-friendly, which is the entire reason not to use a chained map.
//
// BACKWARD-SHIFT DELETION rather than tombstones. Under the realistic workload
// (roughly 90% cancels) tombstones accumulate and probe lengths grow without
// bound until a rehash, and rehashing is forbidden on a no-allocation hot path.
// Keeping the table tombstone-free is also what lets find() stop at the first
// empty slot, forever.
//
// Never rehashes. Capacity is fixed at construction; reaching the load ceiling is
// a capacity rejection (spec E40), not a resize.

#include <ob/types.hpp>

#include <cassert>
#include <cstddef>
#include <vector>

namespace ob {

class IdIndex {
public:
    // Capacity is rounded up to a power of two of at least 2 * min_entries, so
    // min_entries live ids sit at a 0.5 load factor and probe lengths stay ~1.5.
    explicit IdIndex(std::size_t min_entries) {
        std::size_t cap = 1;
        while (cap < min_entries * 2) {
            cap <<= 1;
        }
        table_.assign(cap, Entry{});
        mask_ = cap - 1;
        max_load_ = cap / 2;
    }

    // SplitMix64 finalizer. Identity hashing would be faster for the sequential
    // ids a real sequencer emits (consecutive ids land in consecutive buckets with
    // zero collisions), and Task 14 measures that as a candidate optimization. It
    // is not the starting point because the fuzzer supplies scattered ids and the
    // structure must not degrade under them.
    [[nodiscard]] static std::uint64_t hash(OrderId id) noexcept {
        std::uint64_t z = id;
        z ^= z >> 33;
        z *= 0xFF51'AFD7'ED55'8CCDULL;
        z ^= z >> 33;
        z *= 0xC4CE'B9FE'1A85'EC53ULL;
        z ^= z >> 33;
        return z;
    }

    [[nodiscard]] bool insert(OrderId id, Slot slot) noexcept {
        if (id == 0) {
            return false;  // 0 is the empty marker; the engine rejects it earlier
        }
        if (size_ >= max_load_) {
            return false;  // capacity, never a rehash
        }
        std::size_t i = hash(id) & mask_;
        while (table_[i].id != 0) {
            if (table_[i].id == id) {
                return false;  // duplicate
            }
            i = (i + 1) & mask_;
        }
        table_[i].id = id;
        table_[i].slot = slot;
        ++size_;
        return true;
    }

    // Stops at the first empty slot, which is only correct because deletion keeps
    // the table tombstone-free.
    [[nodiscard]] Slot find(OrderId id) const noexcept {
        if (id == 0) {
            return kInvalidSlot;
        }
        std::size_t i = hash(id) & mask_;
        while (table_[i].id != 0) {
            if (table_[i].id == id) {
                return table_[i].slot;
            }
            i = (i + 1) & mask_;
        }
        return kInvalidSlot;
    }

    bool erase(OrderId id) noexcept {
        if (id == 0) {
            return false;
        }
        std::size_t i = hash(id) & mask_;
        while (table_[i].id != 0 && table_[i].id != id) {
            i = (i + 1) & mask_;
        }
        if (table_[i].id == 0) {
            return false;
        }

        // Backward-shift deletion. Walk forward from the hole; any entry whose
        // ideal bucket does NOT lie cyclically in (i, j] must move back into the
        // hole, or find() would stop early at it.
        table_[i] = Entry{};
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (table_[j].id == 0) {
                break;
            }
            const std::size_t k = hash(table_[j].id) & mask_;
            const bool leave_it =
                (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (leave_it) {
                continue;
            }
            table_[i] = table_[j];
            table_[j] = Entry{};
            i = j;
        }
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return table_.size(); }
    [[nodiscard]] std::size_t max_load() const noexcept { return max_load_; }
    [[nodiscard]] bool full() const noexcept { return size_ >= max_load_; }

    // No prefault() here on purpose: the constructor's assign() writes every byte
    // of the table, so every page is already resident before any measurement runs.
    // A separate prefault would be a no-op with a misleading name.
    void reset() noexcept {
        table_.assign(table_.size(), Entry{});
        size_ = 0;
    }

private:
    struct Entry {
        OrderId id = 0;                // 0 means empty
        Slot    slot = kInvalidSlot;
    };
    static_assert(sizeof(Entry) == 16);

    std::vector<Entry> table_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t max_load_ = 0;
};

}  // namespace ob
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build -R IdIndex --output-on-failure
```

Expected: PASS, 10 tests. `MatchesAReferenceMapUnderRandomInsertAndErase` runs 400,000 operations against `std::unordered_map`; it is the one that actually validates the backward shift. If `CollisionChainSurvivesDeletionFromTheMiddle` fails, the `leave_it` predicate's cyclic-range test is wrong — check the `i <= j` versus `i > j` branches against the comment before changing anything else.

- [ ] **Step 5: Commit**

```bash
git add include/ob/id_index.hpp tests/test_id_index.cpp tests/CMakeLists.txt
git commit -m "feat: open-addressed id index with backward-shift deletion and no rehashing"
```

---

## Task 5: The occupancy bitmap

**Files:**
- Create: `include/ob/level_bitmap.hpp`, `tests/test_level_bitmap.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/types.hpp`.
- Produces: `ob::LevelBitmap` with `static constexpr std::uint32_t kBits`, `static constexpr std::uint32_t kNotFound`, `set(std::uint32_t)`, `clear(std::uint32_t)`, `test(std::uint32_t) const`, `next_set_at_or_above(std::uint32_t) const -> std::uint32_t`, `prev_set_at_or_below(std::uint32_t) const -> std::uint32_t`, `empty() const`, `popcount() const`, `reset()`.

This is the piece that replaces the red-black tree walk. Three levels over 65,536 bits:

```
L2:      1 word   ( 16 bits used )  ─┐
L1:     16 words  ( 1024 bits )     ─┼─ each bit says "the word below me is non-empty"
L0:   1024 words  ( 65536 bits )    ─┘
```

Finding the next occupied level is at most three word loads and three `ctz`/`clz` instructions, regardless of how far away the next occupied level is. A `std::map` would be a pointer chase per tree level with a likely cache miss each time.

**The shift-by-64 trap.** `~0ull << 64` is undefined behavior, and it is reached whenever the bit index lands on a word boundary, which is exactly the case a hand-written test forgets. Both masking helpers guard it explicitly, and there are named boundary tests.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_level_bitmap.cpp
#include <ob/level_bitmap.hpp>

#include <gtest/gtest.h>

#include <random>
#include <set>

namespace {

using ob::LevelBitmap;

TEST(LevelBitmap, GeometryMatchesTheLadder) {
    static_assert(LevelBitmap::kBits == ob::kLadderSize);
    static_assert(LevelBitmap::kBits % 64 == 0);
    static_assert((LevelBitmap::kBits / 64) % 64 == 0);
    EXPECT_EQ(LevelBitmap::kBits, 65536u);
}

TEST(LevelBitmap, StartsEmpty) {
    const LevelBitmap b;
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.popcount(), 0u);
    EXPECT_EQ(b.next_set_at_or_above(0), LevelBitmap::kNotFound);
    EXPECT_EQ(b.prev_set_at_or_below(LevelBitmap::kBits - 1), LevelBitmap::kNotFound);
}

TEST(LevelBitmap, SetTestClear) {
    LevelBitmap b;
    EXPECT_FALSE(b.test(1234));
    b.set(1234);
    EXPECT_TRUE(b.test(1234));
    EXPECT_FALSE(b.empty());
    EXPECT_EQ(b.popcount(), 1u);
    b.clear(1234);
    EXPECT_FALSE(b.test(1234));
    EXPECT_TRUE(b.empty()) << "clearing the last bit must collapse the whole hierarchy";
}

TEST(LevelBitmap, FindsASingleBitFromEitherDirection) {
    for (const std::uint32_t i : {0u, 1u, 63u, 64u, 65u, 4095u, 4096u, 4097u,
                                  32768u, 65534u, 65535u}) {
        LevelBitmap b;
        b.set(i);
        EXPECT_EQ(b.next_set_at_or_above(0), i) << "bit " << i;
        EXPECT_EQ(b.next_set_at_or_above(i), i) << "bit " << i << " at-or-above is inclusive";
        EXPECT_EQ(b.prev_set_at_or_below(LevelBitmap::kBits - 1), i) << "bit " << i;
        EXPECT_EQ(b.prev_set_at_or_below(i), i) << "bit " << i << " at-or-below is inclusive";
        if (i + 1 < LevelBitmap::kBits) {
            EXPECT_EQ(b.next_set_at_or_above(i + 1), LevelBitmap::kNotFound) << "bit " << i;
        }
        if (i > 0) {
            EXPECT_EQ(b.prev_set_at_or_below(i - 1), LevelBitmap::kNotFound) << "bit " << i;
        }
    }
}

// The shift-by-64 trap. Every one of these indices puts the masking helper at a
// word boundary, which is where `~0ull << (b+1)` would be undefined behavior.
TEST(LevelBitmap, WordAndHierarchyBoundaries) {
    LevelBitmap b;
    b.set(63);
    b.set(64);
    b.set(4095);   // last bit of L1 word 0's coverage
    b.set(4096);   // first bit of L1 word 1's coverage
    b.set(65535);  // very last bit

    EXPECT_EQ(b.next_set_at_or_above(0), 63u);
    EXPECT_EQ(b.next_set_at_or_above(64), 64u);
    EXPECT_EQ(b.next_set_at_or_above(65), 4095u);
    EXPECT_EQ(b.next_set_at_or_above(4096), 4096u);
    EXPECT_EQ(b.next_set_at_or_above(4097), 65535u);
    EXPECT_EQ(b.next_set_at_or_above(65535), 65535u);

    EXPECT_EQ(b.prev_set_at_or_below(65535), 65535u);
    EXPECT_EQ(b.prev_set_at_or_below(65534), 4096u);
    EXPECT_EQ(b.prev_set_at_or_below(4095), 4095u);
    EXPECT_EQ(b.prev_set_at_or_below(4094), 64u);
    EXPECT_EQ(b.prev_set_at_or_below(63), 63u);
    EXPECT_EQ(b.prev_set_at_or_below(62), LevelBitmap::kNotFound);
}

TEST(LevelBitmap, OutOfRangeQueriesReturnNotFoundRatherThanReadingOutOfBounds) {
    LevelBitmap b;
    b.set(100);
    EXPECT_EQ(b.next_set_at_or_above(LevelBitmap::kBits), LevelBitmap::kNotFound);
    EXPECT_EQ(b.next_set_at_or_above(LevelBitmap::kBits + 1000), LevelBitmap::kNotFound);
}

TEST(LevelBitmap, AllBitsSetMakesEveryQueryTheIdentity) {
    LevelBitmap b;
    for (std::uint32_t i = 0; i < LevelBitmap::kBits; ++i) {
        b.set(i);
    }
    EXPECT_EQ(b.popcount(), LevelBitmap::kBits);
    for (const std::uint32_t i : {0u, 1u, 63u, 64u, 4095u, 4096u, 33333u, 65535u}) {
        EXPECT_EQ(b.next_set_at_or_above(i), i) << i;
        EXPECT_EQ(b.prev_set_at_or_below(i), i) << i;
    }
}

TEST(LevelBitmap, ClearingAllBitsRestoresTheEmptyState) {
    LevelBitmap b;
    for (std::uint32_t i = 0; i < LevelBitmap::kBits; i += 7) {
        b.set(i);
    }
    for (std::uint32_t i = 0; i < LevelBitmap::kBits; i += 7) {
        b.clear(i);
    }
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.popcount(), 0u);
    EXPECT_EQ(b.next_set_at_or_above(0), LevelBitmap::kNotFound);
}

TEST(LevelBitmap, ResetEmptiesEverything) {
    LevelBitmap b;
    b.set(10);
    b.set(50000);
    b.reset();
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.next_set_at_or_above(0), LevelBitmap::kNotFound);
}

// The strongest test: a std::set says what the answer is, for random set/clear
// sequences and random queries. Hierarchy-collapse bugs need a specific
// arrangement of bits to show up, which is exactly what randomization finds.
TEST(LevelBitmap, MatchesASetModelUnderRandomMutationAndQueries) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        LevelBitmap b;
        std::set<std::uint32_t> model;
        std::mt19937_64 rng(seed);

        for (int op = 0; op < 4000; ++op) {
            const std::uint32_t i =
                static_cast<std::uint32_t>(rng() % LevelBitmap::kBits);
            if (rng() % 2 == 0) {
                b.set(i);
                model.insert(i);
            } else {
                b.clear(i);
                model.erase(i);
            }
            ASSERT_EQ(b.empty(), model.empty()) << "seed " << seed << " op " << op;
            ASSERT_EQ(b.popcount(), model.size()) << "seed " << seed << " op " << op;

            const std::uint32_t q =
                static_cast<std::uint32_t>(rng() % LevelBitmap::kBits);

            const auto up = model.lower_bound(q);
            const std::uint32_t want_next =
                (up == model.end()) ? LevelBitmap::kNotFound : *up;
            ASSERT_EQ(b.next_set_at_or_above(q), want_next)
                << "seed " << seed << " op " << op << " q " << q;

            const auto after = model.upper_bound(q);
            const std::uint32_t want_prev =
                (after == model.begin()) ? LevelBitmap::kNotFound : *std::prev(after);
            ASSERT_EQ(b.prev_set_at_or_below(q), want_prev)
                << "seed " << seed << " op " << op << " q " << q;
        }
    }
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/level_bitmap.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/level_bitmap.hpp`**

```cpp
// include/ob/level_bitmap.hpp
#pragma once

// Three-level occupancy bitmap over the price ladder. This is what replaces a
// red-black tree walk with two or three ctz/clz instructions.
//
//   L2:     1 word  (16 bits used)   each bit: "the L1 word below me is non-empty"
//   L1:    16 words (1024 bits)      each bit: "the L0 word below me is non-empty"
//   L0:  1024 words (65536 bits)     each bit: "this price level has orders"
//
// Cost of next_set_at_or_above / prev_set_at_or_below is bounded at three word
// loads no matter how far the next occupied level is.
//
// THE TRAP: `~0ull << 64` is undefined behavior, and the case is reached whenever
// the index lands on a word boundary. Both mask helpers guard it explicitly.

#include <ob/types.hpp>

#include <cassert>
#include <cstdint>

namespace ob {

class LevelBitmap {
public:
    static constexpr std::uint32_t kBits = static_cast<std::uint32_t>(kLadderSize);
    static constexpr std::uint32_t kNotFound = 0xFFFF'FFFFu;

    static_assert(kBits % 64 == 0, "kBits must be a whole number of words");
    static_assert((kBits / 64) % 64 == 0, "L0 word count must be a whole L1 word");
    static_assert(kBits / 64 / 64 <= 64, "hierarchy needs exactly one L2 word");

    void set(std::uint32_t i) noexcept {
        assert(i < kBits);
        const std::uint32_t w0 = i >> 6;
        const std::uint32_t w1 = w0 >> 6;
        l0_[w0] |= bit(i & 63);
        l1_[w1] |= bit(w0 & 63);
        l2_ |= bit(w1 & 63);
    }

    void clear(std::uint32_t i) noexcept {
        assert(i < kBits);
        const std::uint32_t w0 = i >> 6;
        l0_[w0] &= ~bit(i & 63);
        if (l0_[w0] != 0) {
            return;  // the summary bits above are still correct
        }
        const std::uint32_t w1 = w0 >> 6;
        l1_[w1] &= ~bit(w0 & 63);
        if (l1_[w1] != 0) {
            return;
        }
        l2_ &= ~bit(w1 & 63);
    }

    [[nodiscard]] bool test(std::uint32_t i) const noexcept {
        assert(i < kBits);
        return (l0_[i >> 6] & bit(i & 63)) != 0;
    }

    [[nodiscard]] bool empty() const noexcept { return l2_ == 0; }

    [[nodiscard]] std::uint32_t popcount() const noexcept {
        std::uint32_t n = 0;
        for (const std::uint64_t w : l0_) {
            n += static_cast<std::uint32_t>(__builtin_popcountll(w));
        }
        return n;
    }

    // Lowest set bit at index >= i, or kNotFound.
    [[nodiscard]] std::uint32_t next_set_at_or_above(std::uint32_t i) const noexcept {
        if (i >= kBits) {
            return kNotFound;
        }
        const std::uint32_t w0 = i >> 6;
        if (const std::uint64_t m = l0_[w0] & mask_at_or_above(i & 63); m != 0) {
            return (w0 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(m));
        }
        const std::uint32_t w1 = w0 >> 6;
        if (const std::uint64_t m1 = l1_[w1] & mask_above(w0 & 63); m1 != 0) {
            const std::uint32_t nw0 =
                (w1 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(m1));
            return (nw0 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(l0_[nw0]));
        }
        if (const std::uint64_t m2 = l2_ & mask_above(w1 & 63); m2 != 0) {
            const std::uint32_t nw1 = static_cast<std::uint32_t>(__builtin_ctzll(m2));
            const std::uint32_t nw0 =
                (nw1 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(l1_[nw1]));
            return (nw0 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(l0_[nw0]));
        }
        return kNotFound;
    }

    // Highest set bit at index <= i, or kNotFound.
    [[nodiscard]] std::uint32_t prev_set_at_or_below(std::uint32_t i) const noexcept {
        if (i >= kBits) {
            i = kBits - 1;
        }
        const std::uint32_t w0 = i >> 6;
        if (const std::uint64_t m = l0_[w0] & mask_at_or_below(i & 63); m != 0) {
            return (w0 << 6) | top_bit(m);
        }
        const std::uint32_t w1 = w0 >> 6;
        if (const std::uint64_t m1 = l1_[w1] & mask_below(w0 & 63); m1 != 0) {
            const std::uint32_t pw0 = (w1 << 6) | top_bit(m1);
            return (pw0 << 6) | top_bit(l0_[pw0]);
        }
        if (const std::uint64_t m2 = l2_ & mask_below(w1 & 63); m2 != 0) {
            const std::uint32_t pw1 = top_bit(m2);
            const std::uint32_t pw0 = (pw1 << 6) | top_bit(l1_[pw1]);
            return (pw0 << 6) | top_bit(l0_[pw0]);
        }
        return kNotFound;
    }

    void reset() noexcept {
        for (std::uint64_t& w : l0_) {
            w = 0;
        }
        for (std::uint64_t& w : l1_) {
            w = 0;
        }
        l2_ = 0;
    }

private:
    static constexpr std::uint32_t kL0Words = kBits / 64;      // 1024
    static constexpr std::uint32_t kL1Words = kL0Words / 64;   // 16

    static constexpr std::uint64_t bit(std::uint32_t b) noexcept {
        return std::uint64_t{1} << b;
    }
    // Bits b..63. Safe: b is always < 64, so no shift-by-64 here.
    static constexpr std::uint64_t mask_at_or_above(std::uint32_t b) noexcept {
        return ~std::uint64_t{0} << b;
    }
    // Bits b+1..63. GUARDED: b == 63 would shift by 64, which is UB.
    static constexpr std::uint64_t mask_above(std::uint32_t b) noexcept {
        return b == 63 ? std::uint64_t{0} : (~std::uint64_t{0} << (b + 1));
    }
    // Bits 0..b. GUARDED for the same reason.
    static constexpr std::uint64_t mask_at_or_below(std::uint32_t b) noexcept {
        return b == 63 ? ~std::uint64_t{0} : ((std::uint64_t{1} << (b + 1)) - 1);
    }
    // Bits 0..b-1. GUARDED: b == 0 would shift by a negative amount.
    static constexpr std::uint64_t mask_below(std::uint32_t b) noexcept {
        return b == 0 ? std::uint64_t{0} : ((std::uint64_t{1} << b) - 1);
    }
    // Index of the highest set bit. Precondition: w != 0 (clzll(0) is UB).
    static std::uint32_t top_bit(std::uint64_t w) noexcept {
        assert(w != 0);
        return 63u - static_cast<std::uint32_t>(__builtin_clzll(w));
    }

    std::uint64_t l0_[kL0Words]{};
    std::uint64_t l1_[kL1Words]{};
    std::uint64_t l2_ = 0;
};

}  // namespace ob
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build -R LevelBitmap --output-on-failure
```

Expected: PASS, 10 tests. `MatchesASetModelUnderRandomMutationAndQueries` is the one to trust; the boundary test is the one that will actually fail first if a mask helper is wrong.

- [ ] **Step 5: Verify no undefined behavior under UBSan, which is where a bad shift shows up**

```bash
cmake -S . -B build-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON -DOB_BUILD_BENCH=ON
cmake --build build-ubsan
./build-ubsan/tests/ob_tests --gtest_filter=LevelBitmap.*
```

Expected: PASS with no UBSan diagnostics. A `shift exponent 64 is too large` report here means one of the mask guards was dropped.

- [ ] **Step 6: Commit**

```bash
git add include/ob/level_bitmap.hpp tests/test_level_bitmap.cpp tests/CMakeLists.txt
git commit -m "feat: three-level occupancy bitmap for O(1) best-price lookup"
```

---

## Task 6: The price ladder

**Files:**
- Create: `include/ob/price_ladder.hpp`, `tests/test_price_ladder.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/order_pool.hpp`, `ob/level_bitmap.hpp`.
- Produces: `ob::PriceLevel` (exactly 24 bytes); `ob::PriceLadder<Side S>` with `push_back(Ticks, Slot, OrderPool&)`, `unlink(Ticks, Slot, OrderPool&) -> Qty`, `reduce(Ticks, Qty)`, `head(Ticks) const -> Slot`, `best() const -> Ticks`, `level(Ticks) const -> const PriceLevel&`, `empty() const`, `order_count() const`, `for_each(Fn&&) const`, `reset()`.

Templated on `Side` so `best()` compiles to "highest occupied bit" for bids and "lowest occupied bit" for asks with no runtime branch.

**The one API hazard, stated here because it is the kind of thing that produces a silently wrong `total`:** `unlink` subtracts `pool.at(slot).remaining` from the level total, so it **must be called while `remaining` still holds the quantity being removed**. Zeroing `remaining` first would subtract nothing and leave the total permanently too high. `unlink` returns the quantity it removed so the caller cannot ignore it, and there is a named test for the wrong order.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_price_ladder.cpp
#include <ob/price_ladder.hpp>

#include <gtest/gtest.h>

#include <deque>
#include <map>
#include <random>

namespace {

using ob::OrderPool;
using ob::PriceLadder;
using ob::Side;
using ob::Slot;
using ob::Ticks;

// Allocates a slot and fills in the fields the ladder reads.
Slot make(OrderPool& pool, ob::OrderId id, Ticks px, ob::Qty qty, Side side) {
    const Slot s = pool.alloc();
    ob::Order& o = pool.at(s);
    o.id = id;
    o.price = px;
    o.remaining = qty;
    o.side = side;
    return s;
}

TEST(PriceLevel, IsExactlyTwentyFourBytes) {
    static_assert(sizeof(ob::PriceLevel) == 24, "PriceLevel must stay 24 bytes");
    SUCCEED();
}

TEST(PriceLadder, StartsEmpty) {
    const PriceLadder<Side::Buy> l;
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.best(), ob::kNoPrice);
    EXPECT_EQ(l.order_count(), 0u);
}

TEST(PriceLadder, PushBackEstablishesFifoOrder) {
    OrderPool pool(16);
    PriceLadder<Side::Sell> l;
    const Slot a = make(pool, 1, 10000, 10, Side::Sell);
    const Slot b = make(pool, 2, 10000, 20, Side::Sell);
    const Slot c = make(pool, 3, 10000, 30, Side::Sell);
    l.push_back(10000, a, pool);
    l.push_back(10000, b, pool);
    l.push_back(10000, c, pool);

    EXPECT_EQ(l.head(10000), a);
    EXPECT_EQ(pool.at(a).next, b);
    EXPECT_EQ(pool.at(b).next, c);
    EXPECT_EQ(pool.at(c).next, ob::kInvalidSlot);
    EXPECT_EQ(pool.at(a).prev, ob::kInvalidSlot);
    EXPECT_EQ(pool.at(c).prev, b);

    EXPECT_EQ(l.level(10000).total, 60u);
    EXPECT_EQ(l.level(10000).count, 3u);
    EXPECT_EQ(l.order_count(), 3u);
}

TEST(PriceLadder, BestIsTheHighestPriceForBidsAndLowestForAsks) {
    OrderPool pool(16);
    PriceLadder<Side::Buy> bids;
    PriceLadder<Side::Sell> asks;

    bids.push_back(10000, make(pool, 1, 10000, 10, Side::Buy), pool);
    bids.push_back(10010, make(pool, 2, 10010, 10, Side::Buy), pool);
    bids.push_back(9990, make(pool, 3, 9990, 10, Side::Buy), pool);
    EXPECT_EQ(bids.best(), 10010);

    asks.push_back(10100, make(pool, 4, 10100, 10, Side::Sell), pool);
    asks.push_back(10050, make(pool, 5, 10050, 10, Side::Sell), pool);
    asks.push_back(10200, make(pool, 6, 10200, 10, Side::Sell), pool);
    EXPECT_EQ(asks.best(), 10050);
}

TEST(PriceLadder, BothLadderExtremesWork) {
    OrderPool pool(8);
    PriceLadder<Side::Buy> bids;
    bids.push_back(ob::kMinTick, make(pool, 1, ob::kMinTick, 1, Side::Buy), pool);
    EXPECT_EQ(bids.best(), ob::kMinTick);
    bids.push_back(ob::kMaxTick, make(pool, 2, ob::kMaxTick, 1, Side::Buy), pool);
    EXPECT_EQ(bids.best(), ob::kMaxTick);
}

TEST(PriceLadder, UnlinkFromHeadMiddleAndTailKeepsTheRestIntact) {
    for (int victim = 0; victim < 3; ++victim) {
        OrderPool pool(16);
        PriceLadder<Side::Sell> l;
        Slot s[3];
        for (int i = 0; i < 3; ++i) {
            s[i] = make(pool, static_cast<ob::OrderId>(i + 1), 10000, 10, Side::Sell);
            l.push_back(10000, s[i], pool);
        }

        const ob::Qty removed = l.unlink(10000, s[victim], pool);
        EXPECT_EQ(removed, 10u) << "victim " << victim;
        EXPECT_EQ(l.level(10000).total, 20u) << "victim " << victim;
        EXPECT_EQ(l.level(10000).count, 2u) << "victim " << victim;

        // Walk what remains and confirm it is the other two, in order.
        std::vector<ob::OrderId> seen;
        for (Slot cur = l.head(10000); cur != ob::kInvalidSlot; cur = pool.at(cur).next) {
            seen.push_back(pool.at(cur).id);
        }
        std::vector<ob::OrderId> want;
        for (int i = 0; i < 3; ++i) {
            if (i != victim) {
                want.push_back(static_cast<ob::OrderId>(i + 1));
            }
        }
        EXPECT_EQ(seen, want) << "victim " << victim;
    }
}

TEST(PriceLadder, EmptyingALevelClearsItAndMovesBest) {
    OrderPool pool(16);
    PriceLadder<Side::Buy> l;
    const Slot hi = make(pool, 1, 10010, 10, Side::Buy);
    const Slot lo = make(pool, 2, 10000, 10, Side::Buy);
    l.push_back(10010, hi, pool);
    l.push_back(10000, lo, pool);
    ASSERT_EQ(l.best(), 10010);

    l.unlink(10010, hi, pool);
    EXPECT_EQ(l.best(), 10000);
    EXPECT_EQ(l.level(10010).count, 0u);
    EXPECT_EQ(l.level(10010).total, 0u);
    EXPECT_EQ(l.level(10010).head, ob::kInvalidSlot);
    EXPECT_EQ(l.level(10010).tail, ob::kInvalidSlot);

    l.unlink(10000, lo, pool);
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.best(), ob::kNoPrice);
}

TEST(PriceLadder, ReduceAdjustsTheLevelTotalForAPartialFill) {
    OrderPool pool(8);
    PriceLadder<Side::Sell> l;
    const Slot s = make(pool, 1, 10000, 100, Side::Sell);
    l.push_back(10000, s, pool);

    pool.at(s).remaining -= 30;
    l.reduce(10000, 30);
    EXPECT_EQ(l.level(10000).total, 70u);
    EXPECT_EQ(l.level(10000).count, 1u) << "a partial fill must not change the count";
    EXPECT_EQ(l.head(10000), s) << "a partially filled order keeps its place";
}

// The API hazard made explicit: unlink subtracts the order's CURRENT remaining,
// so zeroing remaining first silently leaves the total too high forever.
TEST(PriceLadder, UnlinkMustBeCalledBeforeZeroingRemaining) {
    OrderPool pool(8);
    PriceLadder<Side::Sell> l;
    const Slot s = make(pool, 1, 10000, 100, Side::Sell);
    l.push_back(10000, s, pool);
    ASSERT_EQ(l.level(10000).total, 100u);

    // Correct order: unlink while remaining is still 100.
    EXPECT_EQ(l.unlink(10000, s, pool), 100u);
    EXPECT_EQ(l.level(10000).total, 0u);
}

TEST(PriceLadder, ForEachVisitsLevelsBestToWorstAndFifoWithin) {
    OrderPool pool(32);
    PriceLadder<Side::Buy> l;
    // Deliberately inserted out of price order.
    l.push_back(10000, make(pool, 1, 10000, 10, Side::Buy), pool);
    l.push_back(10020, make(pool, 2, 10020, 10, Side::Buy), pool);
    l.push_back(10000, make(pool, 3, 10000, 10, Side::Buy), pool);
    l.push_back(10010, make(pool, 4, 10010, 10, Side::Buy), pool);

    std::vector<ob::OrderId> ids;
    std::vector<Ticks> prices;
    l.for_each(pool, [&](Ticks px, const ob::Order& o) {
        prices.push_back(px);
        ids.push_back(o.id);
    });
    // Bids: highest price first. Within 10000, id 1 arrived before id 3.
    EXPECT_EQ(prices, (std::vector<Ticks>{10020, 10010, 10000, 10000}));
    EXPECT_EQ(ids, (std::vector<ob::OrderId>{2, 4, 1, 3}));
}

TEST(PriceLadder, ResetEmptiesEverything) {
    OrderPool pool(8);
    PriceLadder<Side::Buy> l;
    l.push_back(10000, make(pool, 1, 10000, 10, Side::Buy), pool);
    l.reset();
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.best(), ob::kNoPrice);
    EXPECT_EQ(l.order_count(), 0u);
}

// Property test against a std::map of deques. Intrusive-list bugs need a specific
// arrangement, which randomization finds and hand-written cases do not.
TEST(PriceLadder, MatchesAMapOfDequesUnderRandomPushAndUnlink) {
    for (std::uint64_t seed = 1; seed <= 15; ++seed) {
        OrderPool pool(4096);
        PriceLadder<Side::Sell> l;
        std::map<Ticks, std::deque<Slot>> model;
        std::mt19937_64 rng(seed);
        std::vector<std::pair<Ticks, Slot>> live;
        ob::OrderId next_id = 1;

        for (int op = 0; op < 3000; ++op) {
            const bool do_push = live.empty() || (rng() % 3 != 0);
            if (do_push && pool.size() < pool.capacity()) {
                const Ticks px = 9950 + static_cast<Ticks>(rng() % 101);
                const Slot s = make(pool, next_id++, px, 10, Side::Sell);
                l.push_back(px, s, pool);
                model[px].push_back(s);
                live.emplace_back(px, s);
            } else if (!live.empty()) {
                const std::size_t k = static_cast<std::size_t>(rng() % live.size());
                const auto [px, s] = live[k];
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
                l.unlink(px, s, pool);
                pool.free(s);
                auto& dq = model[px];
                dq.erase(std::find(dq.begin(), dq.end(), s));
                if (dq.empty()) {
                    model.erase(px);
                }
            }

            // best() must agree with the model's lowest occupied price.
            const Ticks want_best = model.empty() ? ob::kNoPrice : model.begin()->first;
            ASSERT_EQ(l.best(), want_best) << "seed " << seed << " op " << op;
            ASSERT_EQ(l.order_count(), live.size()) << "seed " << seed << " op " << op;
        }

        // Full traversal must reproduce the model exactly, in order.
        std::vector<Slot> got;
        l.for_each(pool, [&](Ticks, const ob::Order& o) {
            got.push_back(static_cast<Slot>(&o - &pool.at(0)));
        });
        std::vector<Slot> want;
        for (const auto& [px, dq] : model) {
            want.insert(want.end(), dq.begin(), dq.end());
        }
        ASSERT_EQ(got, want) << "seed " << seed;
    }
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/price_ladder.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/price_ladder.hpp`**

```cpp
// include/ob/price_ladder.hpp
#pragma once

// Flat array of price levels indexed by tick, plus a three-level occupancy bitmap.
//
// Memory: 65,536 levels x 24 B = 1.5 MB per side. The allocation does not fit in
// L2, but the TOUCHED set does: activity clusters within a few dozen ticks of the
// touch, so the hot footprint is a few kilobytes of contiguous memory that the
// prefetcher handles well. That is the actual argument for this layout, and it is
// the reason a tree keyed by price loses despite allocating far less.
//
// Templated on Side so best() becomes "highest occupied bit" or "lowest occupied
// bit" at compile time, with no runtime branch.

#include <ob/level_bitmap.hpp>
#include <ob/order_pool.hpp>

#include <cassert>
#include <vector>

namespace ob {

struct PriceLevel {
    Slot          head  = kInvalidSlot;  // 4  FIFO front: the oldest order
    Slot          tail  = kInvalidSlot;  // 4  FIFO back: the newest order
    QtySum        total = 0;             // 8  sum of remaining qty at this level
    std::uint32_t count = 0;             // 4  order count, for L2 output
    std::uint32_t pad   = 0;             // 4
};
static_assert(sizeof(PriceLevel) == 24, "PriceLevel must stay 24 bytes");

template <Side S>
class PriceLadder {
public:
    PriceLadder() : levels_(kLadderSize) {}

    void push_back(Ticks px, Slot s, OrderPool& pool) noexcept {
        assert(price_in_range(px) && "caller must range-check the price FIRST");
        PriceLevel& lv = levels_[idx(px)];
        Order& o = pool.at(s);

        o.next = kInvalidSlot;
        o.prev = lv.tail;
        if (lv.tail != kInvalidSlot) {
            pool.at(lv.tail).next = s;
        } else {
            lv.head = s;
        }
        lv.tail = s;

        lv.total += o.remaining;
        ++lv.count;
        ++orders_;
        occupied_.set(idx32(px));
    }

    // Removes `s` from its level and returns the quantity removed.
    //
    // MUST be called while pool.at(s).remaining still holds the quantity being
    // removed. Zeroing remaining first would subtract nothing and leave the level
    // total permanently too high, and nothing else would notice until an invariant
    // check. The return value exists so the caller cannot quietly ignore it.
    [[nodiscard]] Qty unlink(Ticks px, Slot s, OrderPool& pool) noexcept {
        assert(price_in_range(px));
        PriceLevel& lv = levels_[idx(px)];
        Order& o = pool.at(s);
        const Qty removed = o.remaining;

        if (o.prev != kInvalidSlot) {
            pool.at(o.prev).next = o.next;
        } else {
            lv.head = o.next;
        }
        if (o.next != kInvalidSlot) {
            pool.at(o.next).prev = o.prev;
        } else {
            lv.tail = o.prev;
        }
        o.next = kInvalidSlot;
        o.prev = kInvalidSlot;

        assert(lv.total >= removed && "level total underflow");
        lv.total -= removed;
        assert(lv.count > 0);
        --lv.count;
        --orders_;
        if (lv.count == 0) {
            assert(lv.head == kInvalidSlot && lv.tail == kInvalidSlot);
            assert(lv.total == 0);
            occupied_.clear(idx32(px));
        }
        return removed;
    }

    // Partial fill: the order stays where it is and keeps its time priority.
    void reduce(Ticks px, Qty by) noexcept {
        assert(price_in_range(px));
        PriceLevel& lv = levels_[idx(px)];
        assert(lv.total >= by && "level total underflow");
        lv.total -= by;
    }

    [[nodiscard]] Slot head(Ticks px) const noexcept {
        assert(price_in_range(px));
        return levels_[idx(px)].head;
    }

    [[nodiscard]] const PriceLevel& level(Ticks px) const noexcept {
        assert(price_in_range(px));
        return levels_[idx(px)];
    }

    // kNoPrice when this side is empty. Two or three word loads, no branch on Side.
    [[nodiscard]] Ticks best() const noexcept {
        std::uint32_t i;
        if constexpr (S == Side::Buy) {
            i = occupied_.prev_set_at_or_below(LevelBitmap::kBits - 1);
        } else {
            i = occupied_.next_set_at_or_above(0);
        }
        return i == LevelBitmap::kNotFound ? kNoPrice : px_of(i);
    }

    [[nodiscard]] bool empty() const noexcept { return occupied_.empty(); }
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_; }

    // Visits every resting order: levels best-to-worst, FIFO within each level.
    // Used by the invariant checker and the L2 publisher.
    template <class Fn>
    void for_each(const OrderPool& pool, Fn&& fn) const {
        std::uint32_t i;
        if constexpr (S == Side::Buy) {
            i = occupied_.prev_set_at_or_below(LevelBitmap::kBits - 1);
        } else {
            i = occupied_.next_set_at_or_above(0);
        }
        while (i != LevelBitmap::kNotFound) {
            const Ticks px = px_of(i);
            for (Slot cur = levels_[i].head; cur != kInvalidSlot;
                 cur = pool.at(cur).next) {
                fn(px, pool.at(cur));
            }
            if constexpr (S == Side::Buy) {
                if (i == 0) {
                    break;
                }
                i = occupied_.prev_set_at_or_below(i - 1);
            } else {
                if (i + 1 >= LevelBitmap::kBits) {
                    break;
                }
                i = occupied_.next_set_at_or_above(i + 1);
            }
        }
    }

    void reset() noexcept {
        for (PriceLevel& lv : levels_) {
            lv = PriceLevel{};
        }
        occupied_.reset();
        orders_ = 0;
    }

private:
    static std::size_t idx(Ticks px) noexcept {
        return static_cast<std::size_t>(px - kMinTick);
    }
    static std::uint32_t idx32(Ticks px) noexcept {
        return static_cast<std::uint32_t>(px - kMinTick);
    }
    static Ticks px_of(std::uint32_t i) noexcept {
        return static_cast<Ticks>(i) + kMinTick;
    }

    std::vector<PriceLevel> levels_;
    LevelBitmap             occupied_;
    std::size_t             orders_ = 0;
};

}  // namespace ob
```

Note `for_each` takes the pool because the ladder stores indices, not orders. The test's `&o - &pool.at(0)` slot recovery relies on `Order` being contiguous in the arena, which it is.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build -R "PriceLa|PriceLevel" --output-on-failure
```

Expected: PASS, 11 tests. `MatchesAMapOfDequesUnderRandomPushAndUnlink` runs 45,000 operations across 15 seeds and checks `best()` after every one.

- [ ] **Step 5: Commit**

```bash
git add include/ob/price_ladder.hpp tests/test_price_ladder.cpp tests/CMakeLists.txt
git commit -m "feat: flat price ladder with bitmap-backed best-price lookup

Templated on Side so best() resolves to highest-or-lowest occupied bit at
compile time. unlink() returns the removed quantity because it must be called
before the order's remaining is zeroed, and returning it stops that being
silently ignored."
```

---

## Task 7: FastEngine

**Files:**
- Create: `include/ob/fast_engine.hpp`, `tests/test_fast_engine.cpp`
- Modify: `include/ob/price_ladder.hpp` (add `for_each_level`), `include/ob/order_pool.hpp` (add `free_list_length`), `tests/test_edge_cases.cpp` (**add `FastEngine` to `EngineTypes`**), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/price_ladder.hpp`, `ob/id_index.hpp`, `ob/engine_concept.hpp`, `ob/invariants.hpp`.
- Produces: `ob::FastEngine` with `static constexpr bool kTracksArrival = false`, `struct Config { std::size_t order_capacity = 1'000'000; }`, `explicit FastEngine(Config = {})`, `submit(const Command&, EventBuffer&) noexcept`, `best_bid()`, `best_ask()`, `live_order_count()`, `for_each_resting(Fn&&) const`, `check_internal_invariants() const -> InvariantResult`, `reset()`. Also `PriceLadder<S>::for_each_level(Fn&&) const` and `OrderPool::free_list_length() const`.

**The payoff of Phase 1's design lands in this task:** adding one type to `EngineTypes` runs all 40 edge cases against `FastEngine` with no new test code. If the logic diverges from `ReferenceEngine` anywhere, those cases say exactly where.

`FastEngine`'s FOK pre-scan is genuinely cheaper than the reference's: it sums `PriceLevel::total` per level rather than walking individual orders, so it is O(levels) instead of O(orders). That the two agree is precisely what differential testing establishes.

- [ ] **Step 1: Add `for_each_level` to `PriceLadder` and `free_list_length` to `OrderPool`**

In `include/ob/price_ladder.hpp`, alongside `for_each`:

```cpp
    // Visits (price, level) best-to-worst, stopping early when `fn` returns false.
    // Used by the FOK pre-scan (which needs level totals, not individual orders)
    // and by the L2 publisher in Phase 3.
    template <class Fn>
    void for_each_level(Fn&& fn) const {
        std::uint32_t i;
        if constexpr (S == Side::Buy) {
            i = occupied_.prev_set_at_or_below(LevelBitmap::kBits - 1);
        } else {
            i = occupied_.next_set_at_or_above(0);
        }
        while (i != LevelBitmap::kNotFound) {
            if (!fn(px_of(i), levels_[i])) {
                return;
            }
            if constexpr (S == Side::Buy) {
                if (i == 0) {
                    return;
                }
                i = occupied_.prev_set_at_or_below(i - 1);
            } else {
                if (i + 1 >= LevelBitmap::kBits) {
                    return;
                }
                i = occupied_.next_set_at_or_above(i + 1);
            }
        }
    }
```

In `include/ob/order_pool.hpp`:

```cpp
    // Debug helper for the invariant checker. Returns the free-list length, or
    // SIZE_MAX if the walk exceeds capacity, which means the list has a cycle.
    [[nodiscard]] std::size_t free_list_length() const noexcept {
        std::size_t n = 0;
        Slot cur = free_head_;
        while (cur != kInvalidSlot) {
            if (n > slots_.size()) {
                return static_cast<std::size_t>(-1);  // cycle
            }
            cur = slots_[cur].next;
            ++n;
        }
        return n;
    }
```

- [ ] **Step 2: Write the failing test**

```cpp
// tests/test_fast_engine.cpp
#include "model/scenario_gen.hpp"

#include <ob/fast_engine.hpp>
#include <ob/invariants.hpp>

#include <gtest/gtest.h>

namespace {

using ob::OrderType;
using ob::Side;

std::vector<ob::Event> run_one(ob::FastEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

void feed(ob::FastEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

TEST(FastEngine, SatisfiesTheEngineAndInspectableConcepts) {
    static_assert(ob::Engine<ob::FastEngine>);
    static_assert(ob::Inspectable<ob::FastEngine>);
    static_assert(ob::FastEngine::kTracksArrival == false);
    SUCCEED();
}

TEST(FastEngine, TheSpecWorkedExample) {
    ob::FastEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10050, 300));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10050, 100));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 10100, 200));

    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10050, 350));
    ASSERT_EQ(ev.size(), 5u);
    EXPECT_EQ(ev[1].maker_id, 1u);
    EXPECT_EQ(ev[1].price, 10050);
    EXPECT_EQ(ev[1].qty, 300u);
    EXPECT_EQ(ev[3].maker_id, 2u);
    EXPECT_EQ(ev[3].qty, 50u);
    EXPECT_EQ(e.best_ask(), 10050);
}

// The generic invariants, which FastEngine must satisfy exactly as the reference
// does. kTracksArrival == false means the FIFO-by-arrival check is compiled out;
// FIFO is covered by the internal structural check and by Task 8.
TEST(FastEngine, GenericInvariantsHoldAcrossRandomStreams) {
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    for (std::uint64_t seed = 1; seed <= 15; ++seed) {
        ob::FastEngine e(ob::FastEngine::Config{4096});
        const auto stream = obtest::generate_stream(seed, 3000, obtest::GenConfig{});
        for (std::size_t i = 0; i < stream.size(); ++i) {
            buf.clear();
            e.submit(stream[i], buf);
            ASSERT_GE(buf.size(), 1u) << "seed " << seed << " op " << i;
            const auto r = ob::check_invariants(e);
            ASSERT_TRUE(r.ok) << "seed " << seed << " op " << i << ": " << r.failure;
        }
    }
}

// The structural checks only FastEngine can make: intrusive list integrity, the
// bitmap agreeing with the levels, the index agreeing with the pool, and the free
// list being acyclic and disjoint from the live set.
TEST(FastEngine, InternalInvariantsHoldAcrossRandomStreams) {
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    for (std::uint64_t seed = 1; seed <= 15; ++seed) {
        ob::FastEngine e(ob::FastEngine::Config{4096});
        const auto stream = obtest::generate_stream(seed, 3000, obtest::GenConfig{});
        for (std::size_t i = 0; i < stream.size(); ++i) {
            buf.clear();
            e.submit(stream[i], buf);
            const auto r = e.check_internal_invariants();
            ASSERT_TRUE(r.ok) << "seed " << seed << " op " << i << ": " << r.failure;
        }
    }
}

// Spec E39 on the real structures: exhaustion rejects, and a cancel frees a slot.
TEST(FastEngine, CapacityExhaustionRejectsThenRecovers) {
    ob::FastEngine e(ob::FastEngine::Config{4});
    for (ob::OrderId id = 1; id <= 4; ++id) {
        ASSERT_EQ(run_one(e, ob::make_new(id, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
                  ob::EventType::Accepted);
    }
    const auto full = run_one(e, ob::make_new(5, Side::Buy, OrderType::Limit, 10000, 10));
    ASSERT_EQ(full.size(), 1u);
    EXPECT_EQ(full[0].reject, ob::RejectReason::EngineCapacity);

    feed(e, ob::make_cancel(1));
    EXPECT_EQ(run_one(e, ob::make_new(6, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
              ob::EventType::Accepted);
}

// A Market/Ioc/Fok taker must never consume a pool slot: it never rests.
TEST(FastEngine, NonRestingOrderTypesDoNotConsumePoolSlots) {
    ob::FastEngine e(ob::FastEngine::Config{2});
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 10));
    ASSERT_EQ(e.live_order_count(), 1u);

    // A market order that fully fills the resting one frees the maker's slot.
    feed(e, ob::make_new(2, Side::Buy, OrderType::Market, ob::kNoPrice, 10));
    EXPECT_EQ(e.live_order_count(), 0u);

    // And an unfillable IOC into an empty book consumes nothing at all.
    feed(e, ob::make_new(3, Side::Buy, OrderType::Ioc, 10000, 10));
    EXPECT_EQ(e.live_order_count(), 0u);
}

TEST(FastEngine, ResetMakesItIndistinguishableFromAFreshEngine) {
    const auto stream = obtest::generate_stream(9, 2000, obtest::GenConfig{});

    ob::FastEngine reused(ob::FastEngine::Config{4096});
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());
    for (const ob::Command& c : stream) {
        buf.clear();
        reused.submit(c, buf);
    }
    reused.reset();

    std::vector<ob::Event> after;
    for (const ob::Command& c : stream) {
        buf.clear();
        reused.submit(c, buf);
        after.insert(after.end(), buf.begin(), buf.end());
    }

    ob::FastEngine fresh(ob::FastEngine::Config{4096});
    std::vector<ob::Event> want;
    for (const ob::Command& c : stream) {
        buf.clear();
        fresh.submit(c, buf);
        want.insert(want.end(), buf.begin(), buf.end());
    }

    ASSERT_EQ(after.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        ASSERT_EQ(after[i], want[i]) << "divergence at event " << i;
    }
}

// The FOK pre-scan sums level totals rather than walking orders, so it takes a
// different code path from the reference. Same answer, cheaper. Spec E36.
TEST(FastEngine, FokPreScanOneUnitShortMutatesNothing) {
    ob::FastEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 60));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10010, 39));

    const auto before_ask = e.best_ask();
    const auto before_live = e.live_order_count();

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Fok, 10010, 100));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[1].cancel, ob::CancelReason::Unfillable);
    EXPECT_EQ(e.best_ask(), before_ask);
    EXPECT_EQ(e.live_order_count(), before_live);

    const auto r = e.check_internal_invariants();
    EXPECT_TRUE(r.ok) << r.failure;
}

}  // namespace
```

- [ ] **Step 3: Add `FastEngine` to the shared edge-case suite**

This is the one-line change Phase 1 was built for. In `tests/test_edge_cases.cpp`:

```cpp
#include <ob/fast_engine.hpp>
#include <ob/reference_engine.hpp>

// ...

using EngineTypes = ::testing::Types<ob::ReferenceEngine, ob::FastEngine>;
```

All 40 cases and all four typed tests now run against both engines.

- [ ] **Step 4: Add the test file to the build, run, verify it fails**

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/fast_engine.hpp' file not found`.

- [ ] **Step 5: Write `include/ob/fast_engine.hpp`**

```cpp
// include/ob/fast_engine.hpp
#pragma once

// The matching engine.
//
// After construction: no allocation, no exceptions, no syscalls, no virtual
// dispatch. Every branch of submit() either completes or returns a typed reason,
// and a rejected command leaves the book bit-for-bit unchanged.
//
// The logic here mirrors ReferenceEngine deliberately, line for line where it can.
// Any divergence is a bug, and Task 8 is what finds it: differential testing over
// generated streams, not inspection.

#include <ob/engine_concept.hpp>
#include <ob/id_index.hpp>
#include <ob/invariants.hpp>
#include <ob/price_ladder.hpp>

#include <algorithm>
#include <cassert>

namespace ob {

class FastEngine {
public:
    // Order is exactly 32 bytes with no room for an arrival sequence, and a
    // parallel array would put a cold-line store on the hot path for the sake of
    // a debug-only check. FIFO order is established instead by
    // check_internal_invariants() and by differential testing.
    static constexpr bool kTracksArrival = false;

    struct Config {
        std::size_t order_capacity = 1'000'000;
    };

    explicit FastEngine(Config cfg = {})
        : pool_(cfg.order_capacity), index_(cfg.order_capacity) {}

    void submit(const Command& c, EventBuffer& out) noexcept {
        if (c.type == CommandType::Cancel) {
            submit_cancel(c, out);
        } else {
            submit_new(c, out);
        }
    }

    [[nodiscard]] Ticks best_bid() const noexcept { return bids_.best(); }
    [[nodiscard]] Ticks best_ask() const noexcept { return asks_.best(); }

    // Only resting orders hold a pool slot: takers are matched without allocating.
    [[nodiscard]] std::size_t live_order_count() const noexcept { return pool_.size(); }

    template <class Fn>
    void for_each_resting(Fn&& fn) const {
        bids_.for_each(pool_, [&fn](Ticks px, const Order& o) {
            fn(RestingOrder{Side::Buy, px, o.id, o.remaining, 0});
        });
        asks_.for_each(pool_, [&fn](Ticks px, const Order& o) {
            fn(RestingOrder{Side::Sell, px, o.id, o.remaining, 0});
        });
    }

    void reset() noexcept {
        bids_.reset();
        asks_.reset();
        pool_.reset();
        index_.reset();
        high_water_ = 0;
        seq_ = 0;
    }

    // Structural checks the generic checker cannot make, because they are about
    // this engine's representation rather than the book's semantics.
    [[nodiscard]] InvariantResult check_internal_invariants() const {
        if (const auto r = check_side(bids_); !r.ok) {
            return r;
        }
        if (const auto r = check_side(asks_); !r.ok) {
            return r;
        }
        if (index_.size() != pool_.size()) {
            return {false, "id index size disagrees with the pool's live count"};
        }
        const std::size_t freelen = pool_.free_list_length();
        if (freelen == static_cast<std::size_t>(-1)) {
            return {false, "free list has a cycle, which means a double free"};
        }
        if (freelen + pool_.size() != pool_.capacity()) {
            return {false, "free list plus live count does not cover the pool"};
        }
        return kInvariantsHold;
    }

private:
    [[nodiscard]] Seq next_seq() noexcept { return seq_++; }

    Event base(EventType t, OrderId id) noexcept {
        Event e{};
        e.seq = next_seq();
        e.type = t;
        e.order_id = id;
        return e;
    }

    void emit_rejected(EventBuffer& out, OrderId id, RejectReason r) noexcept {
        Event e = base(EventType::Rejected, id);
        e.reject = r;
        out.push(e);
    }

    Event trade_event(OrderId taker, OrderId maker, Ticks px, Qty qty) noexcept {
        Event e = base(EventType::Trade, taker);
        e.maker_id = maker;
        e.price = px;
        e.qty = qty;
        return e;
    }

    [[nodiscard]] bool would_cross(const Command& c) const noexcept {
        const Ticks opp = (c.side == Side::Buy) ? asks_.best() : bids_.best();
        return opp != kNoPrice && crosses(c.side, c.price, opp);
    }

    // Identical order and identical rules to ReferenceEngine::validate_new. Any
    // difference here is a differential-test failure waiting to happen.
    [[nodiscard]] RejectReason validate_new(const Command& c) const noexcept {
        if (!qty_valid(c.qty)) {
            return RejectReason::InvalidQuantity;
        }
        if (c.order_type != OrderType::Market && !price_in_range(c.price)) {
            return RejectReason::PriceOutOfRange;
        }
        if (c.id <= high_water_) {
            return RejectReason::DuplicateOrderId;
        }
        const bool can_rest =
            c.order_type == OrderType::Limit || c.order_type == OrderType::PostOnly;
        // The pool always binds before the index: index capacity is
        // next_pow2(2 * order_capacity) with a ceiling of half that, which is
        // always >= order_capacity. The index term is kept anyway, because it costs
        // one comparison and removes the need to trust that argument forever.
        if (can_rest && (pool_.full() || index_.full())) {
            return RejectReason::EngineCapacity;
        }
        if (c.order_type == OrderType::PostOnly && would_cross(c)) {
            return RejectReason::WouldCross;
        }
        return RejectReason::None;
    }

    // Non-mutating. Sums LEVEL totals rather than individual orders, so this is
    // O(levels) where the reference engine is O(orders). Same answer; the
    // differential test is what establishes that.
    template <Side OppSide>
    [[nodiscard]] QtySum fillable_qty(const PriceLadder<OppSide>& book,
                                      const Command& c) const noexcept {
        QtySum total = 0;
        book.for_each_level([&](Ticks px, const PriceLevel& lv) {
            if (c.order_type != OrderType::Market && !crosses(c.side, c.price, px)) {
                return false;  // stop: prices only get worse from here
            }
            total += lv.total;
            return total < c.qty;  // stop early once it is provably enough
        });
        return total;
    }

    [[nodiscard]] bool fok_is_fillable(const Command& c) const noexcept {
        const QtySum available = (c.side == Side::Buy) ? fillable_qty(asks_, c)
                                                      : fillable_qty(bids_, c);
        return available >= c.qty;
    }

    template <Side OppSide>
    void match_into(PriceLadder<OppSide>& book, const Command& c, Qty& remaining,
                    EventBuffer& out) noexcept {
        while (remaining > 0) {
            const Ticks level_px = book.best();
            if (level_px == kNoPrice) {
                break;
            }
            // A Market order ignores price; everything else must cross.
            if (c.order_type != OrderType::Market &&
                !crosses(c.side, c.price, level_px)) {
                break;
            }

            while (remaining > 0) {
                const Slot s = book.head(level_px);
                if (s == kInvalidSlot) {
                    break;  // level drained; the outer loop re-reads best()
                }
                Order& maker = pool_.at(s);
                const Qty fill = std::min(remaining, maker.remaining);
                remaining -= fill;
                out.push(trade_event(c.id, maker.id, level_px, fill));

                if (fill == maker.remaining) {
                    const OrderId maker_id = maker.id;
                    // unlink BEFORE the slot is freed and before remaining is
                    // touched: it subtracts the order's current remaining.
                    const Qty removed = book.unlink(level_px, s, pool_);
                    assert(removed == fill);
                    (void)removed;
                    out.push(base(EventType::Filled, maker_id));
                    const bool erased = index_.erase(maker_id);
                    assert(erased && "a resting order was missing from the index");
                    (void)erased;
                    pool_.free(s);
                } else {
                    maker.remaining -= fill;
                    book.reduce(level_px, fill);
                }
            }
        }
    }

    void submit_new(const Command& c, EventBuffer& out) noexcept {
        const RejectReason r = validate_new(c);
        if (r != RejectReason::None) {
            emit_rejected(out, c.id, r);
            return;
        }
        out.push(base(EventType::Accepted, c.id));
        high_water_ = c.id;  // only an Accepted advances the mark

        if (c.order_type == OrderType::Fok && !fok_is_fillable(c)) {
            Event e = base(EventType::Cancelled, c.id);
            e.cancel = CancelReason::Unfillable;
            e.qty = c.qty;
            out.push(e);
            return;
        }

        Qty remaining = c.qty;

        if (c.order_type != OrderType::PostOnly) {
            if (c.side == Side::Buy) {
                match_into(asks_, c, remaining, out);
            } else {
                match_into(bids_, c, remaining, out);
            }
        }

        if (remaining == 0) {
            out.push(base(EventType::Filled, c.id));
            return;
        }

        switch (c.order_type) {
            case OrderType::Limit:
            case OrderType::PostOnly: {
                const Slot s = pool_.alloc();
                assert(s != kInvalidSlot && "capacity was checked during validation");
                Order& o = pool_.at(s);
                o.id = c.id;
                o.price = c.price;
                o.remaining = remaining;
                o.side = c.side;
                o.flags = 0;

                const bool inserted = index_.insert(c.id, s);
                assert(inserted && "index rejected an id validation had cleared");
                (void)inserted;

                if (c.side == Side::Buy) {
                    bids_.push_back(c.price, s, pool_);
                } else {
                    asks_.push_back(c.price, s, pool_);
                }
                return;  // rests; Accepted already conveyed that
            }

            case OrderType::Market:
            case OrderType::Ioc: {
                Event e = base(EventType::Cancelled, c.id);
                e.cancel = (remaining == c.qty) ? CancelReason::NoLiquidity
                                                : CancelReason::IocRemainder;
                e.qty = remaining;
                out.push(e);
                return;
            }

            case OrderType::Fok:
                assert(false && "Fok reached the remainder branch: the pre-scan "
                                "disagreed with the match loop");
                return;
        }
    }

    void submit_cancel(const Command& c, EventBuffer& out) noexcept {
        const Slot s = index_.find(c.id);
        if (s == kInvalidSlot) {
            // Never existed (E18), already filled (E19) or already cancelled (E20).
            emit_rejected(out, c.id, RejectReason::UnknownOrderId);
            return;
        }
        const Order& o = pool_.at(s);
        const Ticks px = o.price;
        const Side side = o.side;

        const Qty removed = (side == Side::Buy) ? bids_.unlink(px, s, pool_)
                                                : asks_.unlink(px, s, pool_);
        const bool erased = index_.erase(c.id);
        assert(erased);
        (void)erased;
        pool_.free(s);

        Event e = base(EventType::Cancelled, c.id);
        e.cancel = CancelReason::UserRequested;
        e.qty = removed;
        e.price = px;
        out.push(e);
    }

    template <Side S>
    [[nodiscard]] InvariantResult check_side(const PriceLadder<S>& book) const {
        InvariantResult result = kInvariantsHold;
        book.for_each_level([&](Ticks px, const PriceLevel& lv) {
            if (lv.count == 0) {
                result = {false, "occupancy bitmap set for a level with no orders"};
                return false;
            }
            std::uint32_t walked = 0;
            QtySum sum = 0;
            Slot prev = kInvalidSlot;
            Slot cur = lv.head;
            while (cur != kInvalidSlot) {
                if (walked > lv.count) {
                    result = {false, "level FIFO list is longer than its count, "
                                     "which means it has a cycle"};
                    return false;
                }
                const Order& o = pool_.at(cur);
                if (o.id == 0) {
                    result = {false, "a freed slot is still linked into a level"};
                    return false;
                }
                if (o.price != px) {
                    result = {false, "an order is linked at a price it does not hold"};
                    return false;
                }
                if (o.prev != prev) {
                    result = {false, "level FIFO prev link is inconsistent"};
                    return false;
                }
                if (index_.find(o.id) != cur) {
                    result = {false, "id index does not map a resting order to its slot"};
                    return false;
                }
                sum += o.remaining;
                prev = cur;
                cur = o.next;
                ++walked;
            }
            if (prev != lv.tail) {
                result = {false, "level tail is not the last node in its FIFO list"};
                return false;
            }
            if (walked != lv.count) {
                result = {false, "level count disagrees with its FIFO list length"};
                return false;
            }
            if (sum != lv.total) {
                result = {false, "level total disagrees with the sum of its orders"};
                return false;
            }
            return true;
        });
        return result;
    }

    OrderPool             pool_;
    IdIndex               index_;
    PriceLadder<Side::Buy>  bids_;
    PriceLadder<Side::Sell> asks_;
    OrderId               high_water_ = 0;
    Seq                   seq_ = 0;
};

static_assert(Engine<FastEngine>);
static_assert(Inspectable<FastEngine>);

}  // namespace ob
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -25
```

Expected: PASS. The 40 edge cases now run against both engines, so the suite grows by 4 typed tests worth of coverage without new test code. **If any edge case passes for `ReferenceEngine` and fails for `FastEngine`, fix `FastEngine`, never the case.** The table is the specification.

- [ ] **Step 7: Verify memory safety and absence of undefined behavior**

The flat ladder is indexed by a caller-supplied price, so this is the task where an out-of-bounds write would appear.

```bash
cmake --build build-ubsan && ./build-ubsan/tests/ob_tests 2>&1 | tail -20
```

Expected: PASS with no ASan or UBSan diagnostics. A heap-buffer-overflow in `PriceLadder::idx` means a price reached the index without being range-checked; fix the validation order, not the assert.

- [ ] **Step 8: Commit**

```bash
git add include/ob/fast_engine.hpp include/ob/price_ladder.hpp include/ob/order_pool.hpp tests/test_fast_engine.cpp tests/test_edge_cases.cpp tests/CMakeLists.txt
git commit -m "feat: FastEngine on flat ladder, arena and O(1) id index

Adding FastEngine to EngineTypes runs all 40 edge cases against it with no new
test code, which was the point of Phase 1's concept-based test design.

The FOK pre-scan sums level totals rather than walking orders, so it is
O(levels) where the reference is O(orders). That the two agree is established
by differential testing in Task 8, not by inspection."
```

---

## Task 8: Differential testing against the reference

**Files:**
- Create: `tests/test_differential.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/reference_engine.hpp`, `ob/fast_engine.hpp`, `tests/model/scenario_gen.hpp`.
- Produces: no new API. This is spec success criterion S1 and edge case E45.

**This is the most valuable test in the project.** Hand-written cases cover what I thought of; this covers what I did not. The shrinker is what makes it usable: a divergence at operation 4,000,000 is undebuggable, and the same divergence reduced to six commands is a test you commit.

- [ ] **Step 1: Write the test**

```cpp
// tests/test_differential.cpp
#include "model/scenario_gen.hpp"

#include <ob/fast_engine.hpp>
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>

namespace {

// Runs a stream through both engines and returns the index of the first differing
// event, or SIZE_MAX when they agree completely.
std::size_t first_divergence(const std::vector<ob::Command>& stream,
                            std::string* detail = nullptr) {
    const std::size_t cap = 64 * 1024;
    ob::ReferenceEngine ref(cap);
    ob::FastEngine fast(ob::FastEngine::Config{cap});

    std::vector<ob::Event> rs(8192), fs(8192);
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
    return static_cast<std::size_t>(-1);
}

bool diverges(const std::vector<ob::Command>& stream) {
    return first_divergence(stream) != static_cast<std::size_t>(-1);
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
               << c.price << ", " << c.qty << ", OrderType::"
               << static_cast<int>(c.order_type) << "),\n";
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
        if (at == static_cast<std::size_t>(-1)) {
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
// bitmap hierarchy and the best-price cursor are most likely to disagree.
TEST(Differential, EnginesAgreeOnNarrowBooksWithDeepSweeps) {
    obtest::GenConfig cfg;
    cfg.half_width = 3;      // only 7 price levels: everything crosses
    cfg.max_qty = 500;
    cfg.cancel_pct = 15;

    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const auto stream = obtest::generate_stream(seed_base() + seed, 20'000, cfg);
        std::string detail;
        ASSERT_EQ(first_divergence(stream, &detail), static_cast<std::size_t>(-1))
            << "seed " << seed << "\n" << detail;
    }
}

// The realistic shape: mostly cancels, wide book, little crossing. This is the
// workload the benchmarks report on, so it is the one that must be right.
TEST(Differential, EnginesAgreeOnCancelHeavyWideBooks) {
    obtest::GenConfig cfg;
    cfg.half_width = 400;
    cfg.cancel_pct = 88;

    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const auto stream = obtest::generate_stream(seed_base() + 1000 + seed, 20'000, cfg);
        std::string detail;
        ASSERT_EQ(first_divergence(stream, &detail), static_cast<std::size_t>(-1))
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
            ASSERT_EQ(first_divergence(stream, &detail), static_cast<std::size_t>(-1))
                << "seed " << seed << "\n" << detail;
        }
    }
}

// The shrinker has to work for the above failures to be actionable, so it is
// tested here against an injected divergence predicate rather than only in Phase 1.
TEST(Differential, ShrinkerReducesAnInjectedDivergence) {
    const auto stream = obtest::generate_stream(77, 5000, obtest::GenConfig{});
    // "Diverges" iff the stream still contains a cancel command.
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
```

- [ ] **Step 2: Add to the build and run the default-size sweep**

```bash
cmake --build build && ctest --test-dir build -R Differential --output-on-failure
```

Expected: PASS, 5 tests, roughly 1.4 million operations at the default size.

- [ ] **Step 3: Run the full 10^7-operation sweep**

```bash
OB_DIFF_OPS=10000000 ./build/tests/ob_tests --gtest_filter=Differential.EnginesAgreeOnGeneratedStreams
```

Expected: PASS, 500 streams of 20,000 operations. This is spec success criterion S1. Expect it to take minutes; if it fails, **do not debug the raw stream** — the failure message already contains a shrunk reproducer, so commit that as an edge case first and debug from there.

- [ ] **Step 4: Commit**

```bash
git add tests/test_differential.cpp tests/CMakeLists.txt
git commit -m "test: differential testing of FastEngine against the reference oracle

Compares event streams AND book state after every command, across four
workload shapes including narrow-book deep sweeps and both ladder extremes.
On divergence the stream is shrunk by delta debugging and printed as
compilable C++, so a failure at operation 4,000,000 becomes a committed
regression case in about a minute. Scales via OB_DIFF_OPS; CI runs 10^7."
```

---

## Task 9: Coverage-guided fuzzing

**Files:**
- Create: `fuzz/fuzz_differential.cpp`, `fuzz/CMakeLists.txt`, `fuzz/corpus/` (seed corpus)
- Modify: `CMakeLists.txt` (add `fuzz` subdirectory behind `OB_BUILD_FUZZ`)

**Interfaces:**
- Consumes: `ob/reference_engine.hpp`, `ob/fast_engine.hpp`, `ob/invariants.hpp`.
- Produces: the `ob_fuzz_differential` executable. No library API.

Differential testing explores the space my generator describes. The fuzzer explores the space the *code* has branches for, which is not the same space, and it is the one that finds the case nobody modelled. It decodes raw bytes as commands, so it will happily produce order ID 0, prices at `INT32_MIN`, quantities of 0, and non-monotonic IDs — exactly the inputs that attack the flat ladder's bounds checking.

- [ ] **Step 1: Write the fuzz target**

```cpp
// fuzz/fuzz_differential.cpp
//
// libFuzzer target. Decodes the input as a command stream and asserts that
// ReferenceEngine and FastEngine agree, event for event, plus every invariant.
//
// The decoder deliberately does NOT sanitise: prices span the full int32 range,
// quantities include 0, ids are arbitrary and non-monotonic. Producing inputs the
// engine must REJECT is the point, because that is where the flat ladder's bounds
// checking lives.

#include <ob/fast_engine.hpp>
#include <ob/invariants.hpp>
#include <ob/reference_engine.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// 12 bytes per command, chosen so a single flipped byte is a meaningful mutation.
constexpr std::size_t kBytesPerCommand = 12;

bool decode(const std::uint8_t* data, std::size_t size, std::size_t i,
            ob::Command& out) {
    const std::size_t off = i * kBytesPerCommand;
    if (off + kBytesPerCommand > size) {
        return false;
    }
    const std::uint8_t* p = data + off;

    std::uint32_t id_lo;
    std::int32_t  price;
    std::uint16_t qty;
    std::memcpy(&id_lo, p, 4);
    std::memcpy(&price, p + 4, 4);
    std::memcpy(&qty, p + 8, 2);
    const std::uint8_t flags = p[10];
    // p[11] is spare: it exists so the record size stays a round 12 bytes.

    out = ob::Command{};
    out.id = id_lo;
    out.price = price;
    out.qty = qty;
    out.side = (flags & 0x01) ? ob::Side::Sell : ob::Side::Buy;
    out.order_type = static_cast<ob::OrderType>((flags >> 1) % 5);
    out.type = (flags & 0x40) ? ob::CommandType::Cancel : ob::CommandType::New;
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t kCapacity = 4096;
    ob::ReferenceEngine ref(kCapacity);
    ob::FastEngine fast(ob::FastEngine::Config{kCapacity});

    std::vector<ob::Event> rs(8192), fs(8192);
    ob::EventBuffer rb(rs.data(), rs.size());
    ob::EventBuffer fb(fs.data(), fs.size());

    const std::size_t n = size / kBytesPerCommand;
    for (std::size_t i = 0; i < n; ++i) {
        ob::Command c{};
        if (!decode(data, size, i, c)) {
            break;
        }

        rb.clear();
        fb.clear();
        ref.submit(c, rb);
        fast.submit(c, fb);

        if (rb.size() != fb.size()) {
            std::fprintf(stderr,
                         "DIVERGENCE at command %zu: reference emitted %zu events, "
                         "fast emitted %zu\n",
                         i, rb.size(), fb.size());
            std::abort();
        }
        for (std::size_t k = 0; k < rb.size(); ++k) {
            if (!(rb[k] == fb[k])) {
                std::fprintf(stderr, "DIVERGENCE at command %zu event %zu\n", i, k);
                std::abort();
            }
        }
        if (rb.empty()) {
            std::fprintf(stderr,
                         "CONTRACT VIOLATION at command %zu: no event emitted\n", i);
            std::abort();
        }
        if (ref.best_bid() != fast.best_bid() || ref.best_ask() != fast.best_ask() ||
            ref.live_order_count() != fast.live_order_count()) {
            std::fprintf(stderr, "BOOK STATE DIVERGENCE at command %zu\n", i);
            std::abort();
        }

        if (const auto r = ob::check_invariants(fast); !r.ok) {
            std::fprintf(stderr, "FAST INVARIANT at command %zu: %s\n", i, r.failure);
            std::abort();
        }
        if (const auto r = fast.check_internal_invariants(); !r.ok) {
            std::fprintf(stderr, "FAST INTERNAL at command %zu: %s\n", i, r.failure);
            std::abort();
        }
        if (const auto r = ob::check_invariants(ref); !r.ok) {
            std::fprintf(stderr, "REF INVARIANT at command %zu: %s\n", i, r.failure);
            std::abort();
        }
    }
    return 0;
}
```

- [ ] **Step 2: Write `fuzz/CMakeLists.txt`**

```cmake
# libFuzzer is a clang feature. Fail loudly rather than silently building nothing.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "OB_BUILD_FUZZ=ON requires clang (libFuzzer)")
endif()

add_executable(ob_fuzz_differential fuzz_differential.cpp)
target_link_libraries(ob_fuzz_differential PRIVATE ob)
target_compile_options(ob_fuzz_differential PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -UNDEBUG)
target_link_options(ob_fuzz_differential PRIVATE
    -fsanitize=fuzzer,address,undefined)
```

`-UNDEBUG` is load-bearing: the invariant checks and the `assert`s inside `OrderPool` and `PriceLadder` are most of what the fuzzer is here to trip, and `NDEBUG` would delete them.

In the root `CMakeLists.txt`:

```cmake
if(OB_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()
```

- [ ] **Step 3: Build and run a short fuzz session**

```bash
CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_BUILD_FUZZ=ON -DOB_BUILD_TESTS=OFF
cmake --build build-fuzz
mkdir -p fuzz/corpus
./build-fuzz/fuzz/ob_fuzz_differential -max_total_time=60 -print_final_stats=1 fuzz/corpus
```

Expected: no crash, and a growing corpus. Read the reported `cov:` figure; it should rise quickly then plateau. On macOS, if clang reports that libFuzzer is unavailable with Apple clang, run this step in Docker:

```bash
docker run --rm -v "$PWD":/w -w /w alpine:3.20 sh -c \
  'apk add --no-cache cmake ninja clang lld compiler-rt >/dev/null 2>&1 &&
   CC=clang CXX=clang++ cmake -S . -B /tmp/bf -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
     -DOB_BUILD_FUZZ=ON -DOB_BUILD_TESTS=OFF &&
   cmake --build /tmp/bf &&
   /tmp/bf/fuzz/ob_fuzz_differential -max_total_time=60 -print_final_stats=1 /w/fuzz/corpus'
```

- [ ] **Step 4: Seed the corpus deliberately and commit it**

A corpus that starts from random bytes wastes minutes rediscovering that a valid order is 12 bytes. Seed it from the generator:

```bash
./build/tests/ob_tests --gtest_filter=Differential.EnginesAgreeOnGeneratedStreams >/dev/null
# Then minimise whatever the fuzzer found into a small committed corpus:
./build-fuzz/fuzz/ob_fuzz_differential -merge=1 fuzz/corpus_min fuzz/corpus
ls fuzz/corpus_min | wc -l
```

Commit `fuzz/corpus_min`, not `fuzz/corpus`: the merged set is the minimal one that preserves coverage, and it stays small enough to live in git.

- [ ] **Step 5: Run a longer session before trusting it**

```bash
./build-fuzz/fuzz/ob_fuzz_differential -max_total_time=900 fuzz/corpus_min
```

Expected: no crash in 15 minutes. **Any crash becomes a committed edge case in `tests/cases/edge_cases.hpp` before it is fixed**, so the fix has a test that fails without it. Reproduce a crash file with:

```bash
./build-fuzz/fuzz/ob_fuzz_differential crash-<hash>
```

- [ ] **Step 6: Commit**

```bash
git add fuzz/fuzz_differential.cpp fuzz/CMakeLists.txt fuzz/corpus_min CMakeLists.txt
git commit -m "test: libFuzzer differential target with an unsanitised decoder

The decoder does not clean its input: prices span the full int32 range, ids are
non-monotonic and quantities include zero. Generating inputs the engine must
REJECT is the point, because that is where the flat ladder's bounds checking
lives. Built with -UNDEBUG so the asserts the fuzzer exists to trip survive."
```

---

## Task 10: The six scenarios, with workload verification

**Files:**
- Create: `bench/scenarios.hpp`, `tests/test_scenarios.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/command.hpp`, `ob/fast_engine.hpp`, `tests/model/scenario_gen.hpp`.
- Produces: `ob::bench::Scenario` (enum: `RestOnly`, `CrossShallow`, `CrossDeep`, `CancelHeavy`, `MixedRealistic`, `WorstCaseSweep`), `name(Scenario) -> const char*`, `build(Scenario, std::uint64_t seed, std::size_t n) -> std::vector<Command>`, `struct WorkloadStats`, `measure_workload(const std::vector<Command>&) -> WorkloadStats`.

**Why the verification half matters as much as the generation half.** A scenario that silently degenerates into "everything rests, nothing ever crosses" produces beautiful numbers that mean nothing, and nothing else in the project would notice. `measure_workload` runs the stream through an engine and counts what actually happened; each scenario then has a test asserting its counts land in the band it claims. That closes a real hole rather than a theoretical one.

**Price distribution is integer-only and geometric**, not a floating-point power law. `libm` differences across platforms would make the streams non-reproducible, and a non-reproducible benchmark input is worthless. Counting trailing zero bits of a random word gives P(offset = k) = 2^-(k+1) exactly, with integer operations only, identically on every platform.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_scenarios.cpp
#include "../bench/scenarios.hpp"

#include <gtest/gtest.h>

namespace {

using ob::bench::Scenario;
using ob::bench::build;
using ob::bench::measure_workload;

constexpr Scenario kAll[] = {Scenario::RestOnly,    Scenario::CrossShallow,
                             Scenario::CrossDeep,   Scenario::CancelHeavy,
                             Scenario::MixedRealistic, Scenario::WorstCaseSweep};

TEST(Scenarios, EveryScenarioIsReproducibleFromItsSeed) {
    for (const Scenario s : kAll) {
        const auto a = build(s, 42, 5000);
        const auto b = build(s, 42, 5000);
        ASSERT_EQ(a.size(), b.size()) << ob::bench::name(s);
        for (std::size_t i = 0; i < a.size(); ++i) {
            ASSERT_EQ(a[i].id, b[i].id) << ob::bench::name(s) << " at " << i;
            ASSERT_EQ(a[i].price, b[i].price) << ob::bench::name(s) << " at " << i;
            ASSERT_EQ(a[i].qty, b[i].qty) << ob::bench::name(s) << " at " << i;
            ASSERT_EQ(a[i].type, b[i].type) << ob::bench::name(s) << " at " << i;
        }
    }
}

TEST(Scenarios, NoScenarioProducesRejections) {
    // A scenario full of rejected commands measures the validation path, not the
    // matching path, and would flatter the engine enormously.
    for (const Scenario s : kAll) {
        const auto stats = measure_workload(build(s, 7, 20000));
        const double reject_rate =
            static_cast<double>(stats.rejects) / static_cast<double>(stats.commands);
        EXPECT_LT(reject_rate, 0.01) << ob::bench::name(s) << " reject rate "
                                     << reject_rate;
    }
}

// Each scenario must actually generate the workload its name claims.
TEST(Scenarios, RestOnlyNeverCrosses) {
    const auto stats = measure_workload(build(Scenario::RestOnly, 1, 20000));
    EXPECT_EQ(stats.trades, 0u) << "rest_only traded, so it is not rest-only";
    EXPECT_GT(stats.rests, 19000u);
}

TEST(Scenarios, CrossShallowTradesConstantlyAtShallowDepth) {
    const auto stats = measure_workload(build(Scenario::CrossShallow, 1, 20000));
    EXPECT_GT(stats.trades, 5000u);
    EXPECT_LT(stats.max_sweep_levels, 3u) << "cross_shallow swept deeply";
}

TEST(Scenarios, CrossDeepSweepsManyLevels) {
    const auto stats = measure_workload(build(Scenario::CrossDeep, 1, 20000));
    EXPECT_GT(stats.trades, 5000u);
    EXPECT_GT(stats.max_sweep_levels, 8u) << "cross_deep did not sweep deeply";
}

TEST(Scenarios, CancelHeavyIsMostlyCancels) {
    const auto stats = measure_workload(build(Scenario::CancelHeavy, 1, 20000));
    const double cancel_rate =
        static_cast<double>(stats.cancels) / static_cast<double>(stats.commands);
    EXPECT_GT(cancel_rate, 0.80) << "cancel rate " << cancel_rate;
    EXPECT_LT(stats.unknown_cancels, stats.cancels / 20)
        << "most cancels missed a live order, so this measures rejection not cancel";
}

TEST(Scenarios, MixedRealisticUsesEveryOrderTypeAndTrades) {
    const auto stats = measure_workload(build(Scenario::MixedRealistic, 1, 40000));
    for (std::size_t t = 0; t < 5; ++t) {
        EXPECT_GT(stats.by_type[t], 0u) << "order type " << t << " never generated";
    }
    EXPECT_GT(stats.trades, 1000u);
    const double cancel_rate =
        static_cast<double>(stats.cancels) / static_cast<double>(stats.commands);
    EXPECT_GT(cancel_rate, 0.20);
    EXPECT_LT(cancel_rate, 0.50);
}

TEST(Scenarios, WorstCaseSweepContainsAnEnormousSweep) {
    const auto stats = measure_workload(build(Scenario::WorstCaseSweep, 1, 20000));
    EXPECT_GT(stats.max_sweep_levels, 100u)
        << "worst_case_sweep never actually swept the book";
    EXPECT_GT(stats.max_events_per_command, 200u)
        << "the harness's event buffer sizing depends on this being large";
}

// The geometric price offset must be integer-only, so the same stream is produced
// on every platform. This asserts the distribution shape rather than the code path.
TEST(Scenarios, PriceOffsetsAreGeometricallyDistributed) {
    const auto cmds = build(Scenario::MixedRealistic, 3, 200000);
    std::size_t at_touch = 0, one_away = 0, far = 0;
    for (const ob::Command& c : cmds) {
        if (c.type != ob::CommandType::New || c.price == ob::kNoPrice) {
            continue;
        }
        const int d = std::abs(c.price - 10000);
        if (d <= 1) {
            ++at_touch;
        } else if (d <= 3) {
            ++one_away;
        } else {
            ++far;
        }
    }
    // Geometric decay: near the touch must dominate, but the tail must exist.
    EXPECT_GT(at_touch, one_away);
    EXPECT_GT(far, 0u) << "no depth at all away from the touch";
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'../bench/scenarios.hpp' file not found`.

- [ ] **Step 3: Write `bench/scenarios.hpp`**

```cpp
// bench/scenarios.hpp
#pragma once

// The six benchmark workloads, plus the counters that prove each one generated
// what its name claims.
//
// WHY THE COUNTERS EXIST: a scenario that silently degenerates into "everything
// rests, nothing crosses" produces excellent and completely meaningless numbers,
// and nothing else in the project would notice. measure_workload() runs the stream
// through an engine and reports what actually happened, and each scenario has a
// test asserting its counts land in the band it advertises.
//
// PRICES ARE GENERATED WITH INTEGER OPERATIONS ONLY. A floating-point power law
// would produce different streams on different libm implementations, and a
// benchmark input that is not reproducible is not a benchmark input. Counting
// trailing zero bits gives P(offset = k) = 2^-(k+1) exactly, everywhere.

#include <ob/command.hpp>
#include <ob/fast_engine.hpp>

#include <cstdint>
#include <vector>

#include "../tests/model/scenario_gen.hpp"

namespace ob::bench {

enum class Scenario : std::uint8_t {
    RestOnly = 0,
    CrossShallow,
    CrossDeep,
    CancelHeavy,
    MixedRealistic,
    WorstCaseSweep,
};

inline const char* name(Scenario s) {
    switch (s) {
        case Scenario::RestOnly:       return "rest_only";
        case Scenario::CrossShallow:   return "cross_shallow";
        case Scenario::CrossDeep:      return "cross_deep";
        case Scenario::CancelHeavy:    return "cancel_heavy";
        case Scenario::MixedRealistic: return "mixed_realistic";
        case Scenario::WorstCaseSweep: return "worst_case_sweep";
    }
    return "?";
}

inline constexpr Ticks kMid = 10000;

// Geometric offset from the touch: P(k) = 2^-(k+1), integer-only, capped.
inline Ticks geometric_offset(obtest::Xoshiro256ss& rng, Ticks cap) {
    // The OR guards __builtin_ctzll(0), which is undefined.
    const std::uint64_t r = rng.next() | (std::uint64_t{1} << 48);
    const Ticks k = static_cast<Ticks>(__builtin_ctzll(r));
    return k > cap ? cap : k;
}

inline std::vector<Command> build(Scenario s, std::uint64_t seed, std::size_t n) {
    obtest::Xoshiro256ss rng(seed ^ (static_cast<std::uint64_t>(s) << 56));
    std::vector<Command> out;
    out.reserve(n);
    OrderId id = 1;

    switch (s) {
        // Every order rests: buys strictly below the mid, sells strictly above.
        // Isolates the insert path, the ladder write and the bitmap set.
        case Scenario::RestOnly: {
            while (out.size() < n) {
                const bool buy = (rng.bounded(2) == 0);
                const Ticks off = geometric_offset(rng, 200) + 1;
                const Ticks px = buy ? (kMid - off) : (kMid + off);
                out.push_back(make_new(id++, buy ? Side::Buy : Side::Sell,
                                       OrderType::Limit, px,
                                       1 + static_cast<Qty>(rng.bounded(100))));
            }
            break;
        }

        // Alternates: rest one order, then cross it with exactly enough quantity.
        // This is the common real case, and it keeps the book one level deep.
        case Scenario::CrossShallow: {
            while (out.size() + 1 < n) {
                const bool sell_first = (rng.bounded(2) == 0);
                const Qty qty = 1 + static_cast<Qty>(rng.bounded(100));
                const OrderId maker = id++;
                const OrderId taker = id++;
                out.push_back(make_new(maker, sell_first ? Side::Sell : Side::Buy,
                                       OrderType::Limit, kMid, qty));
                out.push_back(make_new(taker, sell_first ? Side::Buy : Side::Sell,
                                       OrderType::Limit, kMid, qty));
            }
            break;
        }

        // Builds depth across many levels, then sweeps 10 to 50 of them.
        // Isolates level traversal and bitmap advance.
        case Scenario::CrossDeep: {
            constexpr Ticks kLevels = 50;
            while (out.size() < n) {
                // Lay down one order per level on the ask side.
                for (Ticks d = 1; d <= kLevels && out.size() < n; ++d) {
                    out.push_back(make_new(id++, Side::Sell, OrderType::Limit,
                                           kMid + d, 10));
                }
                // Then one buy that sweeps most of them.
                if (out.size() < n) {
                    const Ticks depth = 10 + static_cast<Ticks>(rng.bounded(40));
                    out.push_back(make_new(id++, Side::Buy, OrderType::Limit,
                                           kMid + depth,
                                           static_cast<Qty>(10 * depth)));
                }
            }
            break;
        }

        // ~90% cancels, the realistic ratio. Cancels always target a live order,
        // because a flood of UnknownOrderId rejections would measure validation.
        case Scenario::CancelHeavy: {
            std::vector<OrderId> live;
            live.reserve(n);
            while (out.size() < n) {
                if (!live.empty() && rng.bounded(100) < 90) {
                    const std::size_t k =
                        static_cast<std::size_t>(rng.bounded(live.size()));
                    out.push_back(make_cancel(live[k]));
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
                } else {
                    const bool buy = (rng.bounded(2) == 0);
                    const Ticks off = geometric_offset(rng, 200) + 1;
                    out.push_back(make_new(id, buy ? Side::Buy : Side::Sell,
                                           OrderType::Limit,
                                           buy ? kMid - off : kMid + off, 10));
                    live.push_back(id);
                    ++id;
                }
            }
            break;
        }

        // The headline workload: geometric depth, all five order types, roughly
        // a 1:3 cancel ratio, and enough crossing to exercise matching.
        case Scenario::MixedRealistic: {
            std::vector<OrderId> live;
            live.reserve(n);
            while (out.size() < n) {
                if (!live.empty() && rng.bounded(100) < 30) {
                    const std::size_t k =
                        static_cast<std::size_t>(rng.bounded(live.size()));
                    out.push_back(make_cancel(live[k]));
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
                    continue;
                }
                const bool buy = (rng.bounded(2) == 0);
                const std::uint64_t roll = rng.bounded(100);
                OrderType t = OrderType::Limit;
                if (roll < 3) {
                    t = OrderType::Market;
                } else if (roll < 8) {
                    t = OrderType::Ioc;
                } else if (roll < 11) {
                    t = OrderType::Fok;
                } else if (roll < 16) {
                    t = OrderType::PostOnly;
                }
                // Signed offset so roughly half the limits cross.
                const Ticks off = geometric_offset(rng, 100);
                const Ticks px = buy ? (kMid + off - 2) : (kMid - off + 2);
                const Qty qty = 1 + static_cast<Qty>(rng.bounded(100));
                out.push_back(make_new(id, buy ? Side::Buy : Side::Sell, t, px, qty));
                if (t == OrderType::Limit || t == OrderType::PostOnly) {
                    live.push_back(id);
                }
                ++id;
            }
            break;
        }

        // The true worst case: fill hundreds of levels, then one order that eats
        // the entire book. This is where the tail of the distribution comes from.
        case Scenario::WorstCaseSweep: {
            constexpr Ticks kLevels = 400;
            while (out.size() < n) {
                for (Ticks d = 1; d <= kLevels && out.size() < n; ++d) {
                    out.push_back(make_new(id++, Side::Sell, OrderType::Limit,
                                           kMid + d, 10));
                }
                if (out.size() < n) {
                    out.push_back(make_new(id++, Side::Buy, OrderType::Limit,
                                           kMid + kLevels,
                                           static_cast<Qty>(10 * kLevels)));
                }
            }
            break;
        }
    }
    out.resize(n);
    return out;
}

struct WorkloadStats {
    std::size_t commands = 0;
    std::size_t news = 0;
    std::size_t cancels = 0;
    std::size_t by_type[5]{};
    std::size_t trades = 0;
    std::size_t rejects = 0;
    std::size_t rests = 0;
    std::size_t unknown_cancels = 0;
    std::size_t max_sweep_levels = 0;
    std::size_t max_events_per_command = 0;
};

// Runs the stream through a real engine and reports what actually happened. This
// is the anti-self-deception check: it is how a scenario that quietly stopped
// generating trades gets caught before its numbers are published.
inline WorkloadStats measure_workload(const std::vector<Command>& cmds) {
    WorkloadStats st;
    FastEngine e(FastEngine::Config{1 << 20});
    std::vector<Event> storage(1 << 16);
    EventBuffer buf(storage.data(), storage.size());

    for (const Command& c : cmds) {
        ++st.commands;
        if (c.type == CommandType::Cancel) {
            ++st.cancels;
        } else {
            ++st.news;
            ++st.by_type[static_cast<std::size_t>(c.order_type)];
        }

        buf.clear();
        e.submit(c, buf);
        st.max_events_per_command = std::max(st.max_events_per_command, buf.size());

        std::size_t distinct_prices = 0;
        Ticks last_px = kNoPrice;
        bool rested = (c.type == CommandType::New);
        for (const Event& ev : buf) {
            switch (ev.type) {
                case EventType::Trade:
                    ++st.trades;
                    if (ev.price != last_px) {
                        ++distinct_prices;
                        last_px = ev.price;
                    }
                    break;
                case EventType::Rejected:
                    ++st.rejects;
                    rested = false;
                    if (ev.reject == RejectReason::UnknownOrderId) {
                        ++st.unknown_cancels;
                    }
                    break;
                case EventType::Cancelled:
                case EventType::Filled:
                    if (ev.order_id == c.id) {
                        rested = false;
                    }
                    break;
                case EventType::Accepted:
                    break;
            }
        }
        st.max_sweep_levels = std::max(st.max_sweep_levels, distinct_prices);
        if (rested) {
            ++st.rests;
        }
    }
    return st;
}

}  // namespace ob::bench
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build -R Scenarios --output-on-failure
```

Expected: PASS, 9 tests. If `CrossDeepSweepsManyLevels` or `WorstCaseSweepContainsAnEnormousSweep` fails, the scenario is not generating the shape it claims and **the scenario must be fixed, not the assertion loosened** — the assertion is the only thing standing between a degenerate workload and published numbers.

- [ ] **Step 5: Commit**

```bash
git add bench/scenarios.hpp tests/test_scenarios.cpp tests/CMakeLists.txt
git commit -m "feat(bench): six workloads with counters that verify each one's shape

Prices use an integer geometric offset (trailing-zero count) rather than a
floating-point power law, so streams are byte-identical across platforms.

Each scenario has a test asserting the workload it actually produced matches
its name: rest_only must not trade, cross_deep must sweep more than 8 levels,
cancel_heavy must be over 80 percent cancels that hit live orders. A scenario
that silently degenerated would otherwise publish excellent meaningless
numbers with nothing to catch it."
```

---

## Task 11: Latency harness

**Files:**
- Create: `bench/alloc_counter.hpp`, `bench/alloc_counter.cpp`, `bench/bench_latency.cpp`, `tests/test_alloc_counter.cpp`
- Modify: `bench/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `bench/clock.hpp`, `bench/histogram.hpp`, `bench/scenarios.hpp`, `ob/fast_engine.hpp`, `ob/reference_engine.hpp`.
- Produces: `ob::bench::alloc_count()`, `ob::bench::reset_alloc_count()`; the `ob_bench_latency` executable emitting JSON to stdout.

Three things this harness does that an off-the-shelf one does not:

1. **Open-loop issue schedule.** Commands are issued against a schedule computed in advance, and latency is measured from the *intended* issue time. When the engine falls behind, the backlog is counted instead of being absorbed by the driver politely waiting. That is the coordinated-omission fix, and it is the single biggest reason not to use a closed-loop harness.
2. **Both service time and response time**, labelled separately. Service time is `done - start`, the engine's own cost. Response time is `done - intended`, what a client would see. Reporting only one of them is how benchmarks mislead without lying.
3. **A zero-allocation assertion.** `operator new` is replaced with a counting version, linked **only** into benchmark executables, and the measured window asserts the counter did not move. "No allocation on the hot path" becomes a claim that can fail a build rather than a sentence in a README.

- [ ] **Step 1: Write the allocation counter and its test**

```cpp
// bench/alloc_counter.hpp
#pragma once

// Global operator new/delete replacement that counts allocations.
//
// Linked ONLY into benchmark executables, never into the library or the test
// binary, so it cannot perturb anything else. Its purpose is to turn "no
// allocation on the hot path" from a claim in a README into an assertion that
// fails a build.

#include <atomic>
#include <cstddef>

namespace ob::bench {

std::size_t alloc_count() noexcept;
void reset_alloc_count() noexcept;

}  // namespace ob::bench
```

```cpp
// bench/alloc_counter.cpp
#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace {
// Relaxed atomic rather than a plain size_t: runtime startup can allocate from
// threads this harness does not control, and a data race would be UB even though
// the count itself is only read single-threaded.
std::atomic<std::size_t> g_allocs{0};
}  // namespace

namespace ob::bench {
std::size_t alloc_count() noexcept {
    return g_allocs.load(std::memory_order_relaxed);
}
void reset_alloc_count() noexcept {
    g_allocs.store(0, std::memory_order_relaxed);
}
}  // namespace ob::bench

// The full replaceable set. Missing one would pair a counted new with an
// uncounted delete, which on some libraries is undefined behavior rather than
// merely a wrong count.
void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
void* operator new(std::size_t n, std::align_val_t a) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(a) < sizeof(void*)
                               ? sizeof(void*)
                               : static_cast<std::size_t>(a),
                       n == 0 ? 1 : n) != 0) {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
```

Add `#include <cstdlib>` for `posix_memalign`. Note this file must **not** be linked into `ob_tests`; the test below builds its own tiny executable instead.

```cpp
// tests/test_alloc_counter.cpp
//
// Built as its OWN executable linking bench/alloc_counter.cpp, because replacing
// global operator new inside the main test binary would perturb GoogleTest itself.
#include "../bench/alloc_counter.hpp"

#include <ob/fast_engine.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main() {
    // 1. The counter must actually count.
    ob::bench::reset_alloc_count();
    {
        std::vector<int> v;
        v.reserve(1000);
        ob::bench::alloc_count();
    }
    CHECK(ob::bench::alloc_count() > 0, "counter did not observe a vector allocation");

    // 2. FastEngine::submit must allocate nothing, ever. This is the claim.
    ob::FastEngine e(ob::FastEngine::Config{1 << 16});
    std::vector<ob::Event> storage(1 << 14);
    ob::EventBuffer buf(storage.data(), storage.size());

    // Warm up outside the measured window so construction-time allocation and
    // any lazy runtime initialisation do not count.
    for (ob::OrderId id = 1; id <= 1000; ++id) {
        buf.clear();
        e.submit(ob::make_new(id, ob::Side::Buy, ob::OrderType::Limit, 9000, 10), buf);
    }

    ob::bench::reset_alloc_count();
    for (ob::OrderId id = 1001; id <= 51000; ++id) {
        buf.clear();
        e.submit(ob::make_new(id, ob::Side::Sell, ob::OrderType::Limit, 11000, 10), buf);
        buf.clear();
        e.submit(ob::make_new(id + 1'000'000, ob::Side::Buy, ob::OrderType::Market,
                              ob::kNoPrice, 5),
                 buf);
        buf.clear();
        e.submit(ob::make_cancel(id), buf);
    }
    const std::size_t allocs = ob::bench::alloc_count();
    std::printf("allocations during 150000 submits: %zu\n", allocs);
    CHECK(allocs == 0, "FastEngine::submit allocated on the hot path");

    std::printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Wire both into the build, run, verify the test fails first**

In `bench/CMakeLists.txt`:

```cmake
add_library(ob_alloc_counter STATIC alloc_counter.cpp)
target_include_directories(ob_alloc_counter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

In `tests/CMakeLists.txt`, as a separate executable (**not** part of `ob_tests`):

```cmake
# Its own binary on purpose: replacing global operator new inside ob_tests would
# perturb GoogleTest's own allocations and make the assertion meaningless.
add_executable(ob_test_alloc_counter test_alloc_counter.cpp)
target_link_libraries(ob_test_alloc_counter PRIVATE ob ob_alloc_counter)
target_compile_options(ob_test_alloc_counter PRIVATE -UNDEBUG)
add_test(NAME alloc_counter COMMAND ob_test_alloc_counter)
```

```bash
cmake --build build && ctest --test-dir build -R alloc_counter --output-on-failure
```

Expected: PASS, reporting `allocations during 150000 submits: 0`. **A nonzero count here means the no-allocation claim is false**; find the allocation before proceeding, because every latency number after this point would be contaminated by it.

- [ ] **Step 3: Write `bench/bench_latency.cpp`**

```cpp
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

#include "alloc_counter.hpp"
#include "clock.hpp"
#include "histogram.hpp"
#include "scenarios.hpp"

#include <ob/fast_engine.hpp>
#include <ob/reference_engine.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace ob;
using namespace ob::bench;

constexpr std::size_t kWarmup = 100'000;  // discarded; the count is published

struct Options {
    std::size_t ops = 2'000'000;
    double      target_rate_hz = 0.0;  // 0 means "as fast as possible"
    std::uint64_t seed = 20260922;
    bool        reference = false;
    const char* raw_dir = nullptr;
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
    const auto shape = measure_workload(build(sc, opt.seed, 20'000));
    const std::size_t buf_cap = shape.max_events_per_command * 8 + 1024;

    EngineT engine{};
    std::vector<Event> storage(buf_cap);
    EventBuffer buf(storage.data(), storage.size());

    Histogram service(opt.ops);
    Histogram response(opt.ops);

    // Warm-up: caches, branch predictor, and the first pass over the arena.
    for (std::size_t i = 0; i < kWarmup; ++i) {
        buf.clear();
        engine.submit(stream[i], buf);
        do_not_optimize(buf.size());
    }

    const Clock& clk = Clock::instance();
    const double period_ticks =
        opt.target_rate_hz > 0.0
            ? static_cast<double>(clk.counter_hz()) / opt.target_rate_hz
            : 0.0;

    reset_alloc_count();
    const std::uint64_t t_origin = Clock::raw_serialized();
    const std::uint64_t batch_begin = t_origin;

    for (std::size_t i = 0; i < opt.ops; ++i) {
        const std::uint64_t intended =
            period_ticks > 0.0
                ? t_origin + static_cast<std::uint64_t>(static_cast<double>(i) *
                                                        period_ticks)
                : 0;
        if (period_ticks > 0.0) {
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
    const std::size_t allocs = alloc_count();

    // Batched per-operation cost: one timestamp pair over the whole run, so the
    // 41.67 ns quantization cancels completely. This is the number to trust for
    // "how much does one operation cost"; the distribution is for tail shape.
    const double batched_ns =
        clk.ticks_to_ns(batch_end - batch_begin) / static_cast<double>(opt.ops);

    const double oh = clk.overhead_ns_serialized();
    const auto ns = [&](std::uint32_t ticks) { return clk.ticks_to_ns(ticks) - oh; };

    std::printf(
        "{\"scenario\":\"%s\",\"engine\":\"%s\",\"ops\":%zu,\"warmup\":%zu,"
        "\"seed\":%llu,\"allocations\":%zu,\"saturated\":%zu,"
        "\"clock_resolution_ns\":%.4f,\"clock_overhead_ns\":%.4f,"
        "\"batched_ns_per_op\":%.2f,"
        "\"service_ns\":{\"p50\":%.1f,\"p90\":%.1f,\"p99\":%.1f,\"p99_9\":%.1f,"
        "\"p99_99\":%.1f,\"min\":%.1f,\"max\":%.1f,\"mean\":%.1f}",
        name(sc), opt.reference ? "reference" : "fast", opt.ops, kWarmup,
        static_cast<unsigned long long>(opt.seed), allocs, service.saturated(),
        clk.resolution_ns(), oh, batched_ns, ns(service.percentile(50.0)),
        ns(service.percentile(90.0)), ns(service.percentile(99.0)),
        ns(service.percentile(99.9)), ns(service.percentile(99.99)),
        ns(service.min()), ns(service.max()),
        clk.ticks_to_ns(static_cast<std::uint64_t>(service.mean())) - oh);

    if (period_ticks > 0.0) {
        std::printf(",\"offered_rate_hz\":%.0f,\"response_ns\":{\"p50\":%.1f,"
                    "\"p99\":%.1f,\"p99_9\":%.1f,\"max\":%.1f}",
                    opt.target_rate_hz, ns(response.percentile(50.0)),
                    ns(response.percentile(99.0)), ns(response.percentile(99.9)),
                    ns(response.max()));
    }
    std::printf("}\n");

    if (allocs != 0) {
        std::fprintf(stderr,
                     "FATAL: %zu allocations during the measured window for %s. "
                     "Every number above is contaminated.\n",
                     allocs, name(sc));
        std::exit(2);
    }
    if (service.saturated() != 0) {
        std::fprintf(stderr,
                     "WARNING: %zu samples exceeded uint32 ticks for %s; the tail "
                     "is clamped and this run should not be published.\n",
                     service.saturated(), name(sc));
    }

    if (opt.raw_dir != nullptr) {
        const std::string path =
            std::string(opt.raw_dir) + "/" + name(sc) + "_service.raw";
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

    constexpr Scenario kAll[] = {Scenario::RestOnly,       Scenario::CrossShallow,
                                Scenario::CrossDeep,      Scenario::CancelHeavy,
                                Scenario::MixedRealistic, Scenario::WorstCaseSweep};
    for (const Scenario sc : kAll) {
        if (opt.reference) {
            run_scenario<ReferenceEngine>(sc, opt);
        } else {
            run_scenario<FastEngine>(sc, opt);
        }
    }
    return 0;
}
```

`ReferenceEngine` is default-constructible with a 1 M capacity, so `EngineT engine{}` works for both; `FastEngine{}` uses its default `Config`.

- [ ] **Step 4: Add the executable and run it**

In `bench/CMakeLists.txt`:

```cmake
add_executable(ob_bench_latency bench_latency.cpp)
target_link_libraries(ob_bench_latency PRIVATE ob ob_bench ob_alloc_counter)
```

```bash
cmake --build build
./build/bench/ob_bench_latency --ops 500000 2>&1 | tail -10
```

Expected: six JSON lines with `"allocations":0`. Sanity-check the output before believing any of it:

- `clock_resolution_ns` should read ~41.67. If it reads ~1, the conversion is wrong.
- `batched_ns_per_op` and `service_ns.p50` should be within about 2x of each other. A large gap means the per-operation timestamps are dominated by clock overhead, which is expected if p50 is near the 41.67 ns floor — and is exactly why the batched figure is reported.
- `worst_case_sweep` must show a far larger `max` than the others. If it does not, the scenario is not doing what it claims.

- [ ] **Step 5: Verify the open-loop driver actually observes queueing**

```bash
# A rate the engine can sustain: response ~= service.
./build/bench/ob_bench_latency --ops 200000 --rate 2000000 2>/dev/null | head -1
# A rate it cannot: response p99 must blow up while service stays flat.
./build/bench/ob_bench_latency --ops 200000 --rate 50000000 2>/dev/null | head -1
```

Expected: at the unsustainable rate, `response_ns.p99` is orders of magnitude larger than `service_ns.p99` while `service_ns` barely moves. **If response and service stay similar at an impossible offered rate, the open-loop driver is not working** and the harness has coordinated omission after all.

- [ ] **Step 6: Commit**

```bash
git add bench/alloc_counter.hpp bench/alloc_counter.cpp bench/bench_latency.cpp bench/CMakeLists.txt tests/test_alloc_counter.cpp tests/CMakeLists.txt
git commit -m "feat(bench): open-loop latency harness with a zero-allocation assertion

Reports service time (engine cost) and response time (what a client sees,
including backlog) separately, from an issue schedule computed in advance, so
queueing delay is counted rather than absorbed by the driver waiting.

Also reports a batched per-operation cost from a single timestamp pair, which
is quantization-free and is the figure to trust for per-op cost given the
41.67 ns clock floor.

operator new is replaced with a counting version linked only into benchmark
binaries, and a nonzero count in the measured window exits non-zero. The
no-allocation claim can now fail a build."
```

---

## Task 12: Throughput and the median-of-5 runner

**Files:**
- Create: `bench/bench_throughput.cpp`, `scripts/run_bench.sh`
- Modify: `bench/CMakeLists.txt`

**Interfaces:**
- Consumes: `bench/clock.hpp`, `bench/scenarios.hpp`, `bench/alloc_counter.hpp`.
- Produces: `ob_bench_throughput` executable; `scripts/run_bench.sh` emitting aggregated JSON.

Throughput is a different question from latency with a different answer, and they trade off, so it gets its own binary rather than a mode flag. The runner does the part that makes results trustworthy: **five independent process runs, median reported, spread published, and a spread over 10% invalidates the run** rather than being silently averaged away.

- [ ] **Step 1: Write `bench/bench_throughput.cpp`**

```cpp
// bench/bench_throughput.cpp
//
// Saturation throughput: how many commands per second the engine sustains when
// nothing throttles it. Deliberately separate from the latency harness, because
// the two trade off against each other and reporting one number for both hides it.

#include "alloc_counter.hpp"
#include "clock.hpp"
#include "scenarios.hpp"

#include <ob/fast_engine.hpp>
#include <ob/reference_engine.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using namespace ob;
using namespace ob::bench;

constexpr std::size_t kWarmup = 100'000;

template <class EngineT>
void run(Scenario sc, std::size_t ops, std::uint64_t seed, bool reference) {
    const auto stream = build(sc, seed, ops + kWarmup);
    const auto shape = measure_workload(build(sc, seed, 20'000));
    const std::size_t buf_cap = shape.max_events_per_command * 8 + 1024;

    EngineT engine{};
    std::vector<Event> storage(buf_cap);
    EventBuffer buf(storage.data(), storage.size());

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
    const std::uint64_t t1 = Clock::raw_serialized();
    const std::size_t allocs = alloc_count();

    const double secs = Clock::instance().ticks_to_ns(t1 - t0) / 1e9;
    std::printf("{\"scenario\":\"%s\",\"engine\":\"%s\",\"ops\":%zu,"
                "\"seconds\":%.6f,\"ops_per_sec\":%.0f,\"events_per_sec\":%.0f,"
                "\"ns_per_op\":%.2f,\"allocations\":%zu}\n",
                name(sc), reference ? "reference" : "fast", ops, secs,
                static_cast<double>(ops) / secs,
                static_cast<double>(events) / secs,
                secs * 1e9 / static_cast<double>(ops), allocs);

    if (allocs != 0) {
        std::fprintf(stderr, "FATAL: %zu allocations during %s\n", allocs, name(sc));
        std::exit(2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t ops = 5'000'000;
    std::uint64_t seed = 20260922;
    bool reference = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--reference") == 0) {
            reference = true;
        } else {
            std::fprintf(stderr, "usage: %s [--ops N] [--seed S] [--reference]\n",
                         argv[0]);
            return 1;
        }
    }
    Clock::instance().print_report(stderr);

    constexpr Scenario kAll[] = {Scenario::RestOnly,       Scenario::CrossShallow,
                                Scenario::CrossDeep,      Scenario::CancelHeavy,
                                Scenario::MixedRealistic, Scenario::WorstCaseSweep};
    for (const Scenario sc : kAll) {
        if (reference) {
            run<ReferenceEngine>(sc, ops, seed, true);
        } else {
            run<FastEngine>(sc, ops, seed, false);
        }
    }
    return 0;
}
```

In `bench/CMakeLists.txt`:

```cmake
add_executable(ob_bench_throughput bench_throughput.cpp)
target_link_libraries(ob_bench_throughput PRIVATE ob ob_bench ob_alloc_counter)
```

- [ ] **Step 2: Write `scripts/run_bench.sh`**

```bash
#!/usr/bin/env bash
# Median-of-5 independent process runs, with the spread published.
#
# Five separate PROCESSES, not five loops inside one: a single process shares a
# warmed cache, a settled frequency state and one allocator arena across all five,
# which is exactly the variance this is supposed to expose.
#
# A spread over 10% INVALIDATES the run. Reporting the median of five wildly
# different numbers as though it were a measurement is how benchmark results
# become fiction.
set -euo pipefail

BIN="${1:?usage: run_bench.sh <binary> [extra args...]}"
shift || true
RUNS="${OB_BENCH_RUNS:-5}"
OUT_DIR="${OB_BENCH_OUT:-bench/results}"
mkdir -p "$OUT_DIR"

echo "running $BIN, $RUNS independent processes" >&2
for i in $(seq 1 "$RUNS"); do
    "$BIN" "$@" > "$OUT_DIR/run_$i.jsonl" 2> "$OUT_DIR/run_$i.clock"
    echo "  run $i done" >&2
done

python3 - "$OUT_DIR" "$RUNS" <<'PY'
import json, statistics, sys, pathlib
out_dir, runs = pathlib.Path(sys.argv[1]), int(sys.argv[2])

by_key = {}
for i in range(1, runs + 1):
    for line in (out_dir / f"run_{i}.jsonl").read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        by_key.setdefault((rec["scenario"], rec["engine"]), []).append(rec)

failed = False
summary = []
for (scenario, engine), recs in sorted(by_key.items()):
    # Pick the headline metric this binary reports.
    if "ops_per_sec" in recs[0]:
        metric, vals = "ops_per_sec", [r["ops_per_sec"] for r in recs]
    else:
        metric, vals = "service_p99_ns", [r["service_ns"]["p99"] for r in recs]

    med = statistics.median(vals)
    spread = (max(vals) - min(vals)) / med if med else 0.0
    ok = spread <= 0.10
    failed = failed or not ok
    summary.append({
        "scenario": scenario, "engine": engine, "metric": metric,
        "median": med, "min": min(vals), "max": max(vals),
        "spread": round(spread, 4), "runs": len(vals), "valid": ok,
    })
    flag = "" if ok else "  <-- SPREAD OVER 10 PERCENT, RUN INVALID"
    print(f"{scenario:<18} {engine:<10} {metric:<16} median={med:>14,.1f} "
          f"spread={spread:6.2%}{flag}", file=sys.stderr)

(out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
if failed:
    print("\nAt least one measurement had over 10% spread across runs. Close other "
          "applications, let the machine cool, and re-run. Do not publish this.",
          file=sys.stderr)
    sys.exit(1)
PY
```

```bash
chmod +x scripts/run_bench.sh
```

- [ ] **Step 3: Run both harnesses through the runner**

```bash
./scripts/run_bench.sh ./build/bench/ob_bench_throughput --ops 3000000
./scripts/run_bench.sh ./build/bench/ob_bench_latency --ops 1000000
```

Expected: a per-scenario table with spreads under 10%. If it exits non-zero, that is the script working: close other applications and re-run rather than publishing the median of a noisy set.

- [ ] **Step 4: Record the reference baseline, so every later speedup is a ratio**

```bash
./build/bench/ob_bench_throughput --ops 200000 --reference 2>/dev/null \
  | tee bench/results/reference_throughput.jsonl
```

Note the smaller `--ops`: `ReferenceEngine` is slow by design, and its `worst_case_sweep` in particular is O(orders) on the FOK pre-scan. **This is the number every "N times faster" claim in the README is measured against**, so it is recorded before any optimization work begins.

- [ ] **Step 5: Commit**

```bash
git add bench/bench_throughput.cpp bench/CMakeLists.txt scripts/run_bench.sh bench/results/reference_throughput.jsonl
git commit -m "feat(bench): saturation throughput and a median-of-5 runner that can fail

Five independent processes rather than five loops in one, because a single
process shares a warmed cache and one allocator arena across all five, which is
the variance this is meant to expose. A spread over 10 percent exits non-zero
instead of quietly reporting the median of a noisy set.

Also records the ReferenceEngine baseline now, before any optimization, so
every later speedup is a ratio against a real measurement."
```

---

## Task 13: Profiling

**Files:**
- Create: `scripts/profile.sh`, `scripts/cachegrind.sh`, `docs/OPTIMIZATION-LOG.md`

**Interfaces:**
- Consumes: the benchmark binaries.
- Produces: profiling scripts and the log that every Task 14 entry is written into.

Both scripts run in Docker, which is the **only** place `perf` exists for this project. Verified available: perf 6.6.31, `perf record -F 999 -e cpu-clock -g` produces correct call-graph profiles, Valgrind 3.23.0 Cachegrind works on aarch64. Verified **not** available: any hardware PMU, so `cycles`, `instructions` and `cache-misses` are `<not supported>` and real cache-miss counts do not exist anywhere in this project. Cachegrind *simulates* them, which is a different thing and is labelled as such.

- [ ] **Step 1: Write `scripts/profile.sh`**

```bash
#!/usr/bin/env bash
# Sampled profile via perf, inside Docker.
#
# perf does not exist on macOS and Apple Silicon exposes no userspace PMU, so this
# is the only way to profile this project locally. It uses the SOFTWARE cpu-clock
# event, because the Docker VM has no PMU either: /sys/bus/event_source/devices
# lists only breakpoint, kprobe, software, tracepoint and uprobe.
#
# Consequence: this gives you WHERE the time goes. It cannot give you cache misses
# or branch mispredictions. Use scripts/cachegrind.sh for simulated versions.
set -euo pipefail

TARGET="${1:-ob_bench_throughput}"
shift || true
ARGS=("${@:---ops}" "${@:+1000000}")
OUT="${OB_PROFILE_OUT:-bench/results/profile}"
mkdir -p "$OUT"

docker run --rm --privileged -v "$PWD":/w -w /w alpine:3.20 sh -euc '
  apk add --no-cache cmake ninja g++ perf >/dev/null 2>&1

  cmake -S . -B /tmp/bp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DOB_BUILD_BENCH=ON -DOB_BUILD_TESTS=OFF \
        -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -g" >/dev/null
  cmake --build /tmp/bp >/dev/null

  echo "=== perf record (software cpu-clock; no PMU in this VM) ===" >&2
  perf record -q -F 999 -e cpu-clock -g --call-graph fp \
    -o /tmp/perf.data "/tmp/bp/bench/'"$TARGET"'" '"${ARGS[*]}"' >/dev/null 2>&1

  echo "=== top 40 symbols, self time ===" 
  perf report -i /tmp/perf.data --stdio --no-children --percent-limit 0.2 2>/dev/null \
    | head -60

  # Folded stacks: dependency-free, and enough to render a flamegraph later with
  # any folded-format tool. Deliberately not downloading one here.
  perf script -i /tmp/perf.data 2>/dev/null \
    | awk '"'"'
        /^$/ { if (stack != "") { print stack" "1 } stack=""; next }
        /^[0-9a-f]+ / { next }
        { sym=$2; if (sym=="") next
          if (stack=="") stack=sym; else stack=sym";"stack }
        END { if (stack != "") print stack" "1 }
      '"'"' | sort | uniq -c \
      | awk '"'"'{ c=$1; $1=""; sub(/ 1$/,""); print substr($0,2)" "c }'"'"' \
      > /w/'"$OUT"'/folded.txt

  echo "folded stacks written to '"$OUT"'/folded.txt" >&2
'
echo "Profile artifacts in $OUT" >&2
```

```bash
chmod +x scripts/profile.sh
```

- [ ] **Step 2: Write `scripts/cachegrind.sh`**

```bash
#!/usr/bin/env bash
# Deterministic instruction and simulated-cache counts via Cachegrind, in Docker.
#
# MEASURED determinism: two runs of one identical binary gave 1,842,733 and
# 1,842,734 instruction references. That is ~1 part in 2,000,000, which is why the
# CI regression gate uses this and not wall-clock time.
#
# --cache-sim=yes is required for D refs and miss rates; Cachegrind reports only
# I refs without it. The miss rates are SIMULATED against a modelled cache, not
# measured on silicon. There is no PMU anywhere in this project to measure them.
set -euo pipefail

TARGET="${1:-ob_cachegrind_probe}"
shift || true
OUT="${OB_CG_OUT:-bench/results/cachegrind}"
mkdir -p "$OUT"

docker run --rm -v "$PWD":/w -w /w alpine:3.20 sh -euc '
  apk add --no-cache cmake ninja g++ valgrind >/dev/null 2>&1
  cmake -S . -B /tmp/bc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DOB_BUILD_BENCH=ON -DOB_BUILD_TESTS=OFF >/dev/null
  cmake --build /tmp/bc >/dev/null

  valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes \
    --cachegrind-out-file=/w/'"$OUT"'/cachegrind.out \
    "/tmp/bc/bench/'"$TARGET"'" '"$*"' 2>&1 | tee /w/'"$OUT"'/summary.txt
'
echo "Cachegrind output in $OUT" >&2
```

```bash
chmod +x scripts/cachegrind.sh
```

- [ ] **Step 3: Get a baseline profile and read it**

```bash
./scripts/profile.sh ob_bench_throughput --ops 2000000 2>&1 | tail -45
```

Expected: a symbol table dominated by `FastEngine::submit` and the structures it inlines. Write down the top five symbols with their percentages; they are the hypothesis input for Task 14. **Do not optimize anything yet.** The point of this step is that every change in Task 14 cites this profile.

- [ ] **Step 4: Write `docs/OPTIMIZATION-LOG.md` with the baseline entry**

```markdown
# Optimization log

One entry per attempt, **including the attempts that did not work.** A log that
only contains successes is a marketing document, and the failures are usually the
more interesting half of the conversation.

Every entry has the same six fields. An entry missing profile evidence is a guess,
and guesses get reverted regardless of what the numbers did.

## Rules

1. **No change lands without a profile pointing at it first.** "This looks slow" is
   not evidence.
2. **No number is believed before the differential test passes.** A fast engine
   that disagrees with the reference is worthless, and it is easy to build by
   accident. Run `OB_DIFF_OPS=10000000` before recording any figure.
3. **Median of 5 independent runs, spread published.** A spread over 10% invalidates
   the measurement; `scripts/run_bench.sh` enforces this.
4. **One optimization per commit**, so any single one can be reverted alone.
5. **A change that improves p50 and worsens p99.9 is recorded as exactly that**,
   and the keep-or-revert decision is written down with its reasoning. Tail
   latency is the product in this domain.

## Entry template

```
### N. <what was changed>

**Hypothesis:** what I expected and why.
**Profile evidence:** the symbol and percentage from scripts/profile.sh that
  motivated this, with the command that produced it.
**Change:** what the code now does differently, and the commit.
**Result:** before/after per scenario. p50, p99, p99.9, throughput, Cachegrind
  I refs. Median of 5, spread stated.
**Regressions:** anything that got worse, including in other scenarios.
**Decision:** kept or reverted, and why.
```

## 0. Baseline

**Hypothesis:** none. This is the starting point everything else is measured
against.

**Profile evidence:** `./scripts/profile.sh ob_bench_throughput --ops 2000000`,
top symbols recorded in `bench/results/profile/`.

**Change:** none. `FastEngine` as first written in Phase 2 Task 7: flat ladder,
three-level bitmap, arena with an index free list, open-addressed ID index with
SplitMix64 hashing and backward-shift deletion.

**Result:** recorded in `bench/results/summary.json` and
`docs/BENCHMARKS.md`. `ReferenceEngine` measured under the identical harness in
`bench/results/reference_throughput.jsonl`, so every later ratio has a real
denominator.

**Regressions:** n/a.

**Decision:** baseline.
```

- [ ] **Step 5: Commit**

```bash
git add scripts/profile.sh scripts/cachegrind.sh docs/OPTIMIZATION-LOG.md bench/results/profile
git commit -m "build: Docker-based perf and Cachegrind profiling, plus the optimization log

Docker is the only place perf exists for this project: it does not exist on
macOS and Apple Silicon has no userspace PMU. The VM has no PMU either, so
this uses software cpu-clock sampling. Cache-miss figures come from Cachegrind
SIMULATION and are labelled as such, because no real counters exist anywhere
in this setup.

The log's rules are the point: no change without profile evidence first, no
number believed before the differential test passes, and failures recorded
rather than deleted."
```

---

## Task 14: The optimization arc

**Files:**
- Modify: `include/ob/id_index.hpp`, `include/ob/price_ladder.hpp`, `include/ob/fast_engine.hpp`, `docs/OPTIMIZATION-LOG.md`, `bench/results/`

**Interfaces:**
- Consumes: everything.
- Produces: no new public API. Behavior must not change at all, which the differential test enforces.

Five candidates, each motivated by something already established rather than by intuition. **The procedure is identical for every one, and it is the deliverable as much as the speedups are.** Expect at least one candidate to lose; record it and move on.

**Per-candidate procedure, applied to each of the five below:**

1. Re-run `./scripts/profile.sh` and confirm the profile still points at this candidate. If it does not, skip it and say so in the log.
2. Make the change on its own branch or as a single commit.
3. `OB_DIFF_OPS=10000000 ./build/tests/ob_tests --gtest_filter=Differential.*` — **must pass before any number is recorded.**
4. Full test suite, plus the ASan/UBSan build, plus 5 minutes of fuzzing.
5. `./scripts/run_bench.sh ./build/bench/ob_bench_latency --ops 1000000` and the same for throughput. Median of 5; a spread over 10% means re-run.
6. `./scripts/cachegrind.sh` for the deterministic instruction count.
7. Write the log entry with all six fields, including regressions.
8. Keep or revert. **Reverting is a valid, recordable outcome and is not a failure.**

### Candidate 1: identity hashing in `IdIndex`

**Motivation, already established in spec 5.7:** the index is 33.5 MB at default capacity and SplitMix64 scatters sequential IDs uniformly across all of it, making it by far the largest touched footprint in the engine. Real order IDs arrive sequentially from a sequencer, so identity hashing maps them to contiguous buckets and should collapse that footprint to a few cache lines.

```cpp
    // Candidate 1: identity hashing. Sequential ids land in consecutive buckets,
    // so the touched footprint collapses from ~33 MB scattered to a few contiguous
    // cache lines. The risk is adversarial or sparse ids clustering badly, which
    // is why step 3 of the procedure (differential + fuzz) is not optional here.
    [[nodiscard]] static std::uint64_t hash(OrderId id) noexcept { return id; }
```

**Must also be measured against scattered IDs**, not only sequential ones, because the fuzzer supplies those and a structure that degrades badly under them is a liability. Add a second measurement with `OB_DIFF_SEED_BASE` varied and note both in the log. If sequential wins big and scattered loses badly, the honest resolution is to keep SplitMix64 and record that the tradeoff was measured rather than assumed — that entry is more interesting than a speedup.

### Candidate 2: cached best-price cursor

**Motivation:** `PriceLadder::best()` currently queries the bitmap on every call, and `match_into` calls it once per level plus once per loop iteration. Three word loads is cheap but not free, and the answer changes rarely.

```cpp
    // Candidate 2: cache the best price, invalidated only when a level empties or
    // a better price appears. The bitmap remains the source of truth, and the
    // debug build asserts the cache agrees with it on every access, which is how a
    // stale-cache bug gets caught in the tests rather than in the numbers.
    [[nodiscard]] Ticks best() const noexcept {
#ifndef NDEBUG
        assert(cached_best_ == compute_best() && "best-price cache is stale");
#endif
        return cached_best_;
    }
```

Update `cached_best_` in `push_back` (when the new price is better) and in `unlink` (when the level emptied and it was the best). **The `assert` comparing cache to bitmap is the load-bearing part**: without it this candidate is a stale-cache bug waiting to happen, and with it the test suite catches one immediately.

### Candidate 3: prefetch the next order during a sweep

**Motivation:** `match_into` walks an intrusive list by index, so the next order's address is not known until the current one is loaded. In `cross_deep` and `worst_case_sweep` that is a dependent-load chain, which is the classic prefetch target.

```cpp
                Order& maker = pool_.at(s);
                // Candidate 3: the next node's address depends on loading this one,
                // so the hardware prefetcher cannot help. Issue it explicitly.
                if (maker.next != kInvalidSlot) {
                    __builtin_prefetch(&pool_.at(maker.next), 0, 3);
                }
```

Expect this to help `cross_deep` and `worst_case_sweep` and do nothing for `cross_shallow`, where the list is one node long. **A prefetch that helps one scenario and measurably hurts another is a real result**; record both.

### Candidate 4: branch hints on the dominant path

**Motivation:** in `mixed_realistic` most commands rest without crossing, and in `cancel_heavy` most are cancels. The validation chain currently treats every rejection branch as equally likely.

```cpp
        if (const RejectReason r = validate_new(c); r != RejectReason::None) [[unlikely]] {
            emit_rejected(out, c.id, r);
            return;
        }
```

Measure with Cachegrind's `--branch-sim=yes` as well as wall-clock: branch hints are exactly the kind of change where the instruction count barely moves and the mispredict count does.

### Candidate 5: fuse index erase with pool free

**Motivation:** the full-fill path in `match_into` does `book.unlink`, then `index_.erase`, then `pool_.free`, touching the order slot three times. The profile will show whether that is material.

Only attempt this if step 1 says the profile points here. **If it does not, the log entry reads "skipped, profile did not support it", and that is a correct outcome** — it is the rule from the log working as intended.

- [ ] **Step 1: Work through candidates 1 to 5 using the procedure above**

One commit per candidate. Each commit message states the measured before/after and the decision. A reverted candidate still gets a log entry and a commit touching only `docs/OPTIMIZATION-LOG.md`.

- [ ] **Step 2: Confirm behavior did not change, at full strength**

```bash
OB_DIFF_OPS=10000000 ./build/tests/ob_tests --gtest_filter=Differential.*
ctest --test-dir build --output-on-failure
./build-fuzz/fuzz/ob_fuzz_differential -max_total_time=600 fuzz/corpus_min
```

Expected: all green. **The entire arc is only valid if behavior is unchanged**; a speedup that altered output is not a speedup.

- [ ] **Step 3: Confirm the log is complete and honest**

```bash
grep -c '^### ' docs/OPTIMIZATION-LOG.md      # one heading per candidate plus baseline
grep -c 'Decision:' docs/OPTIMIZATION-LOG.md   # must equal the heading count
grep -n 'reverted\|skipped' docs/OPTIMIZATION-LOG.md
```

Expected: every entry has all six fields. **If the third command prints nothing, be suspicious of your own log**: five out of five candidates succeeding is unusual, and an arc with no negative results usually means the negative results were quietly dropped. The failures are the most credible part of the document.

- [ ] **Step 4: Commit the final log**

```bash
git add docs/OPTIMIZATION-LOG.md bench/results
git commit -m "perf: documented optimization arc with measured before and after

One entry per candidate including the ones that lost. Each cites the profile
that motivated it, passes 10^7 differential operations before its number is
recorded, and reports median-of-5 with the spread."
```

---

## Task 15: Cachegrind regression gate, published results, README

**Files:**
- Create: `bench/cachegrind_probe.cpp`, `bench/baselines/instructions.json`, `scripts/check_regression.py`, `scripts/gen_benchmarks_md.py`, `docs/BENCHMARKS.md`
- Modify: `bench/CMakeLists.txt`, `.github/workflows/ci.yml`, `README.md`

**Interfaces:**
- Consumes: everything.
- Produces: `ob_cachegrind_probe` executable; the CI performance gate.

The gate counts instructions rather than measuring time, for a reason now backed by measurement rather than belief: **two Cachegrind runs of one identical binary differed by 1 instruction in 1,842,733, about 1 part in 2 million.** A 2% threshold therefore has roughly 40,000x margin, so it cannot flake, while wall-clock on a shared runner varies by tens of percent. A flaky gate gets disabled and then nobody notices the real regression, which is strictly worse than no gate.

- [ ] **Step 1: Write `bench/cachegrind_probe.cpp`**

```cpp
// bench/cachegrind_probe.cpp
//
// Fixed work per scenario, for deterministic instruction counting. Deliberately
// small: Cachegrind is roughly 50x slower than native, and determinism is what
// matters here, not scale.
//
// No timing and no clock calls at all. A clock read would make the instruction
// count depend on how many times the loop ran, which is the one thing that must
// not vary.

#include "scenarios.hpp"

#include <ob/fast_engine.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    using namespace ob;
    using namespace ob::bench;

    std::size_t ops = 50'000;
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            only = argv[++i];
        }
    }

    constexpr Scenario kAll[] = {Scenario::RestOnly,       Scenario::CrossShallow,
                                Scenario::CrossDeep,      Scenario::CancelHeavy,
                                Scenario::MixedRealistic, Scenario::WorstCaseSweep};

    for (const Scenario sc : kAll) {
        if (only != nullptr && std::strcmp(only, name(sc)) != 0) {
            continue;
        }
        const auto stream = build(sc, 20260922, ops);
        const auto shape = measure_workload(build(sc, 20260922, 5'000));

        FastEngine engine(FastEngine::Config{1 << 20});
        std::vector<Event> storage(shape.max_events_per_command * 8 + 1024);
        EventBuffer buf(storage.data(), storage.size());

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
```

In `bench/CMakeLists.txt`:

```cmake
# No alloc_counter here: counting allocations would itself change the instruction
# count, and this binary exists to make that count stable.
add_executable(ob_cachegrind_probe cachegrind_probe.cpp)
target_link_libraries(ob_cachegrind_probe PRIVATE ob ob_bench)
```

- [ ] **Step 2: Write `scripts/check_regression.py`**

```python
#!/usr/bin/env python3
"""Compare Cachegrind instruction counts against a committed baseline.

Gates on INSTRUCTION COUNT, not wall-clock time. Measured justification: two
Cachegrind runs of one identical binary gave 1,842,733 and 1,842,734 I refs, or
about 1 part in 2,000,000. The 2% threshold therefore has ~40,000x margin and
cannot flake. Wall-clock on a shared CI runner varies by tens of percent, so a
wall-clock gate either flakes constantly or is set so loose it catches nothing --
and a flaky gate gets disabled, after which nobody notices the real regression.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

THRESHOLD = 0.02  # 2% rise fails the build


def measure(binary: str, scenario: str, ops: int) -> int:
    """Run one scenario under Cachegrind and return its instruction count."""
    out = subprocess.run(
        ["valgrind", "--tool=cachegrind", "--cache-sim=no",
         f"--cachegrind-out-file=/tmp/cg.{scenario}",
         binary, "--scenario", scenario, "--ops", str(ops)],
        capture_output=True, text=True, check=True,
    ).stderr
    m = re.search(r"I\s+refs:\s+([\d,]+)", out)
    if not m:
        sys.exit(f"could not parse I refs for {scenario} from:\n{out}")
    return int(m.group(1).replace(",", ""))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--ops", type=int, default=50_000)
    ap.add_argument("--update", action="store_true",
                    help="overwrite the baseline with the measured values")
    args = ap.parse_args()

    scenarios = ["rest_only", "cross_shallow", "cross_deep",
                 "cancel_heavy", "mixed_realistic", "worst_case_sweep"]

    measured = {s: measure(args.binary, s, args.ops) for s in scenarios}
    for s, n in measured.items():
        print(f"{s:<18} {n:>14,} instructions  ({n / args.ops:>9.1f} per op)")

    path = pathlib.Path(args.baseline)
    if args.update or not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"ops": args.ops, "instructions": measured},
                                   indent=2) + "\n")
        print(f"\nbaseline written to {path}")
        return 0

    baseline = json.loads(path.read_text())
    if baseline["ops"] != args.ops:
        sys.exit(f"baseline was taken at ops={baseline['ops']}, "
                 f"this run used {args.ops}; counts are not comparable")

    failed, missing = [], []
    print()
    for s in scenarios:
        if s not in baseline["instructions"]:
            missing.append(s)
            continue
        old, new = baseline["instructions"][s], measured[s]
        delta = (new - old) / old
        mark = "FAIL" if delta > THRESHOLD else "ok"
        print(f"{s:<18} {old:>14,} -> {new:>14,}  {delta:+7.3%}  {mark}")
        if delta > THRESHOLD:
            failed.append((s, old, new, delta))

    if missing:
        print(f"\nnew scenarios not in the baseline: {missing}. "
              f"Re-run with --update to record them.")

    if failed:
        print(f"\nREGRESSION: {len(failed)} scenario(s) exceeded "
              f"{THRESHOLD:.0%} more instructions per operation.")
        print("If this rise is intentional, re-run with --update and say why in "
              "the commit message and in docs/OPTIMIZATION-LOG.md.")
        return 1

    print(f"\nAll scenarios within {THRESHOLD:.0%}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

```bash
chmod +x scripts/check_regression.py
```

- [ ] **Step 3: Record the baseline and confirm the gate can both pass and fail**

```bash
docker run --rm -v "$PWD":/w -w /w alpine:3.20 sh -euc '
  apk add --no-cache cmake ninja g++ valgrind python3 >/dev/null 2>&1
  cmake -S . -B /tmp/bc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DOB_BUILD_BENCH=ON -DOB_BUILD_TESTS=OFF >/dev/null
  cmake --build /tmp/bc >/dev/null
  python3 scripts/check_regression.py --binary /tmp/bc/bench/ob_cachegrind_probe \
    --baseline bench/baselines/instructions.json --update
  echo "--- second run against the recorded baseline ---"
  python3 scripts/check_regression.py --binary /tmp/bc/bench/ob_cachegrind_probe \
    --baseline bench/baselines/instructions.json
'
```

Expected: the second run reports every scenario within a small fraction of a percent. **Then prove the gate can fail**, because a gate that cannot fail is not a gate:

```bash
python3 - <<'PY'
import json, pathlib
p = pathlib.Path("bench/baselines/instructions.json")
d = json.loads(p.read_text())
d["instructions"] = {k: int(v * 0.5) for k, v in d["instructions"].items()}
pathlib.Path("/tmp/fake_baseline.json").write_text(json.dumps(d, indent=2))
print("wrote a deliberately halved baseline to /tmp/fake_baseline.json")
PY
# Re-run against the halved baseline; it must exit 1.
```

Expected: exit code 1 with every scenario reported as roughly +100%.

- [ ] **Step 4: Add the CI jobs**

Append to `.github/workflows/ci.yml`:

```yaml
  differential:
    name: differential 10^7 operations
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y ninja-build
      - name: Configure and build
        run: |
          cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DOB_WARNINGS_AS_ERRORS=ON
          cmake --build build
      - name: Differential sweep
        # The seed base is derived from the commit, so coverage ACCUMULATES across
        # commits instead of re-running the same streams forever.
        env:
          OB_DIFF_OPS: 10000000
        run: |
          export OB_DIFF_SEED_BASE=$(( 0x$(git rev-parse --short=8 HEAD) ))
          echo "seed base $OB_DIFF_SEED_BASE"
          ./build/tests/ob_tests --gtest_filter=Differential.*

  fuzz:
    name: fuzz (short)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y ninja-build clang
      - name: Build fuzz target
        run: |
          CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_BUILD_FUZZ=ON -DOB_BUILD_TESTS=OFF
          cmake --build build-fuzz
      - name: Fuzz from the committed corpus
        run: ./build-fuzz/fuzz/ob_fuzz_differential -max_total_time=300 fuzz/corpus_min
      - name: Upload any crash
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: fuzz-crashes
          path: crash-*

  perf-gate:
    name: instruction-count regression gate
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install tools
        run: sudo apt-get update && sudo apt-get install -y ninja-build valgrind
      - name: Assert valgrind is present
        # A skipped gate is worse than a missing one: it reports green forever.
        run: valgrind --version
      - name: Build
        run: |
          cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DOB_BUILD_BENCH=ON -DOB_BUILD_TESTS=OFF
          cmake --build build
      - name: Check instruction counts against the baseline
        run: |
          python3 scripts/check_regression.py \
            --binary ./build/bench/ob_cachegrind_probe \
            --baseline bench/baselines/instructions.json
      - name: Upload a perf profile for inspection
        run: |
          sudo apt-get install -y linux-tools-common linux-tools-generic || true
          # Software event only: GitHub runners expose no hardware PMU.
          perf record -F 999 -e cpu-clock -g -o perf.data \
            ./build/bench/ob_cachegrind_probe --ops 200000 || true
          perf report -i perf.data --stdio --no-children 2>/dev/null \
            | head -50 > perf-report.txt || true
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: perf-report
          path: perf-report.txt
```

- [ ] **Step 5: Generate `docs/BENCHMARKS.md` from the results, never by hand**

Write `scripts/gen_benchmarks_md.py`:

```python
#!/usr/bin/env python3
"""Generate docs/BENCHMARKS.md from measured results.

Generated, never hand-edited, so the published numbers cannot drift from the
measurements that produced them. Re-run it after every benchmark run.
"""
import json
import pathlib
import platform
import subprocess
import sys


def read_jsonl(path: pathlib.Path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def main() -> int:
    results = pathlib.Path("bench/results")
    latency = read_jsonl(results / "latency.jsonl")
    throughput = read_jsonl(results / "throughput.jsonl")
    reference = read_jsonl(results / "reference_throughput.jsonl")
    if not latency and not throughput:
        sys.exit("no results in bench/results; run scripts/run_bench.sh first")

    ref_by_scenario = {r["scenario"]: r for r in reference}
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True).stdout.strip()

    out = [
        "# Benchmarks",
        "",
        "**Generated by `scripts/gen_benchmarks_md.py`. Do not hand-edit.**",
        "",
        f"- Commit: `{commit}`",
        f"- Machine: {platform.platform()}, {platform.processor() or platform.machine()}",
        "- Method: `docs/METHODOLOGY.md`. Median of 5 independent process runs,",
        "  100,000 discarded warm-up operations, open-loop issue schedule.",
        "",
        "## Read this before the numbers",
        "",
        "The finest timestamp granularity on the development machine is **41.67 ns**",
        "(measured: `mach_timebase` numer 125, denom 3). The target operation costs a",
        "few hundred nanoseconds. Two consequences:",
        "",
        "1. `batched_ns_per_op` comes from a single timestamp pair across the whole",
        "   run, so it is quantization-free. **It is the figure to trust for",
        "   per-operation cost.**",
        "2. The percentiles come from per-operation timestamps and therefore carry a",
        "   41.67 ns quantization floor. They are reported for the *shape* of the",
        "   tail, which is what matters in this domain, not for their absolute p50.",
        "",
        "No figure here comes from a hardware performance counter, because no PMU is",
        "accessible on this hardware, in the Docker VM, or on GitHub runners. Cache",
        "and branch figures elsewhere in the repo are Cachegrind *simulations* and",
        "are labelled as such.",
        "",
    ]

    if latency:
        out += ["## Latency, service time (nanoseconds)", "",
                "| scenario | batched/op | p50 | p90 | p99 | p99.9 | p99.99 | max | samples | allocs |",
                "|---|---|---|---|---|---|---|---|---|---|"]
        for r in latency:
            s = r["service_ns"]
            out.append(
                f"| `{r['scenario']}` | {r['batched_ns_per_op']:.1f} | {s['p50']:.0f} | "
                f"{s['p90']:.0f} | {s['p99']:.0f} | {s['p99_9']:.0f} | "
                f"{s['p99_99']:.0f} | {s['max']:.0f} | {r['ops']:,} | "
                f"{r['allocations']} |")
        out.append("")
        out += ["The `allocs` column is not decoration: it is asserted to be zero and",
                "the harness exits non-zero otherwise.", ""]

    if throughput:
        out += ["## Throughput", "",
                "| scenario | ops/sec | ns/op | reference ops/sec | speedup |",
                "|---|---|---|---|---|"]
        for r in throughput:
            ref = ref_by_scenario.get(r["scenario"])
            if ref:
                ratio = f"{r['ops_per_sec'] / ref['ops_per_sec']:.1f}x"
                ref_s = f"{ref['ops_per_sec']:,.0f}"
            else:
                ratio, ref_s = "n/a", "not measured"
            out.append(f"| `{r['scenario']}` | {r['ops_per_sec']:,.0f} | "
                       f"{r['ns_per_op']:.1f} | {ref_s} | {ratio} |")
        out.append("")
        out += ["Speedup is against `ReferenceEngine` measured under the identical",
                "harness, not against an absent baseline.", ""]

    out += ["## Reproducing every figure above", "",
            "```bash",
            "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_BUILD_BENCH=ON",
            "cmake --build build",
            "./scripts/run_bench.sh ./build/bench/ob_bench_latency    --ops 1000000",
            "./scripts/run_bench.sh ./build/bench/ob_bench_throughput --ops 5000000",
            "python3 scripts/gen_benchmarks_md.py",
            "```",
            "",
            "Raw per-sample data is committed under `bench/results/*.raw` as little-endian",
            "`uint32` counter ticks, so any percentile here can be recomputed independently",
            "rather than taken on trust.",
            ""]

    pathlib.Path("docs/BENCHMARKS.md").write_text("\n".join(out))
    print("wrote docs/BENCHMARKS.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Then produce the real document:

```bash
mkdir -p bench/results
./build/bench/ob_bench_latency    --ops 1000000 --raw-dir bench/results 2>/dev/null > bench/results/latency.jsonl
./build/bench/ob_bench_throughput --ops 5000000 2>/dev/null > bench/results/throughput.jsonl
python3 scripts/gen_benchmarks_md.py
cat docs/BENCHMARKS.md
```

- [ ] **Step 6: Update `README.md` with the real numbers**

Replace the "Performance: not yet measured" section with the measured headline figures, the honest caveats, and a link to `docs/BENCHMARKS.md`, `docs/METHODOLOGY.md` and `docs/OPTIMIZATION-LOG.md`. **Put the p99.9 next to the p50, not below the fold.** Also update the status line from "Phase 1 complete" to reflect Phase 2, and state plainly which spec success criteria are met and which are not.

- [ ] **Step 7: Verify every published claim against the spec's success criteria**

```bash
grep -n "p99_9\|p99\.9" docs/BENCHMARKS.md | head
grep -c '"allocations":0' bench/results/latency.jsonl   # must equal 6
grep -c '^### ' docs/OPTIMIZATION-LOG.md                 # baseline + one per candidate
python3 -c "import json;d=json.load(open('bench/baselines/instructions.json'));print(len(d['instructions']),'scenarios gated')"
```

Confirm by hand against spec section 2: S1 met (10^7 differential plus fuzzing, both in CI), S2 met, S3 met, S4 met, S5 met, S6 met, S7 met. **S8 belongs to Phase 3 and is not met**; say so in the README rather than leaving it ambiguous. If a spec performance target in 5.6 was missed, publish the real number and record why the target moved, per spec section 1.

- [ ] **Step 8: Commit**

```bash
git add bench/cachegrind_probe.cpp bench/baselines/instructions.json bench/results \
        scripts/check_regression.py scripts/gen_benchmarks_md.py \
        docs/BENCHMARKS.md README.md .github/workflows/ci.yml bench/CMakeLists.txt
git commit -m "ci: Cachegrind instruction-count gate, published results, README numbers

The gate counts instructions rather than timing, justified by measurement: two
Cachegrind runs of one identical binary differed by 1 instruction in 1,842,733,
about 1 part in 2,000,000, so a 2% threshold has ~40,000x margin and cannot
flake. Wall-clock on a shared runner varies by tens of percent, and a flaky
gate gets disabled, after which nobody notices the real regression.

BENCHMARKS.md is generated from the measured results and never hand-edited, so
published numbers cannot drift from the runs that produced them. Raw per-sample
data is committed so any percentile can be recomputed independently."
```

- [ ] **Step 9: Confirm before any push**

```bash
git log --oneline | head -30
git remote -v
```

Per the global constraints and spec O3, **nothing is pushed without explicit approval.** Report the commits and ask.

---

## Self-review

**Spec coverage.** Success criteria S1 (Task 8 differential plus Task 9 fuzzing), S2 (Task 7 internal invariants plus Phase 1's generic checker), S3 (Task 11 distributions), S4 (`docs/METHODOLOGY.md` from Phase 1, extended in Task 15), S5 (Task 13 log, Task 14 entries), S6 (Task 15 gate), S7 (Task 15 reproduction commands plus committed raw samples). S8 is Phase 3 and is out of scope here by construction. Spec 5.6's performance targets are measured in Tasks 11 and 12 and published in Task 15; a missed target publishes the real number with a recorded reason, per spec section 1. Edge cases E39 and E40 are covered in Tasks 3, 4 and 7; E45 is Task 8. Spec section 9's rejected alternatives reappear as measured candidates in Task 14 (identity hashing) rather than being asserted.

**Placeholder scan.** No "TBD", no "add appropriate error handling", no "similar to Task N". Task 14 is a procedure with five concrete named candidates and real code for each, not a promise to optimize something later; its outcomes cannot be written in advance because they are measurements, and the plan says exactly how each is obtained and recorded.

**Type consistency.** `Slot`/`kInvalidSlot` spelled identically across `order.hpp`, `order_pool.hpp`, `id_index.hpp`, `price_ladder.hpp`, `fast_engine.hpp`. `LevelBitmap::kNotFound` (not `kInvalidSlot`) is the bitmap's miss value throughout, and `PriceLadder::best()` converts it to `kNoPrice`. `unlink` returns `Qty` everywhere it is called. `Clock::raw_serialized` is used for every latency boundary and `Clock::raw` only inside `spin_until` and the overhead loops. `FastEngine::Config{n}` is brace-initialized consistently. `measure_workload` returns `WorkloadStats` whose `max_events_per_command` is what sizes every `EventBuffer` in `bench/`.

**Two hazards for the executor.**
1. `bench/alloc_counter.cpp` must **never** be linked into `ob_tests`. It replaces global `operator new`, which would perturb GoogleTest's own allocations and make the zero-allocation assertion meaningless. Task 11 gives it a separate executable for exactly this reason.
2. Task 14's candidate 2 (cached best price) is a stale-cache bug unless the debug `assert` comparing the cache against the bitmap is present. Do not drop that assert to make a test faster.
