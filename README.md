# Order book and matching engine

A limit order book and matching engine in C++20. Price-time priority, five order
types, deterministic event output, and a measurement harness built to survive
someone attacking it.

**Status: Phase 1 complete.** The engine is correct and exhaustively tested. It is
not yet fast, and it is not yet claimed to be: `ReferenceEngine` uses `std::map`
and `std::list` on purpose. Phase 2 adds `FastEngine` and the measured results.

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
| Test count | **99 passing**, 0.75 s |
| Edge-case table | **40 cases** covering spec E1–E49, run against every engine via one type list |
| Invariants after every operation | 100,000 operations across 25 seeds, clean |
| Full ladder occupancy | 65,536 levels filled, both extremes, clean |
| Determinism across optimization levels | `-O0` and `-O3` produce the **identical** event-stream digest `d54a7c38cb35f3e3` over 104,880 events |
| ASan + UBSan | **Clean**, all 99 tests, 11.5 s |

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

Phase 2 adds a sixth: differential testing of `FastEngine` against
`ReferenceEngine` over more than 10^7 generated operations, plus libFuzzer.

## Performance

Not yet measured. Deliberately blank rather than aspirational.

The methodology is written first, in [`docs/METHODOLOGY.md`](docs/METHODOLOGY.md),
including the awkward parts. Two worth stating here:

- The development machine's finest timestamp granularity is **41.67 ns** while the
  target operation costs a few hundred, so per-operation cost is measured in
  batches and distributions carry an explicit resolution floor.
- **No hardware performance counter is accessible** on this machine, in Docker, or
  on GitHub runners, so cache and branch figures will be Cachegrind *simulations*
  and will say so. The CI regression gate counts instructions, which two runs of
  one binary showed to be deterministic to 1 part in 2,000,000.

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
