# Measurement methodology

## Why this document exists

Benchmark numbers without a method are decoration. This file states what is
measured, how, and what could not be engineered away, so a skeptical reader can
attack the method directly.

It exists **before** any benchmark has been run, so the numbers in Phase 2 land on
top of a stated method rather than the method being reverse-engineered to fit the
numbers.

## Hardware and toolchain

Recorded per result, not assumed. The development machine:

- Apple M4 Pro, arm64, 8 performance cores + 4 efficiency cores
- Cache line **128 bytes**, L1d 64 KB, L2 4 MB
- Apple clang 21.0.0, C++20, CMake 4.4.3, Ninja
- macOS 15 (Darwin 25.6)

## The clock, measured rather than assumed

| Source | Nominal | Measured minimum delta | Read overhead |
|---|---|---|---|
| `mach_absolute_time` | 41.6667 ns/tick (`mach_timebase` numer 125, denom 3) | 1 tick = 41.67 ns | — |
| `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` | nanoseconds | 41 ns | 11.52 ns |
| `CLOCK_MONOTONIC_RAW`, `steady_clock` | nanoseconds | 41 ns | — |
| `CNTVCT_EL0` | `CNTFRQ_EL0` claims 1 GHz | median **42**, min 41 (the 1 GHz is fiction) | 0.43 ns unserialized, **16.6 ns serialized** |

`bench/clock.hpp` reports the **median** consecutive-read delta, not the minimum:
median **42.000 ns**, minimum **17.000 ns**, both reproducible to three decimal
places across five independent runs. The median is published because it is the
typical inter-tick gap and therefore the honest floor for any single measurement.

Getting that measurement right took two corrections, both worth recording:

1. **The first version reported only the minimum**, which is outlier-sensitive and
   optimistic. A harness quoting 17 ns as "its resolution" would understate its own
   error bars by 2.5x.
2. **The measurement itself was unsound.** It used the *unserialized* read, and two
   bare `mrs` instructions can complete out of order relative to each other, so the
   delta does not reflect elapsed time. The reported resolution swung between 1 ns
   and 42 ns across runs of the same binary. Measuring with `raw_serialized()` - the
   same read the harness uses for timing - made it exactly reproducible.

### Cross-platform confirmation

The same header, built on both platforms, reports frequencies that differ by 40x
and resolutions that agree to within one tick:

| Platform | `CNTFRQ_EL0` | ns/tick | Measured median resolution | Serialized read cost |
|---|---|---|---|---|
| macOS arm64 (host) | 1,000,000,000 (fiction) | 1.000000 | **42.000 ns** (min 17.000) | 8.7-18.6 ns, varies with machine load; measured and subtracted per run |
| Linux arm64 (Docker) | 24,000,000 (true) | 41.666667 | **41.667 ns** | 8.84 ns |

That agreement is the evidence that converting through `CNTFRQ_EL0` is correct on
both, rather than a hardcoded frequency happening to work on one. The serialized
read (`isb` + `mrs`) costs materially more than the bare read on both, which is why
the two are measured separately and why only the serialized figure is subtracted
from latency samples.

**The finest timestamp granularity available on this machine is ~41.67 ns.** The
target operation costs a few hundred nanoseconds. That ratio is the central
constraint on everything below.

The 1 GHz figure is demonstrably fiction: the same register read inside a Linux VM
on the same silicon reports the true **24 MHz**. So on macOS the counter is scaled
into nanosecond units but only *advances* every 41.67 ns. Converting through
`CNTFRQ_EL0` is correct on both platforms, which is why Phase 2's `bench/clock.hpp`
never hardcodes a frequency.

Consequences:

1. **Per-operation cost is measured in batches** (time N operations, divide by N).
   Quantization cancels; the distribution is lost.
2. **The distribution is measured per operation**, with the 41.67 ns resolution
   floor stated next to every figure.
3. **Both are cross-checked on x86-64 Linux**, where `rdtsc` resolves below a
   nanosecond, to confirm the distribution's shape is a property of the code and
   not of the clock. Docker does **not** help here: the container's counter is the
   same 24 MHz part.
4. **The CI regression gate uses no clock at all.** It counts instructions with
   Cachegrind, which is deterministic on a shared runner.

## Profiling and counters, measured availability

| Tool | Available? | Notes |
|---|---|---|
| `perf`, natively on macOS | **No** | Linux-only, and cannot be ported |
| `perf` in Docker | **Yes** | perf 6.6.31; `perf record -F 999 -e cpu-clock -g` verified to produce a correct call-graph profile |
| Hardware PMU, anywhere | **No** | The Docker VM's `/sys/bus/event_source/devices/` lists only `breakpoint kprobe software tracepoint uprobe`. `cache-misses` returns `<not supported>`; `cycles` and `instructions` are silently dropped. GitHub runners have none either |
| Instruments / `xctrace` | **No** | Command Line Tools only, no full Xcode |
| `sample` | Yes | Coarse, but works natively |
| Cachegrind in Docker | **Yes** | Valgrind 3.23.0 on aarch64. Needs `--cache-sim=yes` for D refs and miss rates |

**No figure this project publishes will come from a hardware performance counter,
because none is accessible anywhere in this setup.** Cache and branch figures are
Cachegrind *simulations* and are labelled as such wherever they appear.

### Why the CI performance gate counts instructions

Measured, not assumed: two Cachegrind runs of one identical binary reported
**1,842,733 and 1,842,734** instruction references. That is roughly 1 part in
2,000,000, so the 2% regression threshold has about 40,000x margin and cannot
flake. Wall-clock on a shared runner varies by tens of percent, so a wall-clock
gate either flakes constantly or is set so loose it catches nothing — and a flaky
gate gets disabled, after which nobody notices the real regression.

## Errors the Phase 2 harness avoids, and how

| Error | Handling |
|---|---|
| Clock resolution | Batched measurement for per-op cost; explicit resolution floor on distributions; x86 cross-check. **The harness counts what fraction of samples fall below the floor and flags a median that does, rather than printing a sub-resolution percentile as though it meant something.** On this hardware the median operation is faster than the clock can resolve, which is exactly why `batched_ns_per_op` is the figure to trust |
| Clock call overhead | Measured at startup with a serialized loop, printed, and subtracted. Serialized and unserialized reads are measured and reported separately, because a throughput loop under-reports (the reads pipeline) |
| Compiler eliding the work | Explicit barriers, plus a guard test asserting a deliberately dead benchmark body does *not* report ~0 ns |
| Coordinated omission | Open-loop driver: commands issued on a schedule computed in advance, latency measured from *intended* issue time |
| Cold cache and branch predictor | 100,000 discarded warm-up operations; the count is published |
| First-touch page faults | The pool, ladder and index are `std::vector`s whose value-initializing constructors write every byte, so every page is resident from construction. There is deliberately no separate `prefault()`, which would be a no-op with a reassuring name |
| P-core vs E-core migration | `QOS_CLASS_USER_INTERACTIVE`. macOS offers no hard affinity, so this is a stated limitation, not a solved problem |
| Thermal throttling | Median of 5 independent *process* runs; inter-run spread published; >10% spread invalidates the run |
| Discarding outliers | Not done. The tail is the product. Max is always published |
| Flattering input | Six scenarios including an explicit worst case, each with counters asserting it generated the workload it claims |
| No baseline | `ReferenceEngine` benchmarked under the identical harness, so speedups are ratios against a real measurement |

## Limitations that remain

Stated rather than buried:

- **41.67 ns quantization on per-operation latency.** A p50 near 250 ns carries
  roughly 8% granularity error on this machine. The x86 cross-check exists because
  of this, not in spite of it.
- **No hardware performance counters at all.** Cache-miss and branch-mispredict
  figures are simulated by Cachegrind, not measured on silicon.
- **No hard CPU affinity on macOS.** A run that migrates between P and E cores
  shows up as a bimodal distribution; such runs are flagged and repeated, not
  quietly dropped.
- **No Instruments locally.** Native profiling is `sample`; real profiles come from
  `perf` in Docker.
- **`ASAN_OPTIONS=detect_leaks=1` does not work on macOS.** LeakSanitizer is
  unsupported there and aborts every test. The CI ASan job is Linux-only, where it
  works; locally the option is omitted.
