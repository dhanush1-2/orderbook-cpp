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

Median of **5 independent process runs**; the spread column is published and any
scenario over 10% invalidates the run (`scripts/run_bench.sh` exits non-zero, and it
did reject a first attempt at 12.49% before these numbers were taken).

| scenario | engine ns/op | throughput ops/s | spread | p99 ns | p99.9 ns | vs reference |
|---|---|---|---|---|---|---|
| `rest_only` | 7.7 | 129,580,257 | 4.21% | 149 | 232 | 10.8x |
| `cross_shallow` | 14.3 | 69,750,381 | 5.78% | 107 | 149 | 2.7x |
| `cross_deep` | 25.7 | 38,972,512 | 1.88% | 482 | 1024 | 3.8x |
| `cancel_heavy` | 9.8 | 102,519,541 | 3.77% | 107 | 149 | 3.7x |
| `mixed_realistic` | **28.8** | **34,761,566** | 7.48% | 148 | 190 | 195.1x |
| `worst_case_sweep` | 25.1 | 39,863,770 | 2.18% | 107 | 5440 | 3.7x |

Zero allocations in every measured window, asserted; the harness exits non-zero
otherwise.

`worst_case_sweep`'s p99.9 of 5.4 us is the 400-level sweep doing exactly what it is
designed to do. The multi-microsecond maxima are OS scheduling, published rather than
trimmed.

### Against the reference implementation

**The honest headline for the data-structure work is 2.7x to 10.8x.** The 195x on
`mixed_realistic` is real but comes from one asymptotic difference: it is the only
scenario with meaningful FOK volume, and the reference's FOK pre-scan sums individual
orders (O(orders)) where `FastEngine` sums level totals (O(levels)). Quoting 195x as
the headline would credit the flat ladder for an algorithmic win.

The reference allocates exactly 2.00 per operation on `rest_only` — the `std::map`
node plus the `std::list` node per resting order. That is precisely what the arena
removes.

### What the optimization bought

One optimization landed: the `IdIndex` hash. Full reasoning, including the candidate
that was measured and rejected, is in
[`docs/OPTIMIZATION-LOG.md`](OPTIMIZATION-LOG.md).

| scenario | SplitMix64 (before) | blocked (after) | change |
|---|---|---|---|
| `rest_only` | 23.1 ns | 13.3 ns | -42.3% |
| `cross_shallow` | 25.0 ns | 15.1 ns | -39.6% |
| `cross_deep` | 34.3 ns | 27.3 ns | -20.4% |
| `cancel_heavy` | 21.5 ns | 9.9 ns | -53.8% |
| `mixed_realistic` | 31.9 ns | 26.4 ns | -17.3% |
| `worst_case_sweep` | 32.8 ns | 25.4 ns | -22.5% |
| **sum of ns/op** | 168.6 ns | 117.4 ns | **-30.3%** |

Motivated by a direct experiment rather than intuition: holding the workload fixed
and varying only the engine capacity moved `cancel_heavy` from 7.6 ns/op (0.5 MB
index) to 33.0 ns/op (128 MB index), a **4.3x swing**. The engine is memory-bound on
the ID index.

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

> **The baseline below is STALE and the gate has a blind spot that matters here.**
> It was recorded with the SplitMix64 hash. The current default, the blocked hash,
> executes *more* instructions per lookup while being 30% faster, because it trades
> ALU work for cache locality. **The gate would flag that optimization as a
> regression.** That is the price of a deterministic metric, not a bug, and it is
> written up in [`docs/OPTIMIZATION-LOG.md`](OPTIMIZATION-LOG.md) entry 3.
> Re-record with `scripts/check_regression.py --update`, which needs Docker.

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
