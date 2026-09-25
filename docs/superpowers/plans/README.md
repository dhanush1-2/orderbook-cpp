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


---

# Part II: ITCH replay and the research agent

**Spec:** [`../specs/2026-09-25-itch-replay-and-research-agent-design.md`](../specs/2026-09-25-itch-replay-and-research-agent-design.md)

Phases 1–4 built a verified matching engine. Part II turns it into a research stack
that runs on **real Nasdaq ITCH 5.0 data**, with an LLM agent on top.

**The conceptual shift:** ITCH replay is book **reconstruction**, not matching. The
feed carries the exchange's own `OrderExecuted` messages, so the engine applies
events rather than deciding fills. The storage (ladder, bitmap, pool, index) and the
entire test and measurement apparatus carry over; the matching logic does not, and
stays as a separate capability.

| Phase | Produces | End state |
|---|---|---|
| **5. ITCH 5.0 feed decoder** | Data acquisition with md5 verification, a zero-copy decoder for all 22 message types, golden-file tests against real bytes, a fuzz target, and an `itch_stat` tool | Real Nasdaq data decodes correctly and at a measured rate. Nothing about the book has changed yet. |
| **6. Multi-symbol reconstruction** | Per-symbol ladder base and grid, `SymbolRouter` over 10 shards, SPSC parser→book queue, feed-consistency oracle, retargeted benchmarks | The headline: full book for 10 real symbols, verified against the exchange's own trades, with published msgs/sec and per-update latency. |
| **7. Python bindings and backtest** | pybind11 module, order-book-imbalance signal, fills with fees, Sharpe / max drawdown / turnover, and a written list of what the backtest does not model | A researcher can drive the C++ engine from Python and get honest numbers. |
| **8. MCP research agent** | `replay`, `book_snapshot`, `run_backtest`, `explain_pnl`, returning structured evidence rather than prose | An LLM can answer "why did this signal lose money on Tuesday?" from tool output alone, and every claim is checkable. |

## Two measured facts that shaped the design

Both came from decoding a real file before any code was written, and both would have
broken the existing engine silently:

1. **Order reference numbers are not monotonic** — 12,731 of 47,970 real AddOrder
   messages (26.5%) are non-increasing. The predecessor's `id <= high_water_`
   duplicate rule would reject a quarter of all orders. Spec 6.2 removes it and
   explains why a replay engine does not need it.
2. **Prices span $0.0004 to $100,000** at 4 implied decimals, which is 10^9 levels
   against a 65,536-entry ladder. But the worst per-symbol spread is **9,600 cents**,
   and there is zero sub-penny quoting at or above $1 (SEC Rule 612). So the ladder
   array is already the right size; it needs a per-symbol base and grid. Spec 6.3.

## Status

| Phase | Written | Executed |
|---|---|---|
| 5. ITCH decoder | Yes | Not started |
| 6. Multi-symbol reconstruction | Outline | Not started |
| 7. Python bindings and backtest | Outline | Not started |
| 8. MCP research agent | Outline | Not started |
