# Benchmarks

Measured 2026-09-23. Regenerate with the commands at the bottom.

- **Hardware:** Apple M4 Pro, arm64, 8 P-cores + 4 E-cores, 128-byte cache line
- **Toolchain:** Apple clang 21.0.0, C++20, `-O3`, CMake 4.4.3 + Ninja
- **Method:** [`docs/METHODOLOGY.md`](METHODOLOGY.md). 100,000 discarded warm-up
  operations per scenario, seed 20260922

## Read this before the numbers

The finest timestamp granularity on this machine is **42.000 ns** (measured, median
of consecutive serialized reads, reproducible to three decimals across five runs).
The operation being measured costs roughly 30 ns. **The clock cannot resolve a
single operation**, so:

- **`engine ns/op` comes from a batched loop with no per-operation timestamping.**
  It is quantization-free and it is the figure to trust.
- **The percentiles come from per-operation timestamps** and carry the 42 ns floor.
  They are reported for the *shape* of the tail, which is what matters in this
  domain, not for their absolute p50. The harness counts what fraction of samples
  fall below the floor and flags a median that does.

No figure here comes from a hardware performance counter, because none is
accessible on this hardware, in Docker, or on GitHub runners.

## Per-operation cost and throughput

| scenario | engine ns/op | throughput ops/s | events/s | allocations |
|---|---|---|---|---|
| `rest_only` | 32.4 | 59,403,269 | 59,403,269 | 0 |
| `cross_shallow` | 25.3 | 46,113,166 | 115,282,915 | 0 |
| `cross_deep` | 30.3 | 30,288,068 | 65,964,201 | 0 |
| `cancel_heavy` | 23.4 | 53,153,430 | 53,153,430 | 0 |
| `mixed_realistic` | **28.9** | **32,971,501** | 51,033,999 | 0 |
| `worst_case_sweep` | 31.8 | 35,226,899 | 105,585,355 | 0 |

The allocations column is not decoration: it is asserted to be zero and the harness
exits non-zero otherwise.

## Latency distribution (service time, nanoseconds)

| scenario | p99 | p99.9 | p99.99 | max | below clock floor |
|---|---|---|---|---|---|
| `rest_only` | 149 | 232 | 566 | 22,353 | 6.4% |
| `cross_shallow` | 149 | 191 | 233 | 17,912 | 34.1% |
| `cross_deep` | 274 | 648 | 1,733 | 27,328 | 21.8% |
| `cancel_heavy` | 149 | 191 | 233 | 23,621 | 41.7% |
| `mixed_realistic` | 149 | 232 | 233 | 27,079 | 15.6% |
| `worst_case_sweep` | 149 | 3,440 | 3,900 | 16,103 | 22.3% |

`worst_case_sweep`'s p99.9 of 3.4 us is the 400-level sweep doing exactly what it
is designed to do. The multi-microsecond maxima across all scenarios are OS
scheduling, not the engine; they are published rather than trimmed.

## Against the reference implementation

Measured under the identical harness, not against an absent baseline.

| scenario | fast ops/s | reference ops/s | speedup | reference allocations/op |
|---|---|---|---|---|
| `rest_only` | 59,403,269 | 11,970,051 | 5.0x | 2.00 |
| `cross_shallow` | 46,113,166 | 25,856,256 | 1.8x | 1.50 |
| `cross_deep` | 30,288,068 | 10,254,535 | 3.0x | 2.40 |
| `cancel_heavy` | 53,153,430 | 27,542,450 | 1.9x | 1.50 |
| `mixed_realistic` | 32,971,501 | 178,206 | 185x | 1.07 |
| `worst_case_sweep` | 35,226,899 | 10,736,854 | 3.3x | 2.99 |

**The honest headline is 1.8x to 5.0x.** The 185x on `mixed_realistic` is real but
comes from one asymptotic difference, not from the data structures: it is the only
scenario with a meaningful share of FOK orders, and the reference's FOK pre-scan
sums individual orders (O(orders)) where `FastEngine` sums level totals (O(levels)).
Quoting 185x as the headline would attribute an algorithmic win to the flat ladder.

`reference allocations/op` of exactly 2.00 on `rest_only` is the `std::map` node plus
the `std::list` node per resting order. That is precisely what the arena removes.

## The instrument costs more than the thing measured

| scenario | engine ns/op | instrumented ns/op | harness overhead |
|---|---|---|---|
| `rest_only` | 32.4 | 94.0 | 61.5 |
| `cross_shallow` | 25.3 | 61.0 | 35.6 |
| `cross_deep` | 30.3 | 86.2 | 55.8 |
| `cancel_heavy` | 23.4 | 59.3 | 36.0 |
| `mixed_realistic` | 28.9 | 75.5 | 46.5 |
| `worst_case_sweep` | 31.8 | 86.0 | 54.2 |

Two serialized clock reads (`isb` + `mrs`, 8.7-18.6 ns each depending on machine
load) cost more than the operation between them. The harness prints both figures so
the gap is visible rather than folded into a single number.

## Deterministic instruction counts (the CI gate)

Cachegrind, `ops=200000`, `capacity=262144`. Determinism measured at **+0.000%**
across repeat runs, at most ~13 instructions out of 153 million.

| scenario | instructions | per op |
|---|---|---|
| `rest_only` | 109,189,249 | 545.9 |
| `cross_shallow` | 106,083,493 | 530.4 |
| `cross_deep` | 112,140,532 | 560.7 |
| `cancel_heavy` | 239,501,132 | 1,197.5 |
| `mixed_realistic` | 279,468,533 | 1,397.3 |
| `worst_case_sweep` | 126,761,251 | 633.8 |

**Known dilution:** `cancel_heavy` and `mixed_realistic` generate their streams with
a shadow engine, and that generation is still inside the counted region, roughly
doubling their totals. A 10% engine regression therefore shows as ~5% on those two,
still far above the 2% threshold. Recorded rather than hidden.

## Reproducing

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_BUILD_BENCH=ON
cmake --build build
./build/bench/ob_bench_latency    --ops 500000  --raw-dir bench/results
./build/bench/ob_bench_throughput --ops 2000000
./build/bench/ob_bench_throughput --ops 200000 --reference
./scripts/run_bench.sh ./build/bench/ob_bench_throughput --ops 2000000   # median of 5
```

Raw per-sample data is under `bench/results/*.raw` as little-endian `uint32` counter
ticks, so any percentile above can be recomputed independently rather than taken on
trust.
