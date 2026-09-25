# Design: Nasdaq ITCH Replay Engine with an AI Research Agent

**Status:** Approved pending review
**Date:** 2026-09-25
**Author:** Dhanush Chandra Shekar
**Supersedes scope of:** [`2026-09-22-order-book-matching-engine-design.md`](2026-09-22-order-book-matching-engine-design.md) — that project's engine becomes a component here, not the deliverable.

---

## 1. Problem

Build a small version of a trading firm's research stack: replay **real Nasdaq
ITCH 5.0 data**, rebuild the full order book for ~10 symbols, expose it to Python,
backtest a signal on it, and put an LLM research agent on top that can answer
questions like *"why did this signal lose money on Tuesday?"*

Three things separate this from the order books that fill GitHub, and all three are
requirements rather than nice-to-haves:

1. **It runs on real data.** Real ITCH 5.0, not synthetic orders.
2. **It has measured numbers.** Latency and throughput, honestly obtained.
3. **It has an AI layer.** An MCP server the agent drives.

### Restated as a testable objective

> Replay a full day of real Nasdaq ITCH 5.0, reconstruct the book for 10 symbols
> with output verified against the feed's own trade messages, at a measured
> throughput of at least 5 M messages/sec single-threaded, and expose it through
> Python and an MCP server such that an LLM can run a backtest and explain its P&L
> from tool output alone.

---

## 2. What already exists, and what it is worth

Phases 1–4 of the predecessor project produced a verified matching engine. Not all
of it applies, and the difference matters.

**ITCH replay is book RECONSTRUCTION, not matching.** The feed carries
`OrderExecuted` messages: the exchange already decided the fills. The replay engine
applies events; it does not match. That is the single biggest conceptual difference
between the two projects.

| Existing component | Lines | Status here |
|---|---|---|
| `price_ladder.hpp` + `level_bitmap.hpp` | 348 | **Reused.** Reconstruction needs the same O(1) best-price and level operations. Needs a per-symbol price base, see 6.3. |
| `order_pool.hpp` + `id_index.hpp` | 290 | **Reused.** ITCH is keyed by order reference number, which is exactly what `IdIndex` resolves. The duplicate rule changes, see 6.2. |
| `clock.hpp`, `histogram.hpp`, benchmark harnesses, Cachegrind gate | ~800 | **Reused**, retargeted at messages/sec. |
| Test infrastructure, differential oracle, fuzzing, 11 CI jobs | ~4,400 | **Reused.** The reference-oracle pattern applies directly to book reconstruction. |
| `seqlock.hpp`, `l2_snapshot.hpp`, TUI | 371 | **Reused.** Watch a real symbol replay live. |
| `wire.hpp` (custom protocol) | 196 | **Replaced** by real ITCH. The decoding *pattern* transfers: validate length, `memcpy`, typed errors, fuzz target. |
| `fast_engine.hpp` matching logic | ~150 of 414 | **Not on the replay path.** Kept as a second capability and as the differential oracle's counterpart, not deleted. |

---

## 3. Measured facts about the real data

**Every number here was measured on 2026-09-25 against a real file**, not taken from
the spec document. A plan built on a remembered wire format is how this project
fails in week three.

### 3.1 The data source

`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/` — reachable, HTTP 200, directory listing
public. Seven full trading days, each with an `.md5sum` companion:

| File | Compressed |
|---|---|
| `12302019.NASDAQ_ITCH50.gz` | **3.52 GB** (smallest — the default) |
| `07302019.NASDAQ_ITCH50.gz` | 3.66 GB |
| `10302019.NASDAQ_ITCH50.gz` | 3.87 GB |
| `08302019.NASDAQ_ITCH50.gz` | 4.08 GB |
| `01302019.NASDAQ_ITCH50.gz` | 4.76 GB |
| `03272019.NASDAQ_ITCH50.gz` | 5.51 GB |
| `01302020.NASDAQ_ITCH50.gz` | 5.60 GB |

Measured compression ratio on the first 4 MB: **2.42x**. So `12302019` is roughly
**8.5 GB uncompressed** at **28.6 bytes/message** = about **300 million messages**.
Local free disk: 211 GB. There is room for the gz and one decompressed day.

### 3.2 The framing, decoded from real bytes

```
000c  53 0000 0000 0a11ea0e8c43 4f            len=12, 'S' SystemEvent, code 'O'
0027  52 0001 0000 0a53a2887058 "A       " N   len=39, 'R' StockDirectory, symbol A
```

- **2-byte big-endian length prefix** before every message. The length EXCLUDES the
  prefix itself.
- Body: `type` u8, `stock_locate` u16 BE, `tracking_number` u16 BE, `timestamp`
  **6 bytes** BE (nanoseconds since midnight), then per-type fields.
- Everything is big-endian. Everything is packed with no padding.

### 3.3 Message inventory, validated

Walking 354,869 real messages from the start of `12302019`:

| Type | Name | Body bytes | Count in sample |
|---|---|---|---|
| `L` | MarketParticipantPosition | 26 | 215,036 |
| `A` | AddOrder | 36 | 47,970 |
| `D` | OrderDelete | 19 | 43,420 |
| `X` | OrderCancel | 23 | 16,908 |
| `R` | StockDirectory | 39 | 8,906 |
| `H` | StockTradingAction | 25 | 8,897 |
| `Y` | RegSHORestriction | 20 | 8,897 |
| `U` | OrderReplace | 35 | 4,635 |
| `E` | OrderExecuted | 31 | 145 |
| `F` | AddOrderMPID | 40 | 43 |
| `P` | TradeNonCross | 44 | 10 |
| `S` | SystemEvent | 12 | 2 |

**Every size matched the specification exactly. Zero mismatches.** Types not present
in this pre-market prefix but which the decoder must still handle: `C`
OrderExecutedWithPrice (36), `Q` CrossTrade (40), `B` BrokenTrade (19), `I` NOII (50),
`N` RPII (20), `V` MWCBLevel (35), `W` MWCBStatus (12), `K` IPOQuotation (28),
`J` LULDAuctionCollar (35), `h` OperationalHalt (21).

The sample is pre-market (timestamps 11,072 s to 14,772 s after midnight, i.e. 03:04
to 04:06), which is why `L` dominates and `E` is rare. **A realistic message mix
requires decompressing into the trading session**, because gzip is not seekable.

### 3.4 Two facts that break the existing engine

**Order reference numbers are NOT monotonic.** Of 47,970 AddOrder messages,
**12,731 (26.5%) had a reference number at or below the previous one.** Range 42 to
203,109 over 47,970 orders — sparse, not dense.

> The predecessor engine's duplicate-detection rule is `id <= high_water_`, adopted
> precisely because it replaced an unbounded set of retired IDs with one comparison.
> **That rule would reject a quarter of all real orders.** It must go, and the
> retired-ID problem it solved comes back. See 6.2 for the replacement.

**Prices are 4 implied decimals and span an enormous range.** Raw values from 4
(`$0.0004`) to 1,000,000,000 (`$100,000.0000`); median `$55.36`. At raw granularity
that is 10^9 possible levels, against a 65,536-level ladder.

But the per-symbol spread is small. Across 66 symbols with 200+ orders:

| Statistic | Raw ticks | At a 1-cent grid |
|---|---|---|
| Median spread | 13,750 | 138 levels |
| p90 spread | 295,100 | 2,951 levels |
| Max spread | 960,000 | **9,600 levels** |

And the grid is exact: of 47,970 priced orders, **zero were sub-penny at or above
$1**, which is what SEC Rule 612 requires. The 70 sub-penny orders were all below $1,
where the entire legal range is $0.0001–$0.9999 = 9,999 raw ticks.

> **Therefore the existing 65,536-level ladder array is the right size and needs no
> change. What it needs is a per-symbol BASE price and a per-symbol grid: 1 cent when
> the symbol's reference price is at or above $1, raw 1/10000 below it.** See 6.3.

### 3.5 Local toolchain

| Tool | Status |
|---|---|
| Python | 3.13.7 |
| numpy 2.2.6, pandas 2.3.3, pytest 9.1.1 | present |
| **pybind11** | **NOT installed** — Phase 7 installs it |
| CMake 4.4.3, Ninja, clang 21, GCC 16, Docker, valgrind (in Docker) | present |
| Free disk | 211 GB |

---

## 4. Users and success criteria

**Primary user:** a quant or low-latency engineering interviewer who will attack the
measurement methodology before the code, and who has seen a hundred toy order books.

| # | Criterion | Verified by |
|---|---|---|
| S1 | Decoder handles every ITCH 5.0 message type, and never reads out of bounds on hostile input | Type-by-type tests against real bytes, plus a libFuzzer target |
| S2 | Book reconstruction is correct | Differential test against a deliberately simple reference reconstructor, over ≥50 M real messages |
| S3 | Reconstruction agrees with the **feed's own trades** | For every `P`/`E`/`C` message, the executed price/size is consistent with the book state the replay had just built. This is the strongest available oracle: the exchange is the ground truth |
| S4 | Throughput is measured and published | ≥5 M msg/sec single-threaded on `mixed` real data, median of 5 runs, spread published |
| S5 | Per-update latency is published as a distribution | p50/p99/p99.9, with the clock-resolution caveat carried over from the predecessor |
| S6 | Multi-symbol with a lock-free handoff | 10 symbols, SPSC queue parser→book, TSan clean |
| S7 | Python can drive it | `pip install -e .`, snapshot a book, run a backtest, get Sharpe / max drawdown / turnover |
| S8 | The backtest is honest about its limits | A written list of what it does not model, in the README, not buried |
| S9 | An LLM can research with it | MCP server with `replay`, `book_snapshot`, `run_backtest`, `explain_pnl`; a transcript where the agent diagnoses a losing day from tool output alone |
| S10 | One resume line with real numbers | "Replays X M ITCH msgs/sec, p99 Y µs per book update" — both measured by this repo |

---

## 5. Non-goals

| Not building | Why |
|---|---|
| A matching engine for ITCH | The feed already contains the fills. The existing matcher stays as a separate capability. |
| Live network capture (multicast/SoupBinTCP) | The sample files are file-based. The decoder is framing-agnostic so a socket could feed it later. |
| All ~8,900 symbols simultaneously | 10 is the stated target; the design shards per symbol so more is a config change, not a redesign. |
| Nasdaq BX / PSX / other venues | Same decoder family, no new learning. |
| A profitable strategy | The backtest exists to exercise the stack and to be honest about its limits. Claiming alpha from one signal on one day would be the opposite of the point. |
| Training or fine-tuning a model | The AI layer is tool design and grounding, which is the transferable skill. |

---

## 6. Design

### 6.1 Architecture

```
  emi.nasdaq.com                      ┌──────────────── Phase 8: MCP server ───────┐
  12302019.NASDAQ_ITCH50.gz           │  replay() book_snapshot()                  │
        │  (md5-verified)             │  run_backtest() explain_pnl()              │
        ▼                             └───────────────┬────────────────────────────┘
  ┌───────────────┐                                   │ tool calls
  │ Phase 5       │  framing + typed decode           ▼
  │ itch::Decoder │─────────┐              ┌────────────────────┐
  └───────────────┘         │              │ Phase 7: Python    │
        │ ItchMessage       │              │ pybind11 module    │
        ▼                   │              │ + backtester       │
  ┌───────────────┐   SPSC  │              └─────────┬──────────┘
  │ Phase 6       │◄────────┘                        │
  │ SymbolRouter  │  (lock-free, parser → book)      │
  │  └ BookShard × 10                                │
  │     └ PriceLadder + OrderPool + IdIndex ─────────┘
  └───────┬───────┘
          │ L2Snapshot via Seqlock
          ▼
     TUI (reused)
```

### 6.2 Replacing the high-water duplicate rule

Measured: 26.5% of real order references are non-increasing, so `id <= high_water_`
is dead. The retired-ID problem returns: how to reject a reference that was already
deleted, without an unbounded set.

**It does not need solving, and that is the key insight.** The high-water rule existed
for a *matching* engine accepting orders from untrusted clients. A *replay* engine
consumes an authoritative exchange feed:

- A reference number it has not seen is an `AddOrder` — insert it.
- A reference in `IdIndex` is a live order — `Execute`/`Cancel`/`Delete` act on it.
- A reference **not** in `IdIndex` for an Execute/Cancel/Delete means the feed
  referenced an order this replay never added. That is either a mid-file start or a
  decoder bug, and it is **counted and reported**, never silently ignored.

So `IdIndex` alone is the whole mechanism. `kMaxLiveOrders` bounds it, and ITCH
guarantees a reference is retired by `Delete` or full `Execute` before reuse. **The
count of unmatched references is a first-class output**: on a clean full-day replay
starting at the file's beginning it must be zero, and that is a test.

### 6.3 Per-symbol ladder base and grid

Measured: max per-symbol spread is 9,600 cents, and no sub-penny quoting at or above
$1. So:

```cpp
struct SymbolGrid {
    std::int64_t base;       // raw 1/10000 price at ladder index 0
    std::int32_t tick;       // 100 (one cent) at or above $1, else 1 (raw)
};
// index = (raw_price - base) / tick,  valid when 0 <= index < kLadderSize
```

The base is established from the **first priced message** for that symbol, centred so
the ladder spans equally either side. A price outside the window is a
`PriceOutOfWindow` event: counted, reported, and the order is tracked in an overflow
`std::map` rather than dropped. **A silently dropped order corrupts the book, which is
exactly the failure this design must not have**, so the overflow path exists even
though measurement says it will rarely be taken. Its hit count is published.

### 6.4 The strongest available oracle

The predecessor verified a matching engine against a simple reference implementation.
That pattern carries over, but ITCH offers something better:

**The feed contains the exchange's own trades.** Every `E`, `C`, `P` and `Q` message
states a price and size the exchange actually executed. A reconstruction that is
correct must be consistent with them — an `OrderExecuted` must reference an order the
book holds, with at least that many shares remaining, at a price the book agrees with.

That is ground truth from the venue, not from a second implementation I also wrote. It
is criterion S3 and it is the most valuable test in the project.

### 6.5 The AI layer, and what makes it more than a wrapper

An MCP server exposing four tools:

| Tool | Signature | Returns |
|---|---|---|
| `replay` | `(symbol: str, date: str, start: str, end: str)` | message counts by type, book state at end, unmatched-reference count |
| `book_snapshot` | `(symbol: str, date: str, at: str, depth: int)` | top-N bids/asks with sizes and order counts |
| `run_backtest` | `(symbol, date, signal, params)` | Sharpe, max drawdown, turnover, fills, fees, per-trade log |
| `explain_pnl` | `(backtest_id, question)` | grounded P&L attribution: which trades, which book states, which times |

**The design decision that matters:** tools return *structured evidence*, not prose.
`explain_pnl` returns the trade log, the book snapshots around each trade, and the
attribution arithmetic. The model writes the narrative; the server never does. That
keeps the model's answer checkable against tool output, which is the difference
between a research assistant and a plausible-sounding liar.

A tool must also be able to say **"I don't know"**: a query outside the replayed
window returns `OutOfRange` rather than an empty book that reads like a real one.

### 6.6 The backtest, and being honest about it

One signal, deliberately simple: **order-book imbalance**, long when
`bid_qty/(bid_qty+ask_qty)` exceeds a threshold, flat otherwise, evaluated at fixed
intervals.

Fills cross the spread at the touch and pay a fee. What it does **not** model, stated
in the README rather than buried:

- **No market impact.** The order is assumed not to move the book it just read.
- **No queue position.** A passive fill is assumed, not earned.
- **No latency.** The signal sees a book it could not have seen that fast in reality.
- **One day, one symbol at a time.** A Sharpe from one day is not a Sharpe.
- **Survivorship and selection.** Symbols chosen for liquidity, which flatters results.

> Any of these can turn a profitable backtest into a losing strategy. Stating them is
> not modesty; it is the difference between a research tool and a demo.

---

## 7. Failure scenarios

| Scenario | Behavior | Verified by |
|---|---|---|
| Download truncated or corrupted | md5 mismatch, refuse to proceed | Checksum step is mandatory, not optional |
| Message length field is nonsense | Typed `BadLength`, consume nothing, resynchronise | Decoder test + fuzz target |
| Unknown message type | Skippable: the header length lets the reader advance | Decoder test |
| Execute/Cancel references an unknown order | Counted as `unmatched_reference`, reported, never silently dropped | Counter is a published output; zero on a clean full-day replay |
| Price outside the symbol's ladder window | Overflow map, counted, reported | Overflow test with a real wide-spread symbol |
| Book crosses during reconstruction | Reported. Real ITCH books can momentarily cross across venues but not within one book | Invariant checker (reused) |
| Replay asked for a time outside the file | `OutOfRange` from the tool, not an empty book | MCP tool test |
| LLM asks about data that was never replayed | Tool returns an explicit "not replayed" marker | MCP tool test |

---

## 8. Alternatives considered

```
DATA SOURCE
  A: Nasdaq ITCH 5.0 sample files          CHOSEN. Free, real, verified reachable,
                                           md5-checked, 7 days available.
  B: Synthetic generator (current project) REJECTED as the primary source; retained
                                           only for micro-benchmarks where a
                                           controlled workload is the point.
  C: A paid vendor feed                    REJECTED: cost, and no reproducibility for
                                           a reader who wants to run this.

BOOK STORAGE FOR REAL PRICES
  A: Existing flat ladder + per-symbol base  CHOSEN. Measurement says max spread is
                                             9,600 cents against a 65,536 array.
  B: Bigger flat array at raw granularity    REJECTED: 10^9 levels is 24 GB per side.
  C: Hash map keyed by price                 REJECTED: loses the O(1) best-price walk
                                             that the bitmap gives.

PARSER -> BOOK HANDOFF
  A: Single-threaded, parse and apply inline  Baseline; simplest and already fast.
  B: SPSC lock-free ring, parser and book     CHOSEN as the shipped design: the brief
     on separate threads                      names concurrency, and it is measurable
                                              against A. Both are benchmarked so the
                                              claim is a number, not an assertion.
  C: Multi-producer queue                     REJECTED: one parser, so MPSC is
                                              unnecessary machinery.

AI LAYER
  A: MCP server with evidence-returning tools  CHOSEN. Checkable, and MCP is the
                                               named requirement.
  B: A chat wrapper that formats prose         REJECTED: unfalsifiable output.
  C: Fine-tuned model                          REJECTED: not the transferable skill,
                                               and unverifiable in a portfolio.
```

---

## 9. Adversarial review

**"You said the existing engine carries over. Does it?"** The storage does — ladder,
bitmap, pool, index, and the whole test and measurement apparatus. The matching logic
does not, because ITCH carries the exchange's fills. Section 2 states the split by
file and line count rather than implying wholesale reuse.

**"Your book could silently diverge from reality and you would not know."** That is
what S3 exists for. The feed's own `E`/`C`/`P`/`Q` messages are the exchange's ground
truth, and the replay is checked against them continuously, not against a second
implementation by the same author.

**"26.5% non-monotonic order IDs sounds like you misread the spec."** It is measured,
not read: 12,731 of 47,970 real AddOrder messages. It is also why 6.2 removes a rule
the predecessor project depended on.

**"A Sharpe from one day means nothing."** Agreed, and 6.6 says so in the deliverable
itself rather than only here.

**"What stops the LLM from making up an explanation?"** The tools return evidence, not
prose, and every claim in an answer is checkable against the trade log and book
snapshots the tool returned. A tool that cannot answer says so.

**"Throughput on one machine is not throughput."** The predecessor's methodology
carries over whole: median of 5 independent processes, published spread, a stated
clock-resolution floor, and a Cachegrind instruction gate in CI that does not depend
on wall-clock at all.

---

## 10. Testing plan

Layers, weakest to strongest — the predecessor's structure, retargeted:

1. **Unit tests per decoder message type**, against byte arrays captured from the
   real file.
2. **Golden-file decode**: the first 10,000 messages of the real sample decode to a
   committed, human-readable dump. Any decoder change that alters it is visible.
3. **Invariant checking** after every applied message (reused checker).
4. **Differential reconstruction**: fast reconstructor versus a deliberately simple
   `std::map`-based one, over ≥50 M real messages.
5. **Feed-consistency (S3)**: every execution message checked against the book state
   the replay had built.
6. **Fuzzing**: the decoder against arbitrary bytes, seeded from the real corpus.
7. **Python-level tests**: bindings round-trip, backtest determinism.
8. **MCP tool tests**: each tool's contract, including the refusal paths.

---

## 11. Open questions

| # | Question | Default if unanswered |
|---|---|---|
| O1 | Which day? | `12302019` — smallest at 3.52 GB, and a normal trading day. |
| O2 | Which 10 symbols? | Chosen by message count from a first full pass, so they are the liquid ones, and the choice is recorded as a selection bias in 6.6. |
| O3 | Which MCP SDK? | The official Python SDK, since Phase 7 already puts the stack in Python. |
| O4 | Keep the decompressed 8.5 GB on disk? | Yes during development, gitignored. CI uses a committed 10 MB slice. |
| O5 | Does the repo stay `orderbook-cpp`? | Yes. The name still describes it; the README leads with ITCH replay. |
