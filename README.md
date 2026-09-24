# Order book and matching engine

A limit order book and matching engine in C++20. Price-time priority, five order
types, deterministic event output, and a measurement harness built to survive
someone attacking it.

**Status: Phase 2 complete.** `FastEngine` matches at **28.8 ns per operation /
34.8 M ops per second** on the realistic workload and **9.8 ns / 102 M ops per
second** on the cancel-heavy one, agrees with the reference engine over 10^7
generated operations plus coverage-guided fuzzing, and carries a documented
optimization arc including the candidate that was measured and rejected.

## What it does

- Five order types: `Limit`, `Market`, `Ioc`, `Fok`, `PostOnly`, plus `Cancel`
- Price-time priority. Trades print at the **maker's** price
- Integer tick prices. No floating point touches a price or a quantity, anywhere,
  including in the tests and the stream generator
- Deterministic, gap-free, sequenced event stream
- Every failure is a typed reason code, and a failed command leaves the book
  bit-for-bit unchanged
- Order IDs must be strictly increasing, which turns duplicate detection into one
  comparison instead of an unbounded set of retired IDs

## Where correctness stands right now

| Check | Result |
|---|---|
| Test count | **188 passing**, 2.2 s |
| Edge-case table | **40 cases** covering spec E1–E49, run against **both** engines via one type list |
| Invariants after every operation | 100,000 operations across 25 seeds, clean |
| Full ladder occupancy | 65,536 levels filled, both extremes, clean |
| Determinism across optimization levels | `-O0` and `-O3` produce the **identical** event-stream digest `d54a7c38cb35f3e3` over 104,880 events |
| ASan + UBSan | **Clean**, all tests |
| Differential vs reference | **10,000,000 operations**, zero divergence, 1.4 s |
| Fuzzing | **73,977 units**, zero crashes |
| Instruction-count gate | Deterministic to **+0.000%**; 2% threshold |

Five layers, weakest to strongest:

1. Unit tests per module
2. A **data-driven edge-case table** where the table is the specification and each
   row cites the spec case it covers
3. A **whole-book invariant checker** run after every operation, with two
   deliberately-broken engine doubles proving the checker can actually fail
4. **Random-stream stress testing** with invariants checked after each operation,
   plus a delta-debugging shrinker that reduces any failure to a minimal
   committable reproducer
5. **Cross-build determinism**, which catches undefined behaviour that happens to
   be benign at one optimization level

6. **Differential testing** of `FastEngine` against `ReferenceEngine`: 10,000,000
   generated operations comparing event streams *and* book state after every
   command, across four workload shapes. Zero divergence. On failure the stream is
   shrunk by delta debugging and printed as compilable C++.
7. **Coverage-guided fuzzing** with libFuzzer over an unsanitised decoder that
   feeds arbitrary prices, quantities and non-monotonic IDs: 73,977 units, zero
   crashes.

## Performance

Full results, method and caveats: [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md).

Median of 5 independent runs; a run with over 10% spread is rejected, not published.

| scenario | engine ns/op | throughput | p99 | p99.9 |
|---|---|---|---|---|
| `mixed_realistic` (headline) | **28.8** | **34.8 M ops/s** | 148 ns | 190 ns |
| `cancel_heavy` | 9.8 | 102.5 M ops/s | 107 ns | 149 ns |
| `rest_only` | 7.7 | 129.6 M ops/s | 149 ns | 232 ns |
| `cross_deep` | 25.7 | 39.0 M ops/s | 482 ns | 1,024 ns |
| `worst_case_sweep` | 25.1 | 39.9 M ops/s | 107 ns | 5,440 ns |

**Zero allocations** in every measured window, asserted by a replaced
`operator new` that fails the run if the counter moves.

Three things stated up front rather than buried:

- **Against the reference implementation the honest speedup is 2.7x to 10.8x.** One
  scenario shows 195x, but that is a single asymptotic difference in the FOK
  pre-scan (O(levels) versus O(orders)), not the flat ladder. Attributing it to the
  data structures would be misleading.
- **The one optimization that landed came from a failure.** Identity hashing in the
  ID index was 35-67% faster on five scenarios and 281% *slower* on the sixth,
  because mapping consecutive IDs to consecutive buckets makes backward-shift
  deletion O(run length). Understanding that produced a blocked hash which is faster
  on all six, -30.3% overall. Both are in
  [`docs/OPTIMIZATION-LOG.md`](docs/OPTIMIZATION-LOG.md), including the rejected one.
- **The CI instruction-count gate cannot see that optimization.** The blocked hash
  executes more instructions while running faster, so the gate reads it as a
  regression. A deterministic metric is worth having on a shared runner, and this is
  what it costs.
- **The clock cannot resolve one operation.** Measured timestamp granularity is
  42.000 ns; the operation costs ~29 ns. Per-operation cost therefore comes from an
  untimestamped batched loop, and the percentiles carry an explicit floor. The
  harness counts what fraction of samples fall below it.
- **The instrument costs more than the thing it measures:** two serialized clock
  reads are 35-62 ns against a 23-32 ns operation. Both figures are published so
  the gap is visible.

No number here comes from a hardware performance counter, because none is
accessible on this hardware, in Docker, or on GitHub runners. Cache and branch
figures elsewhere are Cachegrind *simulations* and say so.

## Building

```bash
brew install cmake ninja          # macOS
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful configurations:

```bash
# sanitizers (omit detect_leaks on macOS: LeakSanitizer is unsupported there)
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON
ASAN_OPTIONS=abort_on_error=1 ctest --test-dir build-asan --output-on-failure

# invariants asserted after every operation
cmake -S . -B build-inv -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOB_ENABLE_INVARIANTS=ON
```

Verify determinism across optimization levels:

```bash
for bt in Release Debug; do
  cmake -S . -B "build-$bt" -G Ninja -DCMAKE_BUILD_TYPE="$bt" >/dev/null
  cmake --build "build-$bt" >/dev/null
  "./build-$bt/tests/ob_tests" --gtest_filter=Determinism.EventStreamFingerprintIsStable \
    | grep FINGERPRINT
done
```

Both lines must be identical.

## Design

The full design document — including the alternatives that were rejected and why,
49 enumerated edge cases, the failure scenarios, and an adversarial review section
answering the questions a reviewer would actually ask — is in
[`docs/superpowers/specs/`](docs/superpowers/specs/2026-09-22-order-book-matching-engine-design.md).

The work is split into four phases, each ending with a repository someone could
open and judge: see [`docs/superpowers/plans/`](docs/superpowers/plans/README.md).
