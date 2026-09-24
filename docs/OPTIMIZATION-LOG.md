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

## Candidates, motivated and not yet measured

Each is motivated by something already established, not by intuition. The procedure
for every one is the rules above.

1. **Identity hashing in `IdIndex`.** The index is 33.5 MB at the default capacity
   and SplitMix64 scatters sequential IDs across all of it, making it the largest
   touched footprint in the engine. Real order IDs arrive sequentially, so identity
   hashing would map them to contiguous buckets. **Must be measured against
   scattered fuzzer IDs too**; if sequential wins big and scattered loses badly, the
   honest outcome is to keep SplitMix64 and record that the tradeoff was measured.
2. **Cached best-price cursor.** `best()` queries the bitmap on every call. Requires
   a debug assert comparing the cache against the bitmap, or it is a stale-cache bug
   waiting to happen.
3. **Prefetch the next order during a sweep.** `match_into` walks an intrusive list
   by index, so the next address is a dependent load. Expect it to help `cross_deep`
   and `worst_case_sweep` and do nothing for `cross_shallow`.
4. **Branch hints on the dominant path.** Measure with Cachegrind `--branch-sim=yes`
   as well as wall-clock: the instruction count may barely move while mispredicts do.
5. **Fuse index erase with pool free.** Only if a profile supports it.
