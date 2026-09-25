# Implementation phases

**Spec:** [`docs/superpowers/specs/2026-09-22-order-book-matching-engine-design.md`](../specs/2026-09-22-order-book-matching-engine-design.md)

The spec covers four subsystems that can each ship independently. One phase per
subsystem, executed in order. **Each phase's end state is a repository a reader could
open and judge**, which is the property that protects a deadline-free project from
never shipping.

| Phase | Produces | End state |
|---|---|---|
| **[1. Foundation and correctness oracle](2026-09-22-phase-1-foundation-and-oracle.md)** | Build system, CI, core types, `ReferenceEngine`, the E1–E45 edge-case suite, invariant checker, scenario generator with shrinker, golden determinism tests | A correct, fully tested matching engine with green CI. Slow, and known to be slow. **Nothing is optimized yet, and nothing needs to be for this to be worth reading.** |
| **[2. Fast engine and the measurement story](2026-09-22-phase-2-fast-engine-and-measurement.md)** | `OrderPool`, `IdIndex`, `LevelBitmap`, `PriceLadder`, `FastEngine`, the latency/throughput harness, the six scenarios, profiling, the documented optimization arc, the Cachegrind CI gate | The headline result: published distributions, a before/after log, and a regression gate. This is the plan that carries the project. |
| **3. Market data and live demo** | `Seqlock`, `L2Snapshot`, the L2 publisher, the replay tool, the TUI | A live depth ladder driven by replayed flow, with a test proving a stalled reader cannot slow the matching thread. |
| **4. Wire protocol and ingest** *(optional, see spec O1)* | Fixed-layout binary decoder, zero-copy parse, busy-poll ingest loop, parser fuzz target | Order entry over a binary protocol. Nothing in Phases 1 – 3 depends on this. |

## Execution order and why

Phase 1 before Phase 2 is the non-negotiable one. `ReferenceEngine` is the oracle that
makes the optimizations in Phase 2 safe to attempt: every behavioral divergence an
optimization introduces gets caught by differential testing instead of by hoping a
hand-written test happened to cover that path. **Building the fast engine first would
mean optimizing without a correctness net, which is how fast wrong engines get
built.**

Phase 3 after Phase 2 because the TUI consumes L2 snapshots from the finished engine,
and because a demo of an unoptimized engine demonstrates nothing.

## Global constraints (apply to every task in every plan)

Copied verbatim from the spec. Every task's requirements implicitly include these.

- **C++20.** `set(CMAKE_CXX_STANDARD 20)`, `CMAKE_CXX_EXTENSIONS OFF`.
- **CMake >= 3.25** (needed for `FetchContent` `SYSTEM`, so dependency headers do not trip our warning flags), Ninja generator.
- **Validation order is fixed:** quantity, price range, duplicate ID, capacity,
  then PostOnly-would-cross. A command bad in two ways always reports the first.
- **Prices are integer ticks (`std::int32_t`). Floating point never touches a price
  or quantity.** Not in the engine, not in the tools, not in the tests.
- **No allocation, no exceptions, no syscalls, no virtual dispatch on the hot path.**
  `submit` violating any of these is a bug, and one is enforced by a test.
- **No `std::map`, `std::list`, `std::unordered_map`, `new`, or `malloc` anywhere in
  `FastEngine` or its components.** `ReferenceEngine` uses them deliberately.
- **Ladder bounds:** `kMinTick = 1`, `kMaxTick = 65536`, ladder size 65536 levels.
  Every externally supplied price is range-checked *before* being used as an index.
- **`sizeof(Order) == 32`** and **`sizeof(PriceLevel) == 24`**, both `static_assert`ed.
- **Cache line is 128 bytes on Apple Silicon, 64 on x86.** Use
  `std::hardware_destructive_interference_size`, never a hardcoded 64.
- **Timestamp resolution on the dev machine is 41.6667 ns** (measured). Per-operation
  cost is measured in batches; distributions state the resolution floor.
- **The CI performance gate is Cachegrind instruction count, never wall-clock time.**
  Threshold: 2% rise fails the build.
- **Warnings are errors** in CI: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion -Wold-style-cast -Wcast-align`.
- **Every commit message uses Conventional Commits** (`feat:`, `test:`, `fix:`,
  `perf:`, `docs:`, `build:`, `ci:`, `refactor:`).
- **Nothing is pushed to a remote without explicit approval** (spec O3).
- **Dependencies:** GoogleTest (tests only), FTXUI (TUI tool only). The engine,
  harness and fuzz targets depend on the standard library alone.

## Status

| Phase | Written | Tasks | Executed |
|---|---|---|---|
| 1. Foundation and correctness oracle | Yes | 13 | **Complete**, merged to main |
| 2. Fast engine and measurement | Yes | 15 | **Complete**, merged to main. The optimization arc landed one change (-30.3%) and rejected one candidate, both documented |
| 3. Market data and live demo | Yes | 6 | **Complete**, merged to main |
| 4. Wire protocol and ingest | Yes | 4 | **Complete**, merged to main |

**Current state on main:** 209 tests passing. All 40 spec edge cases against both
engines; 10^7 differential operations with zero divergence; 73,977 fuzz units with
zero crashes; clean under ASan, UBSan and TSan. 28.8 ns per operation and 34.8 M
ops/s on the realistic workload with zero allocations asserted, after an
optimization arc that improved the sum of per-op costs by 30.3%. A live terminal
depth ladder renders from a seqlock that the matching thread never waits on, and
orders can arrive over a fixed-layout binary protocol whose parser survived 146,576
fuzz runs.

Phase 2 additionally carries a pre-verification note: `LevelBitmap` and `IdIndex`
were compiled and tested against reference models, under ASan and UBSan, before the
plan was committed. A failure in those two during execution is a transcription
error, not an algorithm error.
