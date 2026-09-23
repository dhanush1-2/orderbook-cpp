# Design: Fast Order Book and Matching Engine (C++)

**Status:** Approved pending review
**Date:** 2026-09-22
**Author:** Dhanush Chandra Shekar
**Reviewers:** (self-review + adversarial review section below)

---

## 1. Problem

Build the core of an exchange: a limit order book and matching engine that accepts
orders, matches them against resting liquidity under price-time priority, and emits
a deterministic stream of trade and book events.

The problem is not "can matching be implemented." It can, in about 150 lines of
`std::map`. The problem is:

> **Implement matching correctly, make it measurably fast, and prove both claims
> with evidence a skeptical reader can reproduce.**

Restated as a testable objective:

> A single-threaded engine that processes a realistic order flow of limit, market,
> IOC, FOK and cancel operations on one symbol, producing output byte-identical to
> an independently written reference implementation, at a median per-order latency
> under 250 ns and a p99.9 under 2 us on an Apple M4 Pro P-core, with the
> latency distribution, the measurement methodology, and the before/after effect of
> each optimization published in the repository.

### Why those numbers

They are targets, not predictions, and the design doc commits to publishing whatever
the real numbers turn out to be. They are chosen to be demanding but not absurd:
a flat-ladder engine with no hot-path allocation should land in the low hundreds of
nanoseconds per order on modern hardware. If measurement shows the target was wrong,
the target moves and the reason is documented. **Moving a target with a recorded
reason is engineering. Quietly reporting only the runs that hit it is not.**

---

## 2. Users and success criteria

### Primary user

A reader evaluating the author's engineering ability for a quantitative trading or
low-latency systems role. This reader:

- has written or read a matching engine before, so shortcuts are visible to them
- will attack the benchmark methodology before the code, because that is where
  most candidate projects fall apart
- cares far more about p99.9 than about mean
- will ask "how do you know it's correct?" and expects better than "I wrote tests"

### Secondary user

The author, six months later, needing to explain every design decision from memory
in an interview. This is why the optimization log and methodology docs exist: the
repository has to be self-documenting under interrogation.

### Success criteria

| # | Criterion | How it is verified |
|---|---|---|
| S1 | Matching is correct under adversarial input | Differential test vs reference engine over >= 10^7 randomly generated operations, plus a libFuzzer target, both green in CI |
| S2 | Book invariants never break | Invariant checker asserted after every single operation in debug and fuzz builds |
| S3 | Latency is reported as a distribution, not a mean | `docs/BENCHMARKS.md` publishes p50/p90/p99/p99.9/p99.99/max per scenario |
| S4 | The measurement is defensible | `docs/METHODOLOGY.md` names each known benchmarking error and states how this harness avoids it, including the ones that could not be fully avoided |
| S5 | Optimizations are justified by evidence | `docs/OPTIMIZATION-LOG.md` has one entry per optimization: hypothesis, profile evidence, change, measured before/after, and any regression it caused |
| S6 | Performance regressions are caught automatically | CI fails on a >2% rise in instruction count per operation, measured deterministically |
| S7 | A reader can reproduce every number | One documented command per published figure, plus recorded hardware and compiler versions |
| S8 | The engine is demonstrable live | TUI renders the depth ladder in real time from a replayed order flow without perturbing the matching thread |

Note the asymmetry in S6: the CI gate is on **instruction count**, not wall-clock
time. Reasoning is in section 8.4.

---

## 3. Non-goals

Stated explicitly, because an unbounded scope is the main risk to a project with no
deadline.

| Not building | Why |
|---|---|
| Multi-symbol sharding across threads | The matching hot path stays single-threaded regardless, so this adds thread plumbing without making matching faster. The engine is designed to be instantiated per symbol so this stays possible later. |
| Order entry over TCP/UDP with a binary wire protocol | Deferred to an explicitly optional plan (Plan 4). It is the strongest remaining addition, but it is a networking project bolted onto a matching project and can be added without redesign. |
| Persistence, journaling, crash recovery | Real exchange concern, large, orthogonal to the performance story. |
| Risk checks, position limits, margin | Business logic, not engine mechanics. |
| Participant identity and self-trade prevention | Requires a participant model the core does not otherwise need. The `flags` byte in `Order` reserves room for it. |
| Pro-rata or size-priority allocation | Price-time FIFO is the dominant model and the one worth doing well. |
| Auctions, opening/closing crosses, halts | Session state machine, a separate subsystem. |
| Iceberg, stop, or pegged orders | Each needs a trigger/reveal mechanism that is a feature project of its own. |
| Decimal or floating-point prices | Prices are integer ticks. This is a correctness decision, not an omission. |
| Lock-free concurrent matching | Matching is inherently serial per symbol. Claiming otherwise would be theatre. |

---

## 4. Current system

There is no current system. The repository was empty at design time, which means:

- no existing patterns to follow, so this document sets them
- no backwards compatibility constraints
- no existing tests, deployment, or monitoring to integrate with
- every dependency choice is open, and therefore has to be justified

"Where should this feature naturally live" has one answer here: the design doc
defines the module boundaries before any code exists, and section 7.2 fixes them.

---

## 5. Requirements

### 5.1 Happy path

An order arrives. The engine validates it, matches it against the opposite side
while price permits, emits a `Trade` event per fill, and rests any unfilled
remainder. Best bid and best ask are updated. The caller receives an ordered,
gap-free event stream.

Concretely, for a buy limit order:

```
validate -> assign sequence -> while (remaining > 0 and best_ask exists and
            order.price >= best_ask): fill against FIFO head of best_ask level
         -> if remaining > 0 and order rests: insert at tail of own price level
         -> update best bid/ask cursors -> emit events
```

Trades execute at the **resting (maker) order's price**, not the incoming order's
price. A buy at 101.00 hitting an ask resting at 100.50 trades at 100.50. This is
correct exchange behavior and a common thing to get wrong.

### 5.2 Order types

| Type | Behavior | Rests? |
|---|---|---|
| `Limit` | Match while price permits, rest the remainder | Yes |
| `Market` | Match until filled or the opposite side is empty, ignoring price | No, remainder is cancelled |
| `Ioc` | Immediate-or-cancel: match what is available now, discard the remainder | No |
| `Fok` | Fill-or-kill: fill the entire quantity or do nothing at all | No |
| `PostOnly` | Reject outright if it would cross; otherwise rest | Yes, or rejected |

`Fok` is the interesting one and the reason it is included: it requires knowing the
fillable quantity **before** mutating any state, because a partial mutation followed
by a rollback is a bug factory. The design handles this with a non-mutating
`fillable_qty(side, limit_price)` pre-scan. That pre-scan is itself a performance
consideration (it walks levels twice in the worst case) and is a good thing to be
able to discuss.

`Cancel` removes a resting order by ID. `Cancel/Replace` is deliberately implemented
as cancel-then-new, which **loses time priority**. That matches how most venues
treat a price or size-increase amendment, and the alternative (in-place size
reduction preserving priority) is noted as a possible extension.

### 5.3 Edge cases

This is the section the "foolproof" requirement lives in. Every row becomes at least
one named test. Grouped by the mechanism that produces the bug.

**Empty and boundary book states**

| # | Case | Required behavior |
|---|---|---|
| E1 | Limit order into a completely empty book | Rests. No trade. Becomes both best and only level on its side. |
| E2 | Market order into an empty book | Zero fills, `Cancelled` with reason `NoLiquidity`. Not an error, not a crash. |
| E3 | `Fok` into an empty book | Zero fills, `Cancelled` with reason `Unfillable`. Book unchanged. |
| E4 | Last order on a side is filled | That side's best-price cursor becomes the no-price sentinel. Not tick 0, not -1 as a valid price. |
| E5 | Last order on a side is cancelled | Same as E4. Cursor invalidation must be driven by level emptiness, not by event type. |
| E6 | Both sides empty, then an order arrives | Works identically to E1. No stale cursor from the previous non-empty state. |
| E7 | Order rests at the lowest valid tick | Accepted. Bitmap word-0 bit-0 path exercised. |
| E8 | Order rests at the highest valid tick | Accepted. Bitmap last-word top-bit path exercised. |
| E9 | Every tick in the ladder occupied | Accepted until the order pool is exhausted. No overflow of the bitmap hierarchy. |

**Price and quantity validation**

| # | Case | Required behavior |
|---|---|---|
| E10 | Quantity is zero | `Rejected(InvalidQuantity)`. No state change. |
| E11 | Quantity exceeds `kMaxOrderQty` | `Rejected(InvalidQuantity)`. Guards level-total overflow. |
| E12 | Limit price below `kMinTick` | `Rejected(PriceOutOfRange)`. **Critical:** the ladder is a flat array, so an unvalidated price is an out-of-bounds write. |
| E13 | Limit price above `kMaxTick` | `Rejected(PriceOutOfRange)`. Same reason. |
| E14 | Price is not a whole number of ticks | Impossible by construction: the API takes integer ticks, never a decimal. Conversion happens at the boundary in the replay tool, which rejects non-multiples. |
| E15 | `Market` order carries a price field | Price is ignored, not validated. Documented, and asserted in a test so it cannot silently change. |

**Identity and lifecycle**

| # | Case | Required behavior |
|---|---|---|
| E16 | New order reuses a live order ID | `Rejected(DuplicateOrderId)`. Existing order untouched. |
| E17 | New order reuses the ID of a fully filled order | `Rejected(DuplicateOrderId)`. IDs are retired permanently, never recycled. Prevents a cancel racing a fill from hitting the wrong order. |
| E48 | New order whose ID is at or below the high-water mark, even one never used | `Rejected(DuplicateOrderId)`. **Order IDs must be strictly increasing.** See the note below: this is what lets duplicate detection be a single comparison instead of an unbounded set. A *rejected* command does not advance the mark, so its ID stays usable. |
| E49 | Order ID 0 | `Rejected(DuplicateOrderId)`, automatically: the high-water mark starts at 0 and the test is `id <= mark`. ID 0 is also the `IdIndex` empty-slot sentinel, so it must never be a real ID. |
| E18 | Cancel an ID that never existed | `Rejected(UnknownOrderId)`. No state change. |
| E19 | Cancel an ID already fully filled | `Rejected(UnknownOrderId)`. |
| E20 | Cancel an ID already cancelled | `Rejected(UnknownOrderId)`. Idempotence is **not** offered; double-cancel is an error the caller must see. |
| E21 | Cancel a partially filled order | Removes only the remaining quantity. Prior fills stand. |
| E22 | Cancel the order currently at the FIFO head | Level head advances correctly; level stays valid if others remain. |
| E23 | Cancel the order at the FIFO tail | Level tail retreats correctly. |
| E24 | Cancel the only order at a price level | Level is emptied, bitmap bit cleared, cursor advanced if it was the best. |
| E25 | Cancel an order in the middle of a level | Intrusive unlink; neighbours' links repaired; no search. |

**Why order IDs must be strictly increasing**

`ReferenceEngine` can hold a `std::set` of every ID it has ever seen. `FastEngine`
cannot: the set grows without bound and allocating is forbidden on the hot path.
Requiring IDs to increase strictly replaces that set with one `uint64_t` high-water
mark and one comparison, and it is not a real restriction, because order IDs come
from a sequencer at the venue boundary in every real system.

Two consequences worth stating, because both are testable:

1. An ID at or below the mark is rejected **even if it was never used** (E48). That
   is a deliberate strengthening of "no duplicates", not an accident.
2. A rejected command does not advance the mark, so a rejected ID remains usable.
   Only an `Accepted` advances it.

**Both engines apply the identical rule.** If they did not, differential testing
would diverge the first time the generator emitted a non-monotonic ID.

**Matching mechanics**

| # | Case | Required behavior |
|---|---|---|
| E26 | Incoming quantity exactly equals one resting order | One trade, resting order fully filled and removed, nothing rests. |
| E27 | Incoming smaller than the resting order | One trade, resting order reduced and **keeps its time priority**. |
| E28 | Incoming larger than one resting order at a level | Fills in strict FIFO order within the level. |
| E29 | Incoming sweeps multiple price levels | Levels consumed in price order, best first. Each emptied level's bitmap bit cleared. |
| E30 | Incoming limit price equals the best opposite price | **Crosses.** The comparison is inclusive: buy crosses if `price >= best_ask`. |
| E31 | Incoming limit price is one tick worse than best opposite | Does not cross. Rests. Spread narrows to zero width but stays uncrossed. |
| E32 | Incoming sweeps the entire opposite side and has remainder | Opposite side becomes empty, remainder rests, `Limit` only. |
| E33 | `PostOnly` that would cross | `Rejected(WouldCross)`. Nothing rests, no trade. |
| E34 | `PostOnly` that would not cross | Rests normally. |
| E35 | `Fok` fillable exactly | Fills completely in one operation. |
| E36 | `Fok` fillable one unit short | Zero trades, `Cancelled(Unfillable)`, **book bit-for-bit unchanged**. The pre-scan must not mutate. |
| E37 | `Ioc` partially fillable | Fills what it can, remainder cancelled, never rests. |
| E38 | Two orders at the same price and same arrival batch | Sequence number breaks the tie; FIFO by sequence, deterministically. |

**Resource exhaustion**

| # | Case | Required behavior |
|---|---|---|
| E39 | Order pool exhausted | `Rejected(EngineCapacity)`. No allocation, no crash, no UB. Engine remains fully usable, and freeing an order makes capacity available again. |
| E40 | ID index at capacity | Same. The index is pre-sized and never rehashes during operation; hitting the load-factor ceiling is a capacity rejection. |
| E41 | Level total quantity would overflow | Prevented upstream by E11 combined with a capacity bound; asserted as an invariant so the bound cannot silently drift. |
| E42 | Sequence number overflow | `uint64_t` at 10^8 ops/sec overflows in ~5800 years. Documented as accepted, not guarded. |

**Determinism**

| # | Case | Required behavior |
|---|---|---|
| E43 | Same input stream replayed twice | Byte-identical event output, including sequence numbers. |
| E44 | Same input, different build (`-O0` vs `-O3`, clang vs gcc) | Byte-identical event output. No UB-dependent or allocation-address-dependent behavior. |
| E45 | Same input, reference engine vs fast engine | Byte-identical event output. This is the differential test oracle. |

**Output buffer contract**

| # | Case | Required behavior |
|---|---|---|
| E46 | `EventBuffer` sized exactly to the events one command produces | Succeeds, no overflow, no reallocation. The engine never owns output memory. |
| E47 | `EventBuffer` too small for the events a command produces | Debug assertion failure. This is a caller programming error, not a runtime condition, because making it recoverable would break the atomicity guarantee in 5.4.3. The engine documents the bound: `2 + 2 x (orders reachable by one sweep)`. |

### 5.4 Failure cases

Every failure is a typed `Rejected` or `Cancelled` event with a reason code. The
engine has exactly one way to fail: it returns an event saying so.

Rules the engine holds to:

1. **No exceptions on the hot path.** Validation returns reason codes. Exceptions
   are reserved for construction-time configuration errors.
2. **No allocation on the hot path.** All memory is reserved at construction. An
   out-of-capacity condition is a rejection, never an allocation.
3. **Failure is atomic.** A rejected operation leaves state exactly as it was. For
   `Fok` this is enforced by pre-scanning; for everything else by validating before
   mutating.
4. **Failure is visible.** There is no silent drop. Every input produces at least
   one output event.

### 5.5 Permissions and security

There is no authentication, no network surface, and no multi-tenancy in scope, so
classical security requirements largely do not apply. What does apply is **memory
safety**, because the performance design deliberately uses raw indices into
pre-allocated arrays, which is exactly the shape of code that produces exploitable
bugs:

- Every externally supplied price is range-validated before being used as an index.
- Every externally supplied order ID goes through the index, never arithmetic.
- Slot indices are `uint32_t` with an explicit `kInvalidSlot` sentinel, and
  dereferencing the sentinel is an assertion failure in debug builds.
- ASan, UBSan and the fuzzer run in CI. A memory-safety finding is a build failure,
  not a ticket.
- The replay tool parses untrusted input (a file). Its parser is bounds-checked and
  is itself a fuzz target, because "it's just a local file" is how parsers get
  shipped without review.

### 5.6 Performance requirements

| Metric | Target | Scenario |
|---|---|---|
| Per-order latency p50 | < 250 ns | `mixed_realistic` |
| Per-order latency p99 | < 600 ns | `mixed_realistic` |
| Per-order latency p99.9 | < 2 us | `mixed_realistic` |
| Throughput, single thread | > 5 M orders/sec | `mixed_realistic` |
| Cancel latency p50 | < 80 ns | `cancel_heavy` |
| Hot-path allocations | Exactly 0 | all scenarios, asserted |
| Hot-path syscalls | Exactly 0 | all scenarios, asserted |

Two of these are worth more than the rest. **"Exactly 0 allocations" is a
verifiable claim, not an aspiration** — it is checked by overriding
`operator new` in the benchmark build and asserting the counter stays flat across
the measured window. A claim that can fail a test is worth more than a number that
depends on the machine.

### 5.7 Scale

Single symbol, single thread, in-process. Sizing at construction:

- Order pool: 1 M orders default, configurable. 1 M x 32 B = 32 MB.
- Price ladder: 65,536 ticks default. 65,536 x 24 B = 1.5 MB, which fits in the
  4 MB L2 of this machine. This is a deliberate choice, see section 9.2.
- ID index: sized to 2 x pool capacity, power of two, never rehashed.

At 10x the expected order rate the engine does not change: it is a single thread
doing serial work, and the answer to "what happens at 10x traffic" is "the queue in
front of it grows, and you shard by symbol." That is an honest answer and the design
keeps it available by making the engine a per-symbol object with no global state.

### 5.8 Compatibility

No existing clients, so no backwards compatibility burden. Forward compatibility
that *is* required:

- The event stream is versioned with a format tag so recorded golden outputs stay
  interpretable when events are added.
- `Engine` is a compile-time interface (a concept, not a virtual base) implemented
  by both `ReferenceEngine` and `FastEngine`, so tests, benchmarks and tools are
  written once and run against either. **No virtual dispatch on the hot path.**

---

## 6. Constraints

### 6.1 Verified platform facts

Measured on this machine on 2026-09-22, not assumed:

| Fact | Value | Consequence |
|---|---|---|
| CPU | Apple M4 Pro, arm64, 8 P-cores + 4 E-cores | Benchmarks must run on a P-core; macOS offers QoS hints, not hard pinning |
| Cache line | **128 bytes** | Padding for false sharing is 128 here, 64 on x86. Do not hardcode 64. |
| L1d / L2 | 64 KB / 4 MB | Ladder sized to stay in L2 |
| `mach_timebase` | numer 125, denom 3 = **41.6667 ns/tick** | **Finest available timestamp granularity is ~41.67 ns** |
| `mach_absolute_time` min delta | 1 tick = 41.67 ns | Cannot time a single sub-100 ns operation directly |
| `clock_gettime_nsec_np` min delta | 41 ns, overhead 11.5 ns | Reports ns, delivers 41.67 ns resolution |
| `CNTVCT_EL0` | `CNTFRQ` claims 1 GHz; real min delta ~42 ns; read overhead 0.32 ns amortized | Cheapest timestamp source, same resolution floor. The 1 GHz is fiction: the same register read inside the Docker VM reports the true 24 MHz, so on macOS the counter is in nanosecond units but only *advances* every 41.67 ns. Converting via `CNTFRQ_EL0` is correct on both platforms and is what `bench/clock.hpp` does |
| `perf` natively | **Not available.** Linux-only. | Native macOS profiling is `sample` only |
| `perf` in Docker | **Available and working.** perf 6.6.31, `perf record -F 999 -e cpu-clock -g` verified to produce a correct call-graph profile | **Local flamegraphs are possible**, on aarch64, the same ISA as the host |
| Hardware PMU, anywhere | **Absent.** `/sys/bus/event_source/devices/` in the VM lists only `breakpoint kprobe software tracepoint uprobe`. `cache-misses` returns `<not supported>`; `cycles` and `instructions` are silently dropped | No real cache-miss or branch-miss counts. Those come from Cachegrind simulation or not at all |
| Instruments / `xctrace` | **Not available.** Command Line Tools only, no full Xcode | Local profiling is `sample` plus manual instrumentation, unless Xcode is installed |
| Docker | **Running.** linux/aarch64, kernel 6.10.14-linuxkit, 12 CPUs, 8 GB, `perf_event_paranoid = 2` | The local profiling environment. Software events only |
| Docker clock | `CNTFRQ_EL0 = 24000000 Hz` exactly = 41.6667 ns/tick; `CLOCK_MONOTONIC` min delta 41 ns | **No resolution improvement over the host.** The x86 cross-check is still required |
| Cachegrind in Docker | Valgrind 3.23.0, works on aarch64. Two runs of an identical binary: 1,842,733 vs 1,842,734 I refs | Deterministic to ~1 part in 2,000,000, so the 2% CI gate has ~40,000x margin. Needs `--cache-sim=yes` for D refs and miss rates |
| Homebrew | 6.0.13 present | `cmake` is installable; it is currently missing |
| `cmake`, `gh` | **Not installed** | Setup task installs them |

### 6.2 The measurement constraint, stated plainly

The single most important constraint in this project: **the target operation takes
roughly the same time as the clock's resolution.** A 250 ns operation measured with
a 41.67 ns-granular clock is quantized into buckets six wide. That is tolerable for
a distribution and intolerable for a single-operation figure.

The design's answer, in full, is section 8. In short:

1. Per-operation cost is measured in **batches** (time N operations, divide),
   which removes quantization entirely but destroys the distribution.
2. The **distribution** is measured with per-operation timestamps, reported with
   the 41.67 ns quantization floor stated next to every figure.
3. Both are cross-checked on **x86-64 Linux in CI**, where `rdtsc` gives
   sub-nanosecond resolution, to confirm the shape of the distribution is a
   property of the code and not of the clock.
4. The CI **regression gate** avoids clocks altogether and counts instructions
   (section 8.4).

A reader who attacks the benchmark will attack exactly here. The document gets
ahead of them.

### 6.3 Other constraints

- **Deadline:** none. Highest-quality version is the goal. The risk this creates is
  unbounded scope, which is why section 3 is long and the work is split into four
  independently shippable plans.
- **Cost:** zero. GitHub Actions free tier, no cloud VMs, no paid tooling.
- **Team:** one person. No ownership boundaries, so the constraint is
  context-switching cost, which argues for small focused files.
- **Dependencies:** kept minimal and each one justified in section 9.5. GoogleTest
  for tests, FTXUI for the TUI, nothing on the engine's hot path. The engine itself
  depends on the standard library only.

---

## 7. Proposed design

### 7.1 Architecture

```
                        ┌──────────────────────────────┐
  order file / gen ───► │  Scenario source             │
                        │  (deterministic, seeded)     │
                        └──────────────┬───────────────┘
                                       │  Command  (value type, no allocation)
                                       ▼
    ┌──────────────────────────────────────────────────────────────────┐
    │                       MATCHING THREAD                            │
    │                                                                  │
    │   ┌──────────────┐                                               │
    │   │  Validation  │── reject ──────────────────────────┐          │
    │   └──────┬───────┘                                    │          │
    │          │ valid                                      │          │
    │          ▼                                            │          │
    │   ┌──────────────────────────────────────────┐        │          │
    │   │            Matching core                  │        │          │
    │   │                                           │        │          │
    │   │   PriceLadder ◄──► LevelBitmap            │        │          │
    │   │      (flat array   (hierarchical, O(1)    │        │          │
    │   │       of levels)    next-occupied-level)  │        │          │
    │   │         │                                 │        │          │
    │   │         ▼                                 │        │          │
    │   │   OrderPool  ◄──► IdIndex                 │        │          │
    │   │   (arena +        (order id -> slot,      │        │          │
    │   │    free list)      O(1) cancel)           │        │          │
    │   └──────────────────┬───────────────────────┘        │          │
    │                      │                                 │          │
    │                      ▼                                 ▼          │
    │             ┌────────────────────────────────────────────┐        │
    │             │  EventSink  (sequenced, gap-free)          │        │
    │             └────────┬───────────────────────┬───────────┘        │
    │                      │                       │                    │
    │                      ▼                       ▼                    │
    │            ┌──────────────────┐   ┌────────────────────┐          │
    │            │ L2 publisher     │   │ latency recorder   │          │
    │            │ (seqlock         │   │ (bench builds only)│          │
    │            │  snapshot)       │   └────────────────────┘          │
    │            └────────┬─────────┘                                   │
    └─────────────────────┼───────────────────────────────────────────┘
                          │  lock-free read, never blocks the writer
                          ▼
                 ┌────────────────────┐
                 │   TUI THREAD       │
                 │   30 fps render,   │
                 │   drops stale      │
                 └────────────────────┘
```

The load-bearing property of this architecture: **nothing downstream of the matching
core can slow it down.** The L2 publisher writes into a seqlock, so a stalled or slow
TUI reader cannot block the writer, it just observes a newer snapshot next time. The
latency recorder is compiled out of release builds entirely. This is the market-data
discipline of a real venue, in miniature, and it is the reason the TUI is safe to add
to a latency-sensitive engine.

### 7.2 Modules and boundaries

Each file has one responsibility, a defined interface, and can be tested alone.

| File | Responsibility | Depends on |
|---|---|---|
| `include/ob/types.hpp` | Scalar aliases, `Side`, `OrderType`, `RejectReason`, ladder bounds, compile-time size assertions | nothing |
| `include/ob/command.hpp` | `Command` input value type and its factory functions | types |
| `include/ob/events.hpp` | `Event` output value type, reason codes, equality and serialization for golden tests | types |
| `include/ob/order.hpp` | The 32-byte `Order` slot, intrusive link fields | types |
| `include/ob/order_pool.hpp` | Arena of order slots with an index free list. `alloc`/`free`/`at`/`capacity` | order |
| `include/ob/id_index.hpp` | Open-addressed `OrderId -> slot` map, pre-sized, never rehashes | types |
| `include/ob/level_bitmap.hpp` | Hierarchical occupancy bitmap. `set`/`clear`/`next_set_at_or_above`/`prev_set_at_or_below` | types |
| `include/ob/price_ladder.hpp` | Flat array of `PriceLevel` plus best-price cursors; FIFO push/pop/unlink within a level | order_pool, level_bitmap |
| `include/ob/engine_concept.hpp` | The compile-time `Engine` concept both implementations satisfy, plus `EventBuffer` | command, events |

**The library is header-only** and exposed as a CMake `INTERFACE` target. Rationale:
the hot path depends on cross-module inlining (`PriceLadder` into `FastEngine`), a
translation-unit boundary there would cost real nanoseconds unless LTO is guaranteed,
and LTO is not guaranteed across every compiler in the CI matrix. The cost is compile
time, which is acceptable at this size.
| `include/ob/reference_engine.hpp` | Deliberately simple, obviously correct matcher. **Never optimized.** | command, events |
| `include/ob/fast_engine.hpp` | The real matcher | price_ladder, order_pool, id_index |
| `include/ob/l2_snapshot.hpp` | Top-N depth snapshot value type | types |
| `include/ob/seqlock.hpp` | Single-writer multi-reader seqlock for snapshot publication | nothing |
| `include/ob/invariants.hpp` | Whole-book invariant checker used by tests and fuzzer | fast_engine |
| `bench/clock.hpp` | Platform timestamp abstraction with measured overhead and resolution | nothing |
| `bench/histogram.hpp` | Exact-percentile recorder over raw samples | nothing |
| `bench/scenarios.hpp` | The six deterministic workload generators | command |
| `tests/model/scenario_gen.hpp` | Random operation-stream generator plus the failure shrinker | command |

`ReferenceEngine` existing purely to be the oracle is the most important structural
decision in the document. It is what makes aggressive optimization safe: any
behavioral divergence introduced by an optimization is caught by differential
testing rather than by hoping a hand-written test covered that path.

### 7.3 Data model

```cpp
// types.hpp
namespace ob {

using OrderId = std::uint64_t;
using Ticks   = std::int32_t;   // price, in whole ticks. never floating point.
using Qty     = std::uint32_t;  // per-order quantity
using QtySum  = std::uint64_t;  // aggregates, wider to make level overflow unreachable
using Seq     = std::uint64_t;
using Slot    = std::uint32_t;  // index into OrderPool

inline constexpr Slot  kInvalidSlot = 0xFFFF'FFFFu;
inline constexpr Ticks kNoPrice     = std::numeric_limits<Ticks>::min();
inline constexpr Qty   kMaxOrderQty = 1u << 31;

enum class Side      : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit, Market, Ioc, Fok, PostOnly };

enum class RejectReason : std::uint8_t {
    None = 0, InvalidQuantity, PriceOutOfRange, DuplicateOrderId,
    UnknownOrderId, WouldCross, EngineCapacity,
};
enum class CancelReason : std::uint8_t {
    None = 0, UserRequested, NoLiquidity, Unfillable, IocRemainder,
};

}  // namespace ob
```

```cpp
// order.hpp  -- exactly 32 bytes, 2 per x86 cache line, 4 per Apple 128B line
struct Order {
    OrderId  id;        // 8
    Ticks    price;     // 4
    Qty      remaining; // 4
    Slot     next;      // 4  intrusive FIFO, index not pointer
    Slot     prev;      // 4
    Side     side;      // 1
    std::uint8_t flags; // 1  reserved: participant/self-trade-prevention
    std::uint16_t pad;  // 2
};
static_assert(sizeof(Order) == 32, "Order must stay 32 bytes");
static_assert(alignof(Order) == 8);
```

Indices rather than pointers, for three reasons worth being able to state: the struct
is smaller, the pool can be reallocated or serialized without fixing up pointers, and
a bad index is caught by a bounds assert while a bad pointer is undefined behavior.

```cpp
// price_ladder.hpp
struct PriceLevel {
    Slot   head  = kInvalidSlot;  // 4  FIFO front, oldest order
    Slot   tail  = kInvalidSlot;  // 4  FIFO back, newest order
    QtySum total = 0;             // 8  sum of remaining qty at this level
    std::uint32_t count = 0;      // 4  order count, for L2 output
    std::uint32_t pad   = 0;      // 4
};
static_assert(sizeof(PriceLevel) == 24);
```

```cpp
// events.hpp -- the engine's entire output vocabulary
enum class EventType : std::uint8_t {
    Accepted, Rejected, Trade, Cancelled, Filled,
};

struct Event {
    Seq       seq;         // strictly monotonic, gap-free, deterministic
    EventType type;
    OrderId   order_id;    // the order this event is about (taker, for Trade)
    OrderId   maker_id;    // Trade only, else 0
    Ticks     price;       // Trade: execution price (the maker's price)
    Qty       qty;         // Trade: filled qty. Cancelled: qty removed.
    RejectReason reject;
    CancelReason cancel;
};
```

Trade carries both sides' IDs because a feed consumer needs to attribute a fill to
both participants, and because the differential test compares maker attribution,
which is the part that breaks when FIFO order is subtly wrong.

### 7.4 API contract

```cpp
// The compile-time interface. No virtual functions: the benchmark must not
// measure vtable dispatch, and the optimizer must be able to inline.
template <class E>
concept Engine = requires(E e, const Command& c, EventBuffer& out) {
    { e.submit(c, out) } -> std::same_as<void>;
    { e.best_bid() } -> std::same_as<Ticks>;   // kNoPrice when empty
    { e.best_ask() } -> std::same_as<Ticks>;
    { e.snapshot_l2(std::declval<L2Snapshot&>()) } -> std::same_as<void>;
    { e.reset() } -> std::same_as<void>;
};
```

Contract terms, each of which becomes a test:

1. `submit` appends one or more events to `out` and **never** fewer than one.
2. `submit` never allocates, never throws, never blocks, never makes a syscall.
3. Sequence numbers are assigned in append order with no gaps across the process.
4. For identical construction parameters and identical command sequences, the event
   stream is byte-identical. Across engines, compilers and optimization levels.
5. `best_bid() < best_ask()` holds after every `submit` returns, whenever both sides
   are non-empty. The book is never observably crossed.
6. A `Rejected` outcome leaves the book bit-for-bit unchanged.

`EventBuffer` is a caller-provided, pre-sized span. The engine never owns output
memory, which is how requirement 2 is kept true.

### 7.5 Data flow for one crossing order

Tracing the example from the problem statement, buy 350 @ 100.50 into asks of
300 @ 100.50 (order B), 100 @ 100.50 (order C), 200 @ 101.00 (order A):

```
submit(Command{New, Buy, Limit, id=99, px=10050, qty=350})
 │
 ├─ validate: qty in range, px in ladder, id 99 not in IdIndex            -> ok
 ├─ seq = next++                                                          -> emit Accepted(99)
 ├─ best_ask() = LevelBitmap.next_set_at_or_above(ask_cursor) = 10050
 ├─ 10050 <= 10050, so it crosses. enter match loop:
 │    ├─ level = ladder[10050]; slot = level.head -> B (300)
 │    │    fill = min(350, 300) = 300
 │    │    B.remaining -> 0, level.total -= 300, level.count -= 1
 │    │    unlink B from level head; IdIndex.erase(B); pool.free(B)
 │    │    emit Trade(taker=99, maker=B, px=10050, qty=300)
 │    │    remaining = 50
 │    ├─ slot = level.head -> C (100)
 │    │    fill = min(50, 100) = 50
 │    │    C.remaining -> 50  (stays, keeps time priority)
 │    │    level.total -= 50
 │    │    emit Trade(taker=99, maker=C, px=10050, qty=50)
 │    │    remaining = 0
 │    └─ remaining == 0 -> exit loop. Never reads 10100.
 ├─ remaining == 0 -> nothing rests. emit Filled(99)
 └─ level 10050 still has C, so bitmap bit stays set, ask cursor unchanged
```

Now the 1000 @ 100.50 variant: after consuming 400 at 10050, `level.count` reaches 0,
so `LevelBitmap.clear(10050)` fires and `next_set_at_or_above` advances the ask cursor
to 10100. The loop then tests `10050 >= 10100`, which is false, so it exits. The
remaining 600 is inserted at the tail of `ladder[10050]` on the bid side,
`LevelBitmap.set(10050)` on the bid bitmap, and the bid cursor moves up to 10050.
Best bid 100.50, best ask 101.00. Uncrossed, invariant holds.

### 7.6 Error handling

One mechanism, applied uniformly: validate fully before mutating anything, and
return a typed reason. Specifically:

- **Order of validation matters** and is fixed so reason codes are deterministic:
  quantity, then price range, then duplicate ID, then capacity, then
  PostOnly-would-cross. A command that is bad in two ways always reports the first
  in that list. `WouldCross` is part of *validation* rather than execution because
  E33 requires a `Rejected`, and a `Rejected` is the only event a failed command
  emits: it cannot follow an `Accepted`.
- **Capacity is checked only for order types that can rest** (`Limit`, `PostOnly`),
  conservatively, before matching and therefore before it is known whether the order
  would have fully filled. Both engines must apply the identical rule or differential
  testing will diverge on a full book.
- **Cancel reason for an unfilled remainder** is `NoLiquidity` when zero fills
  occurred and `IocRemainder` when at least one fill occurred. Applies to `Market`
  and `Ioc` alike.
- **`Fok` gets a pre-scan.** `fillable_qty(side, limit)` walks levels without
  mutating and returns the total available. Only if it is at least the order
  quantity does the mutating match loop run. This is the only place in the design
  where the same levels are walked twice, and the cost is documented in the
  benchmark as a separate scenario.
- **Capacity is checked before the first mutation,** not when the allocation fails.
  A `Limit` order that will rest requires a free pool slot and an index slot; both
  are reserved up front.
- **Debug builds assert the invariants** after every operation. Release builds do
  not, because the check is O(ladder) and would dominate the measurement. The fuzzer
  therefore runs a debug build.

---

## 8. Measurement design

This section exists because for this project the measurement *is* the deliverable.

### 8.1 What is measured

**Latency** is wall-clock time from just before `submit` returns control to the
engine, to just after it returns, for one command. Reported as a distribution:
p50, p90, p99, p99.9, p99.99, max, plus sample count and the resolution floor.
Never as a mean, and never without the count, because a p99.9 computed from 1000
samples is one sample.

**Throughput** is commands per second sustained over a long run at saturation, which
is a different question with a different answer and is measured separately.

**Instruction count per operation** is measured with Cachegrind and is the CI gate.

### 8.2 The six scenarios

Workload shape changes results by more than most optimizations do, so results are
always reported per scenario, never as one number.

| Scenario | Composition | What it isolates |
|---|---|---|
| `rest_only` | 100% non-crossing limits | Insert path and ladder/bitmap writes |
| `cross_shallow` | Every order crosses exactly one resting order | The common real case |
| `cross_deep` | Orders sweep 10 to 50 levels | Level traversal and bitmap advance |
| `cancel_heavy` | 90% cancels, 10% new | The realistic ratio; isolates `IdIndex` and unlink |
| `mixed_realistic` | Power-law depth, ~10:1 cancel:trade, all five order types | The headline number |
| `worst_case_sweep` | One order consuming the entire populated book | Tail behavior and the true worst case |

Each is generated from a seeded `xoshiro256**` so a scenario is reproducible from an
integer, and the seed is printed with every result.

### 8.3 Known measurement errors and how each is handled

| Error | Handling |
|---|---|
| **Clock resolution ~41.67 ns** (verified) | Per-op cost measured in batches, which is quantization-free. Distributions carry an explicit resolution-floor note. Distribution shape cross-checked on x86 CI where `rdtsc` resolves sub-ns. |
| **Clock call overhead** | `bench/clock.hpp` measures its own overhead at startup with a dependency-chained loop (not a throughput loop, which under-reports), prints it, and subtracts it. `CNTVCT_EL0` is used because it is ~36x cheaper than `clock_gettime_nsec_np`. |
| **Compiler eliding the work** | Explicit `do_not_optimize` / `clobber_memory` barriers around the measured region. A guard test asserts a deliberately dead benchmark body does *not* report ~0 ns, which catches the barriers silently breaking. |
| **Coordinated omission** | The throughput driver is **open-loop**: commands are issued on a fixed schedule computed in advance, and latency is measured from *intended* issue time, so queueing delay is counted instead of being absorbed by the driver waiting. |
| **Cold caches and cold branch predictor** | Fixed warm-up of 100k operations, discarded. The count is published. |
| **First-touch page faults in the arena** | The pool, ladder and index are fully pre-faulted at construction before any measurement begins. |
| **P-core vs E-core migration** (Apple-specific) | Benchmark thread requests `QOS_CLASS_USER_INTERACTIVE`. macOS has no hard affinity API, so the limitation is documented rather than hidden, and runs showing bimodal distributions are re-run and flagged. |
| **Thermal throttling / noisy machine** | Each result is median-of-5 independent process runs, with inter-run spread published. A spread over 10% invalidates the run. |
| **Discarding inconvenient outliers** | Not done. The tail is the product. Max is always published. |
| **Input that flatters the engine** | Six scenarios including an explicit worst case, plus the power-law realistic mix. |
| **Comparing against nothing** | `ReferenceEngine` is benchmarked under the identical harness, so every speedup is stated as a ratio against a real baseline rather than in isolation. |

### 8.4 Why the CI gate counts instructions, not time

GitHub-hosted runners are shared, virtualized, and have no accessible PMU. Wall-clock
benchmarks there vary by tens of percent run to run, so a wall-clock regression gate
either flakes constantly or is set so loose it catches nothing. Both outcomes are
worse than no gate, because a flaky gate gets disabled and then nobody notices the
real regression.

Cachegrind counts instructions, and data and instruction cache references, by
simulation. It is deterministic to within a fraction of a percent, runs fine on a
shared runner, and needs no PMU. So:

- **CI gate:** instruction count per operation from Cachegrind, per scenario,
  compared against a committed baseline JSON. A rise over 2% fails the build.
- **CI informational:** wall-clock numbers, recorded and plotted but never gating.
- **README numbers:** measured locally on the M4 Pro with the full protocol from
  8.3, with hardware and compiler recorded.

This is the part of the design most likely to impress a reader who has actually tried
to run benchmarks in CI, because almost everyone gets it wrong the first time.

---

## 9. Alternatives considered

### 9.1 Book data structure

```
Option A: std::map<Ticks, std::list<Order>>
  + trivial, obviously correct, no price-range limit
  + arbitrary price range, no configuration
  - red-black tree: O(log n) with a pointer chase and a likely cache miss per node
  - std::list allocates per node: malloc on the hot path
  - O(1) cancel needs a separate side index anyway
  -> CHOSEN for ReferenceEngine, precisely because it is boring and correct.
     REJECTED for FastEngine.

Option B: flat array of levels indexed by tick + hierarchical occupancy bitmap
  + price lookup is arithmetic plus one load
  + next-occupied-level is 2 to 3 ctz/clz instructions, not a scan
  + levels are contiguous, so sweeping is cache- and prefetch-friendly
  + no allocation at all after construction
  - fixed price range, requires validation (edge cases E12, E13)
  - memory proportional to the tick range, not the order count
  -> CHOSEN for FastEngine.

Option C: intrusive skip list or B-tree keyed by price
  + unbounded price range with better locality than std::map
  - still O(log n), still pointer-chasing
  - materially more complex than B for a worse result in the common case
  -> REJECTED.

Option D: sorted flat vector of occupied levels, binary search
  + compact, only occupied levels stored
  - insert and erase are O(n) memmove in the middle of the book
  - cancel-heavy workloads (the realistic ones) hammer exactly that path
  -> REJECTED.
```

Option B is chosen because the workload is known: prices cluster tightly around the
touch, most activity is at or near the best price, and the cancel-to-trade ratio is
roughly 10:1. That workload rewards contiguity and O(1) best-price access, and
punishes pointer chasing and mid-container inserts.

### 9.2 Handling the fixed price range

Option B's weakness is the bounded ladder, so it deserves its own comparison.

```
Option B1: one fixed ladder covering the whole plausible price range
  e.g. 1,000,000 ticks x 24 B = 24 MB
  + no special cases, any valid price is in range
  - 24 MB blows past the 4 MB L2, so sweeping touches cold memory
  -> REJECTED on cache grounds.

Option B2: fixed ladder of 65,536 ticks, prices outside it rejected
  1.5 MB, fits in L2 with room for the pool's working set
  + fastest, simplest
  - an order far from the touch is rejected rather than accepted
  -> CHOSEN for v1. Honest, testable (E12/E13), and matches how
     band-limited single-symbol engines actually behave.

Option B3: ladder window around a reference price, plus an overflow std::map
  + unbounded range with fast common path
  - two code paths for every operation, including the window sliding while
    orders rest, which is a genuinely hard correctness problem
  -> DEFERRED. Documented as the natural next step, not built now.
```

Choosing B2 and writing down B3 is the point: it shows the limitation was chosen
with a known upgrade path, not overlooked.

### 9.3 Engine dispatch: how tests and benchmarks target two implementations

```
Option A: virtual base class Engine with two subclasses
  + familiar, runtime-swappable via a flag
  - virtual call on the hot path, inside the measured region
  - blocks inlining across the call, which is where most of the win lives
  -> REJECTED. It would corrupt the benchmark.

Option B: C++20 concept, templates instantiated per engine
  + zero dispatch cost, full inlining
  + one test body and one benchmark body, compiled twice
  - longer compiles; template error messages
  -> CHOSEN.
```

### 9.4 Latency measurement harness

```
Option A: Google Benchmark
  + recognizable, handles warm-up and iteration counts
  - reports mean/median/stddev, not p99.9, which is the figure that matters
  - closed-loop by construction, so coordinated omission is unavoidable
  -> REJECTED as the primary harness.

Option B: purpose-built harness, raw samples, exact percentiles
  + exact percentiles from sorted raw samples (10 M x 4 B = 40 MB, trivial)
  + open-loop driver, so queueing is measured
  + the harness itself is reviewable evidence of methodology
  - has to be written and, itself, tested
  -> CHOSEN. The harness gets its own unit tests, including a
     known-distribution test that asserts recovered percentiles.

Option C: HdrHistogram
  + constant memory, designed exactly for this
  - lossy bucketing, and memory is not a constraint at these sample counts
  -> REJECTED; exact beats approximate when exact is affordable.
```

### 9.5 Dependencies, each justified

| Dependency | Used for | Why accepted |
|---|---|---|
| GoogleTest (pinned tag, FetchContent) | Unit and differential tests | Industry standard; test-only, never linked into the engine or benchmarks |
| FTXUI (pinned tag, FetchContent) | TUI rendering | Terminal control, resize handling and flicker-free redraw are fiddly and are *not* the skill being demonstrated. Tool-only, on a different thread from matching. |
| Cachegrind (Valgrind, Linux CI only) | Deterministic instruction counts | CI-only, no code dependency |
| **Nothing else** | | The engine, the benchmark harness and the fuzz target depend on the standard library alone. |

---

## 10. Failure scenarios

The design-review question "what happens when X is down" has no services to answer
for, so it becomes "what happens when a resource runs out or an input is hostile."

| Scenario | Behavior | Verified by |
|---|---|---|
| Order pool exhausted mid-run | `Rejected(EngineCapacity)`. Engine stays usable; a subsequent cancel frees a slot and the next order succeeds. | Test that fills the pool, asserts rejection, cancels one, asserts the next order is accepted |
| ID index reaches its load ceiling | `Rejected(EngineCapacity)`. Never rehashes, so no hidden allocation. | Unit test on `IdIndex` at capacity |
| Price outside the ladder | `Rejected(PriceOutOfRange)` **before** any indexing. | Tests E12, E13, plus ASan proving no out-of-bounds access |
| Hostile or corrupt replay file | Parser rejects with a diagnostic; it is bounds-checked and is its own fuzz target. | `fuzz_replay_parser` |
| `Fok` pre-scan disagrees with the match loop | Would be a silent partial fill, the worst possible bug. Invariant: after a `Cancelled(Unfillable)`, a full book hash equals the pre-operation hash. | Dedicated test plus a differential-test assertion |
| TUI thread stalls or is killed | Engine is unaffected; seqlock writer never waits on a reader. | Test that stops reading and asserts writer throughput is unchanged |
| Terminal resized to something tiny | TUI clamps depth rows and never writes outside bounds. | Manual check plus a render-to-string unit test at 1x1 |
| Benchmark run on a throttled machine | Median-of-5 with published spread; >10% spread invalidates the run. | Harness reports spread and exits non-zero |
| Cachegrind unavailable in CI | Regression job fails loudly rather than skipping silently. A skipped gate is worse than a missing one. | Workflow asserts the tool is present |
| Engine and reference diverge | Differential test fails, shrinker reduces to a minimal reproducer, which is committed as a permanent regression test. | Every divergence becomes a named test |

### 10.1 Rollback

There is no production deployment, so "rollback" means: every optimization lands as
its own commit with its own benchmark entry, so any single one can be reverted
independently, and `OPTIMIZATION-LOG.md` records which commit produced which number.
An optimization that improves p50 while worsening p99.9 is recorded as exactly that,
and the decision to keep or revert it is written down with its reasoning.

---

## 11. Adversarial design review

The questions a reviewer would actually ask, answered now.

**"Your ladder is a flat array. What stops an order at price 2^31 from writing off
the end of it?"**
Validation before indexing, ordered so price-range checking precedes any arithmetic
on the price. E12 and E13 test both bounds, ASan runs in CI, and the fuzzer feeds
arbitrary 32-bit prices specifically to attack this.

**"How do you know the fast engine matches the reference, rather than matching it on
the cases you thought of?"**
The oracle is not hand-written cases, it is >10^7 randomly generated operation
streams compared event-for-event, plus a coverage-guided fuzzer exploring paths I did
not think of. Hand-written tests cover the cases I *did* think of; those are the
cheap half.

**"41.67 ns clock resolution and a 250 ns target. Aren't your numbers noise?"**
For a single operation, largely yes, and the document says so rather than hiding it.
Per-operation cost is therefore reported from batched measurement, which has no
quantization. Distributions carry the resolution floor next to every figure and are
cross-checked on x86 where the clock resolves sub-nanosecond.

**"Your CI benchmark will flake."**
Which is why CI does not gate on time. It gates on Cachegrind instruction counts,
which are deterministic on a shared runner. Wall-clock is recorded as informational.

**"Why is a TUI in a low-latency engine at all?"**
It is on a separate thread reading a seqlock snapshot, so it cannot block the
matching thread, and there is a test that stops the reader and asserts writer
throughput is unchanged. Architecturally it is the market-data path, which a real
venue has too.

**"What happens at 10x the order rate?"**
The engine is a single thread doing serial work, so the queue in front of it grows.
The answer is to shard by symbol, which the design keeps available by holding no
global state. Pretending a single symbol's matching can be parallelized would be
wrong.

**"You have no deadline. What stops this from never shipping?"**
Four independently shippable plans. Plan 1 alone produces a correct, tested,
CI-verified engine. Each plan's end state is a repository someone could read.

**"Why should I believe the before/after optimization numbers?"**
Each is one commit, with the command to reproduce it, on named hardware, with the
raw sample files committed. Including the optimizations that did not work, which are
recorded rather than deleted.

---

## 12. Testing plan

Five layers, weakest to strongest.

**Layer 1: unit tests per module.** `OrderPool` alloc/free/exhaustion,
`IdIndex` insert/find/erase/capacity, `LevelBitmap` set/clear/next/prev at word
boundaries and across the hierarchy, `PriceLadder` FIFO ordering and unlink from
head/middle/tail, `Histogram` percentile recovery from a known distribution.

**Layer 2: the enumerated edge cases.** Every row E1 through E45 in section 5.3 is a
named test, run against **both** engines via the concept. A test named `E36_...`
maps to a documented requirement, so a failure says what broke, not just where.

**Layer 3: invariant checking.** After every operation in debug builds:
best bid strictly below best ask; each level's `total` equals the sum of its orders'
`remaining`; each level's `count` equals its list length; every FIFO list is acyclic
with consistent `prev`/`next`; every live ID resolves to a slot holding that ID;
free-list and live-set are disjoint and together cover the pool; the bitmap's set
bits are exactly the non-empty levels; best-price cursors agree with the bitmap.

**Layer 4: differential testing against the reference.** Seeded random operation
streams, both engines, event streams compared element by element. On divergence, a
shrinker minimizes the stream (delta-debugging: halve, drop, re-test) and the
minimal reproducer is printed and committed as a permanent test. Target >10^7
operations in CI per run, with the seed derived from the commit so coverage
accumulates across commits rather than repeating.

**Layer 5: coverage-guided fuzzing.** libFuzzer target interpreting the input as an
encoded command stream, running both engines, asserting event equality and all
invariants. Built with ASan and UBSan. Short run in CI, seed corpus committed, any
crash becomes a Layer 2 test.

Plus: golden-file tests for the six scenarios, asserting byte-identical event streams
across compilers and optimization levels (E43, E44); and a build-level test asserting
zero allocations on the hot path by counting through a replaced `operator new`.

### 12.1 Staged verification (the analogue of a rollout)

```
   module unit tests           (seconds)
          ↓
   edge-case suite E1..E45     (seconds)
          ↓
   invariant-checked debug run (seconds)
          ↓
   differential vs reference   (minutes, 10^7 ops)
          ↓
   ASan + UBSan builds         (minutes)
          ↓
   libFuzzer short run         (minutes in CI, longer locally)
          ↓
   Cachegrind regression gate  (deterministic, gating)
          ↓
   wall-clock benchmarks       (informational in CI, authoritative locally)
          ↓
   TUI smoke test on a replay  (manual, once per release)
```

A change that fails at any stage does not proceed to the next. Optimizations in
particular must clear the differential test before their benchmark number is
believed, because a fast wrong engine is easy to build by accident.

---

## 13. Monitoring and observability

There is no production system to monitor, so this becomes: what does the repository
publish, and what fails loudly?

- `docs/BENCHMARKS.md`: per-scenario distributions, hardware, compiler, seeds,
  reproduction commands. Regenerated by one script, never hand-edited.
- `docs/OPTIMIZATION-LOG.md`: one entry per attempt, including failures, each with
  hypothesis, profile evidence, change, before/after, and any regression.
- `docs/METHODOLOGY.md`: section 8 in reader-facing form, including the limitations
  that could not be engineered away.
- `bench/baselines/*.json`: committed Cachegrind instruction counts. The gate.
- CI artifacts: flamegraph SVG from `perf record` on Linux, raw latency samples,
  Cachegrind output.
- Engine-internal counters (rejections by reason, fills, sweep depth histogram)
  compiled in under a flag, off in the measured build, used to confirm a scenario
  generated the workload it claimed to.

That last one closes a real hole: a scenario that silently degenerates into
"everything rests, nothing crosses" would produce excellent and meaningless numbers.
The counters prove the workload was what it said it was.

---

## 14. The six questions, answered directly

1. **What problem am I solving?** Implement exchange matching correctly, make it
   measurably fast, and prove both with reproducible evidence. The proof is the
   deliverable.
2. **What exactly should the system do?** Accept New (Limit, Market, IOC, FOK,
   PostOnly) and Cancel commands for one symbol; match under price-time priority at
   the maker's price; emit a deterministic, gap-free, sequenced event stream; reject
   invalid input atomically with a typed reason; publish top-N depth without ever
   blocking the matching thread.
3. **What are the main components?** `PriceLadder` + `LevelBitmap` (where orders
   live and how the best price is found), `OrderPool` + `IdIndex` (how orders are
   stored and found by ID), `FastEngine` (the matching logic), `ReferenceEngine`
   (the correctness oracle), the benchmark harness, and the seqlock L2 publisher
   feeding the TUI.
4. **How does data move?** Command in, validate, match against the opposite side
   best-price-first while price permits, rest any remainder, emit events into a
   caller-owned buffer, publish an L2 snapshot through a seqlock that a separate TUI
   thread reads without ever blocking the writer.
5. **What can fail?** Pool exhaustion, index capacity, out-of-range price,
   duplicate or unknown order ID, a post-only that would cross, an unfillable FOK,
   a hostile replay file, and a stalled TUI reader. Every one has a defined typed
   outcome, leaves state unchanged, and has a test in section 10.
6. **How will I know it works?** Five test layers ending in >10^7 differential
   operations against an independently written reference plus coverage-guided
   fuzzing; invariants asserted after every operation in debug builds; determinism
   proven across compilers and optimization levels; and a Cachegrind instruction
   gate so it cannot silently get slower.

---

## 15. Open questions

| # | Question | Default if unanswered |
|---|---|---|
| O1 | Is the optional Plan 4 (binary wire protocol + busy-poll ingest) in scope? | Not built. Plans 1 to 3 stand alone. |
| O2 | Install full Xcode for Instruments, or accept `sample` plus Linux CI `perf`? | Accept the limitation; `sample` locally, `perf` in CI. Revisit if profiling proves too coarse. |
| O3 | Is the repository public on GitHub? CI and the resume value both assume yes. | Assume public. Nothing is pushed without explicit approval. |
| O4 | Add in-place size *reduction* preserving time priority, as real venues do? | Not in v1. Cancel/replace loses priority, documented. |
| O5 | Latency target of p50 < 250 ns: keep, or set it after a first measurement? | Keep as a target; publish the real number regardless and move the target with a recorded reason. |

---

## 16. Scope assumption carried into the plans

Brainstorming was interrupted before the layer-scope question was settled. The
selection on record was the TUI, without the wire protocol or the L2 feed. The L2
feed is a hard prerequisite for the TUI, since the TUI has to read book state from
somewhere without touching the engine, so it is included. The wire protocol is
isolated into **Plan 4, explicitly optional**, and nothing in Plans 1 to 3 depends
on it.

If that reading is wrong, Plan 4 is the only thing that moves.
