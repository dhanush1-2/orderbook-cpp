# Optimization log

One entry per attempt, **including the attempts that did not work.** A log that
only contains successes is a marketing document, and the failures are usually the
more interesting half of the conversation.

## Rules

1. **No change lands without a profile pointing at it first.** "This looks slow" is
   not evidence.
2. **No number is believed before the differential test passes.** A fast engine
   that disagrees with the reference is worthless, and it is easy to build by
   accident. Run `OB_DIFF_OPS=10000000` before recording any figure.
3. **Median of 5 independent runs, spread published.** A spread over 10% invalidates
   the measurement; `scripts/run_bench.sh` enforces this.
4. **One optimization per commit**, so any single one can be reverted alone.
5. **A change that improves p50 and worsens p99.9 is recorded as exactly that**, and
   the keep-or-revert decision is written down with its reasoning.

## Entry template

```
### N. <what was changed>

**Hypothesis:** what I expected and why.
**Profile evidence:** the symbol and percentage that motivated this.
**Change:** what the code now does differently, and the commit.
**Result:** before/after per scenario, median of 5, spread stated.
**Regressions:** anything that got worse, including in other scenarios.
**Decision:** kept or reverted, and why.
```

## 0. Baseline

**Change:** none. `FastEngine` as first written: flat ladder, three-level bitmap,
arena with an index free list, open-addressed ID index with SplitMix64 hashing and
backward-shift deletion.

**Result, measured 2026-09-23 on Apple M4 Pro, Apple clang 21, `-O3`:**

| scenario | engine ns/op | throughput ops/s | p99 ns | p99.9 ns | vs reference |
|---|---|---|---|---|---|
| `rest_only` | 32.4 | 59,403,269 | 149 | 232 | 5.0x |
| `cross_shallow` | 25.3 | 46,113,166 | 149 | 191 | 1.8x |
| `cross_deep` | 30.3 | 30,288,068 | 274 | 648 | 3.0x |
| `cancel_heavy` | 23.4 | 53,153,430 | 149 | 191 | 1.9x |
| `mixed_realistic` | 28.9 | 32,971,501 | 149 | 232 | 185x |
| `worst_case_sweep` | 31.8 | 35,226,899 | 149 | 3,440 | 3.3x |

Zero allocations in every measured window, asserted.

**On that 185x.** It is real but it is not the flat ladder winning. `mixed_realistic`
is the only scenario with a meaningful share of FOK orders, and the two engines
differ *asymptotically* there: `ReferenceEngine` sums individual orders in its FOK
pre-scan (O(orders)), `FastEngine` sums level totals (O(levels)). **The honest
headline for the data-structure work is the 1.8x to 5.0x range**, and the 185x is
one algorithmic difference that would be worth applying to any implementation.

## Instrument overhead, recorded because it is larger than what it measures

| scenario | engine ns/op | instrumented ns/op | harness overhead |
|---|---|---|---|
| `rest_only` | 32.4 | 94.0 | 61.5 |
| `cross_shallow` | 25.3 | 61.0 | 35.6 |
| `cross_deep` | 30.3 | 86.2 | 55.8 |
| `cancel_heavy` | 23.4 | 59.3 | 36.0 |
| `mixed_realistic` | 28.9 | 75.5 | 46.5 |
| `worst_case_sweep` | 31.8 | 86.0 | 54.2 |

Two serialized clock reads cost more than the operation between them. This is why
the per-operation figure comes from an untimestamped batched loop and the
distribution is used only for tail shape. An earlier version of the harness
reported the instrumented figure as though it were the engine's cost.


## 1. Identity hashing in `IdIndex` — **REVERTED**

**Hypothesis:** the ID index is the largest touched footprint in the engine, and
SplitMix64 scatters sequential IDs across all of it. Real order IDs arrive
sequentially from a sequencer, so identity hashing should map them to contiguous
buckets and collapse the footprint.

**Profile evidence:** not a sampling profile — a direct experiment on the
hypothesis, which is stronger for a locality question. Holding the workload fixed
and varying only the engine capacity (and therefore the index size):

| capacity | index size | `cancel_heavy` ns/op | vs smallest |
|---|---|---|---|
| 16,384 | 0.5 MB | 7.6 | — |
| 262,144 | 8 MB | 8.6 | 1.1x |
| 1,000,000 | 32 MB | 21.8 | **2.9x** |
| 4,000,000 | 128 MB | 33.0 | **4.3x** |

A 4.3x swing from capacity alone, with identical work. The engine is memory-bound
on this table, and ~14 of the 21.8 ns at default capacity is cache-miss cost.

**Change:** `hash(id) { return id; }`, selectable via `OB_IDINDEX_HASH_IDENTITY`.

**Result:** median of 3 runs, 1.5 M ops per scenario, default 1 M capacity.

| scenario | SplitMix64 | identity | change |
|---|---|---|---|
| `rest_only` | 23.1 ns | 7.5 ns | **-67.3%** |
| `cross_shallow` | 25.0 ns | 11.6 ns | **-53.6%** |
| `cross_deep` | 34.3 ns | 21.7 ns | **-36.7%** |
| `cancel_heavy` | 21.5 ns | 7.0 ns | **-67.3%** |
| `mixed_realistic` | 31.9 ns | 20.6 ns | **-35.5%** |
| `worst_case_sweep` | 32.8 ns | 125.3 ns | **+281.4%** |
| **sum** | 168.6 ns | 193.7 ns | **+14.9%** |

**Regressions:** `worst_case_sweep` is 3.8x slower, which makes the change a net
loss despite five large wins.

**Why, mechanically:** identity hashing maps consecutive IDs to *consecutive
buckets*, so a run of live sequential IDs is a contiguous run of occupied buckets.
Backward-shift deletion over a contiguous run is O(run length). `worst_case_sweep`
holds 400+ consecutive live IDs, so every erase shifts hundreds of entries.
**Identity hashing trades O(1) deletion for O(1) locality.** Correctness was never
in question: 188 tests and 10^7 differential operations passed.

**Decision: REVERTED.** Tail latency is the product in this domain, and a 3.8x
regression on the adversarial scenario is not a trade worth taking for a better
median. Superseded by entry 2, which was only findable by understanding *why* this
one failed.

## 2. Blocked hashing in `IdIndex` — **KEPT**

**Hypothesis:** entry 1 failed because contiguous runs make deletion O(run length).
Keep the locality but *bound* the run: take the low 4 bits of the bucket from the ID
so 16 consecutive IDs land in 16 consecutive buckets, and scramble the block index
so different blocks land far apart. Runs are then bounded at ~16 regardless of how
many orders are live.

**Profile evidence:** the capacity experiment above, plus the mechanism established
by entry 1's failure. This candidate was not in the original list; it came from the
failure.

**Change:**
```cpp
static constexpr unsigned kBlockShift = 4;  // 16 ids x 16 B = 256 B = 2 cache lines
return (splitmix(id >> kBlockShift) << kBlockShift) | (id & ((1u << kBlockShift) - 1));
```

**Result:** same campaign, median of 3.

| scenario | SplitMix64 | blocked | change |
|---|---|---|---|
| `rest_only` | 23.1 ns | 13.3 ns | **-42.3%** |
| `cross_shallow` | 25.0 ns | 15.1 ns | **-39.6%** |
| `cross_deep` | 34.3 ns | 27.3 ns | **-20.4%** |
| `cancel_heavy` | 21.5 ns | 9.9 ns | **-53.8%** |
| `mixed_realistic` | 31.9 ns | 26.4 ns | **-17.3%** |
| `worst_case_sweep` | 32.8 ns | 25.4 ns | **-22.5%** |
| **sum** | 168.6 ns | 117.4 ns | **-30.3%** |

**Regressions: none.** Faster on all six scenarios.

**Verification before the numbers were believed:** 188 tests, 10^7 differential
operations against `ReferenceEngine`, and ASan + UBSan, all clean.

**Decision: KEPT, and made the default.** SplitMix64 and identity remain selectable
via CMake so the A/B stays reproducible.

## 3. The instruction-count gate cannot see this optimization — a methodology limit

Recorded because it is a limitation of this project's own CI gate, and finding it
was more useful than the optimization.

The blocked hash executes **more** instructions per lookup than SplitMix64 — the
same multiply-xor chain plus a shift, an and, and an or — while being **30% faster**,
because it trades ALU work for cache locality. The Cachegrind gate counts
instructions with `--cache-sim=no`. **It would therefore flag entry 2 as a
regression.**

That is not a bug in the gate; it is the price of choosing a deterministic metric.
The gate exists because wall-clock on a shared CI runner varies by tens of percent,
and a flaky gate gets disabled. What it buys is reliable detection of
instruction-count regressions. What it cannot see is any optimization that trades
instructions for memory behaviour — which, on this engine, is the category that
matters most.

**Consequences, all now true of the repo:**

- `bench/baselines/instructions.json` is **stale**: it was recorded with SplitMix64.
  It must be re-recorded with `--update` before the gate is meaningful again.
- The gate's description in `docs/METHODOLOGY.md` now states this blind spot.
- A rise in instruction count accompanied by a wall-clock *improvement* is a valid
  reason to re-baseline, and the commit message must say so.

Re-baselining requires Docker, which was unavailable when this entry was written.
**Marked as outstanding rather than silently skipped.**

## Remaining candidates, motivated and not yet measured

Each is motivated by something already established, not by intuition. The procedure
for every one is the rules above.

1. **Cached best-price cursor.** `best()` queries the bitmap on every call. Requires
   a debug assert comparing the cache against the bitmap, or it is a stale-cache bug
   waiting to happen.
2. **Prefetch the next order during a sweep.** `match_into` walks an intrusive list
   by index, so the next address is a dependent load. Expect it to help `cross_deep`
   and `worst_case_sweep` and do nothing for `cross_shallow`.
3. **Branch hints on the dominant path.** Measure with Cachegrind `--branch-sim=yes`
   as well as wall-clock: the instruction count may barely move while mispredicts do.
4. **Fuse index erase with pool free.** Only if a profile supports it.

5. **Tune `kBlockShift`.** Entry 2 picked 4 (16 ids per block) on the reasoning that
   256 bytes spans two 128-byte cache lines and bounds the deletion run at ~16. It
   was never swept. 3, 5 and 6 are all plausible and the optimum is workload
   dependent.
