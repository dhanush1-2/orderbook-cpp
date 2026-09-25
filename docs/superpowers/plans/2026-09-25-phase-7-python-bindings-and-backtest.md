# Phase 7: Python Bindings and an Honest Backtest — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the C++ replay to Python through pybind11, and use it to evaluate an order-book-imbalance signal with realistic fills and fees — reporting what it actually does, including that it loses money net of costs.

**Architecture:** A thin pybind11 module wraps `Replay` and `SymbolBook` and emits sampled book state as NumPy arrays. The research layer is ordinary Python: pandas for the panel, a vectorised backtest with explicit cost assumptions, and a report of Sharpe, drawdown and turnover alongside the assumptions that produced them.

**Tech Stack:** C++20, pybind11 3.1.0, Python 3.13, NumPy 2.2, pandas 2.3, pytest 9.1, SciPy 1.16.

**Spec:** [`../specs/2026-09-25-itch-replay-and-research-agent-design.md`](../specs/2026-09-25-itch-replay-and-research-agent-design.md)

**Depends on:** Phases 5 and 6.

## The finding this phase is built around

**The signal was measured before this plan was written.** Top-of-book imbalance,
`(bid_qty - ask_qty) / (bid_qty + ask_qty)`, sampled once a second against forward mid
returns, over 30.7 M real messages:

| Symbol | IC @1s | t-stat | IC @5s | IC @10s | IC @30s | mean abs 1s move | spread |
|---|---|---|---|---|---|---|---|
| INTC | **0.256** | 9.0 | 0.119 | 0.084 | −0.037 | 0.69 bps | 5.49 bps |
| QQQ | **0.123** | 8.8 | 0.079 | 0.048 | 0.021 | 0.24 bps | 1.05 bps |
| SPY | 0.075 | 3.7 | 0.057 | 0.025 | 0.054 | 0.23 bps | 1.28 bps |
| MSFT | 0.001 | 0.0 | 0.070 | 0.066 | 0.081 | 0.80 bps | 9.57 bps |
| AAPL | 0.016 | 0.7 | −0.019 | 0.020 | 0.012 | 0.83 bps | 6.41 bps |

Two things follow, and **the whole phase is designed around them**:

1. **The signal is statistically real on some symbols and absent on others.** INTC and
   QQQ clear t = 8.8 at one second. AAPL and MSFT do not clear t = 1. A backtest that
   pools all five and reports one number would hide this.
2. **It is not tradeable by crossing the spread, and the arithmetic is not close.**
   QQQ's entire average one-second move is **0.24 bps** while half its spread is
   **0.52 bps**. Even a *perfect* forecast pays more to enter than the move is worth.
   For AAPL it is 0.83 bps of move against 3.2 bps of half-spread.

> **So the deliverable is not a profitable strategy, and the plan does not pretend to
> look for one.** It is a correct evaluation that reaches a negative conclusion with
> the evidence attached. The spec asks to be "honest about shortcomings"; the honest
> version of this result is that a real, strongly significant predictive signal is
> economically dead once you pay the spread, and saying so precisely — with the IC, the
> t-stat, the horizon decay, and the cost arithmetic — is the actual finding.

**A backtest that reports a positive Sharpe here is a bug, and the plan treats it as
one.** Task 5 contains a test that fails if the net result comes out positive under
spread-crossing costs, because that outcome would mean lookahead has leaked in.

## Global Constraints

- **The C++ side stays the source of truth.** Python never reimplements book logic.
- **No lookahead, ever.** A feature at time `t` uses only data with timestamp `<= t`.
  The forward return uses `t + h`. Off-by-one here manufactures the entire result.
- **Costs are explicit and named**, never a single fudge factor.
- **Every reported number carries its sample size and a t-stat or confidence interval.**
  An IC of 0.05 on 300 points is noise; the report must make that visible.
- **Fills are modelled, not assumed.** A passive fill assumption needs a queue-position
  model, and its assumptions get stated rather than buried.
- **Pin `pybind11==3.1.0`** in `requirements.txt`; it is not currently installed.

---

## Task 1: The pybind11 module skeleton

**Files:** Create `python/ob_replay/__init__.py`, `python/src/bindings.cpp`, `python/CMakeLists.txt`, `requirements.txt`, `tests/python/test_bindings.py`. Modify the top-level `CMakeLists.txt`.

**Interfaces:** Produces the importable `ob_replay` module exposing `Replay(symbols: list[str], order_capacity: int = 1<<20)`, `Replay.feed_file(path: str, limit_messages: int = 0) -> None`, `Replay.stats() -> dict`, `Replay.book(symbol: str) -> Book`, `Book.best_bid() -> float`, `Book.best_ask() -> float`, `Book.counters() -> dict`, and `ob_replay.__version__`.

Prices cross the boundary as **integers in 1/10000 units**, not floats, and are
converted at the presentation layer only. A `float` round trip on a price is how
`$55.36` becomes `55.359999999999999` and two orders stop landing on the same level.

- [ ] **Step 1: Pin the dependency**

```
# requirements.txt
pybind11==3.1.0
numpy>=2.2
pandas>=2.3
pytest>=9.1
scipy>=1.16
```

```bash
python3 -m pip install -r requirements.txt
python3 -c "import pybind11; print(pybind11.get_cmake_dir())"
```

- [ ] **Step 2: Write the failing test**

```python
# tests/python/test_bindings.py
import ob_replay


def test_module_imports_and_reports_a_version():
    assert isinstance(ob_replay.__version__, str)


def test_unknown_symbol_raises_rather_than_returning_an_empty_book():
    r = ob_replay.Replay(["AAPL"])
    try:
        r.book("NOTREAL")
    except KeyError:
        return
    raise AssertionError("an empty book would be silently backtested as real data")


def test_replays_the_committed_slice():
    r = ob_replay.Replay(["AAPL", "MSFT", "SPY", "INTC", "QQQ"])
    r.feed_file("testdata/itch_slice_10mb.bin")
    s = r.stats()
    assert s["messages"] == 354_869
    assert s["decode_errors"] == 0


def test_prices_cross_the_boundary_as_integers():
    r = ob_replay.Replay(["AAPL"])
    r.feed_file("testdata/itch_slice_10mb.bin")
    b = r.book("AAPL")
    assert isinstance(b.best_bid_raw(), int), "a float price loses cent alignment"
```

- [ ] **Step 3: Run it, verify it fails** with `ModuleNotFoundError: No module named 'ob_replay'`.
- [ ] **Step 4: Write `python/src/bindings.cpp` and `python/CMakeLists.txt`.**
- [ ] **Step 5: Build and run.** Expected: PASS, 4 tests.
- [ ] **Step 6: Commit.**

---

## Task 2: Sampling the book into NumPy

**Files:** Modify `python/src/bindings.cpp`. Create `tests/python/test_sampling.py`.

**Interfaces:** Produces `Replay.sample(symbols: list[str], interval_ns: int, depth: int = 1) -> dict[str, numpy.ndarray]` returning, per symbol, a structured array with fields `ts` (int64 ns), `bid_px`/`ask_px` (int64, 1/10000), `bid_qty`/`ask_qty` (int64), and, when `depth > 1`, `bid_px_n`/`bid_qty_n`/`ask_px_n`/`ask_qty_n` 2-D arrays.

**Sampling happens in C++, during the replay, not afterwards in Python.** A day is
~300 M messages; crossing the boundary per message would dominate everything. Sampling
at one-second intervals produces ~23,400 rows per symbol, which is nothing.

**The sample must be taken on a wall-clock grid, not per message.** Sampling "every N
messages" makes the sample rate a function of activity, which correlates with
volatility, which biases every statistic computed from it.

- [ ] **Step 1: Write the failing test**, asserting: timestamps are strictly increasing; the grid interval is respected; no row has `bid_px >= ask_px` unless the book was genuinely crossed; row count matches the elapsed time divided by the interval to within one; and **a sample is never taken before both sides exist**.
- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement the sampler.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit.**

---

## Task 3: The feature panel

**Files:** Create `python/ob_replay/features.py`, `tests/python/test_features.py`.

**Interfaces:** Produces `build_panel(samples: dict[str, numpy.ndarray], horizons: list[int]) -> pandas.DataFrame` with columns `ts`, `symbol`, `mid`, `spread_bps`, `obi`, `obi_depth`, and `fwd_ret_{h}s` for each horizon.

**This is where lookahead gets introduced if it is going to be.** Three specific traps:

1. `fwd_ret_h` must be `mid.shift(-h) / mid - 1`, and **the `shift` must be per symbol**.
   A `shift` over a concatenated frame reads the next symbol's prices at the boundary.
2. **A gap in the sample grid must break the forward return, not span it.** If the book
   was one-sided for 40 seconds, `shift(-1)` crosses that gap and the "1-second" return
   is really 41 seconds. Check the timestamp difference and emit `NaN` when it is wrong.
3. **The last `h` rows of every symbol have no forward return.** They must be `NaN` and
   dropped, not filled with zero, which would silently add a block of "flat" outcomes.

- [ ] **Step 1: Write the failing test.** Include, specifically:

```python
def test_forward_return_never_crosses_a_symbol_boundary():
    # Two symbols concatenated, with wildly different price levels. A global shift
    # would compute a ~100% "return" at the boundary row.
    ...
    assert panel.groupby("symbol")["fwd_ret_1s"].apply(lambda s: s.abs().max()).max() < 0.05


def test_forward_return_is_nan_across_a_sampling_gap():
    # A 40-second hole in the grid must NOT become a 1-second return.
    ...
    assert math.isnan(panel.loc[gap_row, "fwd_ret_1s"])


def test_last_rows_are_dropped_not_zero_filled():
    assert not (panel.groupby("symbol").tail(1)["fwd_ret_1s"] == 0).any()
```

- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement `build_panel`.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit.**

---

## Task 4: Reproduce the measured ICs

**Files:** Create `python/ob_replay/evaluate.py`, `tests/python/test_evaluate.py`.

**Interfaces:** Produces `information_coefficient(panel, feature, target) -> pandas.DataFrame` with columns `symbol`, `n`, `ic`, `t_stat`, `p_value`.

**This task exists to catch pipeline bugs before any money is simulated.** The ICs
above were measured independently, so the pipeline must reproduce them. If it does not,
the pipeline is wrong — and finding that out here is far cheaper than finding it out
from a backtest that looks plausible.

- [ ] **Step 1: Write the failing test**

```python
# Measured independently over 30.7M real messages, 1-second sampling.
EXPECTED_IC_1S = {"INTC": 0.256, "QQQ": 0.123, "SPY": 0.075,
                  "MSFT": 0.001, "AAPL": 0.016}


def test_reproduces_the_independently_measured_information_coefficients(panel):
    ic = information_coefficient(panel, "obi", "fwd_ret_1s").set_index("symbol")
    for sym, expected in EXPECTED_IC_1S.items():
        # Generous: the reference used a slightly different window and sampling phase.
        # The point is to catch a broken pipeline, not to pin a third decimal place.
        assert abs(ic.loc[sym, "ic"] - expected) < 0.06, (
            f"{sym}: got {ic.loc[sym,'ic']:.3f}, reference {expected:.3f}. "
            "A large miss means the pipeline is wrong, not that the market changed."
        )


def test_the_significant_symbols_are_significant_and_the_others_are_not(panel):
    ic = information_coefficient(panel, "obi", "fwd_ret_1s").set_index("symbol")
    assert ic.loc["INTC", "t_stat"] > 4
    assert ic.loc["QQQ", "t_stat"] > 4
    assert abs(ic.loc["AAPL", "t_stat"]) < 2, "AAPL had no 1-second signal"


def test_signal_decays_with_horizon(panel):
    # INTC: 0.256 -> 0.119 -> 0.084 -> -0.037 across 1/5/10/30s.
    ics = [information_coefficient(panel, "obi", f"fwd_ret_{h}s")
           .set_index("symbol").loc["INTC", "ic"] for h in (1, 5, 10, 30)]
    assert ics[0] > ics[1] > ics[2], f"expected monotone decay, got {ics}"


def test_a_shuffled_feature_has_no_information(panel):
    # The null. If this does NOT come out near zero, something leaks.
    shuffled = panel.assign(obi=panel.groupby("symbol")["obi"]
                            .transform(lambda s: s.sample(frac=1, random_state=0).values))
    ic = information_coefficient(shuffled, "obi", "fwd_ret_1s")
    assert ic["ic"].abs().max() < 0.05
```

- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement `information_coefficient`.**
- [ ] **Step 4: Run.** Expected: PASS. **If the measured ICs do not reproduce, stop and
      fix the pipeline.** Do not proceed to the backtest with a pipeline that disagrees
      with an independent measurement.
- [ ] **Step 5: Commit.**

---

## Task 5: The backtest, and the negative result

**Files:** Create `python/ob_replay/backtest.py`, `tests/python/test_backtest.py`.

**Interfaces:** Produces `CostModel` (a dataclass: `half_spread_bps`, `fee_bps`, `rebate_bps`, `impact_bps_per_pct_adv`), `backtest(panel, signal, horizon, costs, execution) -> BacktestResult`, and `BacktestResult` with `sharpe`, `max_drawdown`, `turnover`, `gross_bps`, `net_bps`, `n_trades`, `hit_rate`, `equity_curve`.

Two execution models, because the whole conclusion hinges on which one is assumed:

| Model | Assumption | Honesty cost |
|---|---|---|
| `"cross"` | Take liquidity; pay half the spread plus fees | None. Fully determined by observed data. |
| `"passive"` | Post and get filled at the touch, earning the rebate | **Large.** Requires a fill-probability model, and adverse selection means the fills you get are the ones you least want. |

**Default to `"cross"`, and report `"passive"` only with its assumptions printed next to
the number.** The passive result will look much better and is much less trustworthy;
presenting it without that caveat is the single easiest way to mislead here.

- [ ] **Step 1: Write the failing test**

```python
def test_zero_signal_earns_nothing_and_costs_nothing(panel):
    r = backtest(panel.assign(obi=0.0), "obi", 1, CostModel(), "cross")
    assert r.n_trades == 0
    assert r.net_bps == 0.0


def test_costs_are_actually_subtracted(panel):
    free = backtest(panel, "obi", 1, CostModel(half_spread_bps=0, fee_bps=0), "cross")
    paid = backtest(panel, "obi", 1, CostModel(half_spread_bps=0.5, fee_bps=0.3), "cross")
    assert paid.net_bps < free.net_bps
    assert paid.gross_bps == free.gross_bps, "costs must not change the gross number"


def test_turnover_is_reported_and_nonzero_for_a_live_signal(panel):
    r = backtest(panel, "obi", 1, CostModel(), "cross")
    assert r.turnover > 0


# THE HEADLINE ASSERTION. Measured: QQQ's whole 1-second move averages 0.24 bps while
# half its spread is 0.52 bps. Crossing the spread cannot be profitable here, and a
# positive net result means lookahead has leaked in.
def test_crossing_the_spread_loses_money_as_the_arithmetic_requires(panel):
    r = backtest(panel, "obi", 1, CostModel.realistic(), "cross")
    assert r.net_bps < 0, (
        f"net {r.net_bps:.3f} bps is positive, which contradicts the measured "
        "cost arithmetic. Suspect lookahead before celebrating."
    )
    assert r.gross_bps > 0, "the signal is real, so gross should be positive"


def test_a_shuffled_signal_is_not_profitable_even_gross(panel):
    shuffled = panel.assign(obi=panel.groupby("symbol")["obi"]
                            .transform(lambda s: s.sample(frac=1, random_state=1).values))
    r = backtest(shuffled, "obi", 1, CostModel(half_spread_bps=0, fee_bps=0), "cross")
    assert abs(r.sharpe) < 1.0


def test_equity_curve_length_matches_the_panel(panel):
    r = backtest(panel, "obi", 1, CostModel(), "cross")
    assert len(r.equity_curve) == len(panel.dropna(subset=["fwd_ret_1s"]))
```

- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement the backtest.** Gross and net must be computed and reported
      separately, so the cost arithmetic is visible rather than baked in.
- [ ] **Step 4: Run.** Expected: PASS, including the negative-result assertion.
- [ ] **Step 5: Commit.**

---

## Task 6: The report

**Files:** Create `python/ob_replay/report.py`, `notebooks/signal_evaluation.md`, `docs/BACKTEST.md`.

- [ ] **Step 1: Generate the report** over the full day for the five symbols: per-symbol IC with t-stat and n, horizon decay, gross and net bps, Sharpe, max drawdown, turnover, hit rate, and the cost breakdown.
- [ ] **Step 2: Write `docs/BACKTEST.md`** stating the conclusion first: *order-book imbalance predicts one-second returns with t = 8.8 on QQQ and t = 9.0 on INTC, and loses money net of spread-crossing costs, because the average one-second move is smaller than half the spread.* Then the evidence.
- [ ] **Step 3: Write the shortcomings list.** At minimum:
  - **One day of data.** Every number is a single-day estimate with no out-of-sample.
  - **One-second sampling** discards the microstructure where the signal is strongest;
    the IC at finer horizons is probably larger and was not measured.
  - **No queue model.** The passive result is only as good as its fill assumption, and
    adverse selection is not modelled at all.
  - **No impact model** beyond a linear term; size is assumed small enough not to move
    the book, which is exactly the assumption that fails when it matters.
  - **Survivorship and halts** are not handled; a symbol halted mid-day is not excluded.
  - **The five symbols were chosen for liquidity**, which is itself a selection.
- [ ] **Step 4: Commit.**

---

## Self-review

**Spec coverage.** Spec section 4 (Python bindings) is Tasks 1 and 2; the backtest with
realistic fills and fees is Tasks 3–5; Sharpe, drawdown and turnover are Task 5's
`BacktestResult`; spec 6.6's honesty list is Task 6 Step 3. Nothing in spec section 4 is
unaddressed.

**Placeholder scan.** Tasks 3–5 carry the tests that matter in full, because the failure
modes of a backtest are all in the tests rather than the implementation. Tasks 1, 2 and
6 are step lists with concrete acceptance criteria. No step says "add validation".

**Type consistency.** `CostModel`, `BacktestResult`, `build_panel`,
`information_coefficient`, `fwd_ret_{h}s`, and `obi` are spelled the same in every task.

**One risk worth naming.** Task 4 pins the pipeline against ICs measured with a
different implementation, sampling phase and time window, so the tolerance is +/-0.06
rather than tight. That is deliberate: the test is there to catch a broken pipeline,
not to certify a third decimal place. If a future run legitimately drifts outside it —
different day, different symbols — the fix is to re-measure the reference and say so in
the commit, not to widen the tolerance quietly.

---

## Verification of this plan

The signal results this phase is built on were **measured, not assumed**: 30.7 M real
ITCH messages replayed, books reconstructed, top-of-book imbalance sampled once a second
against forward mid returns at 1, 5, 10 and 30 seconds, for five symbols. The table at
the top of this plan is that output.

The toolchain was checked: Python 3.13.7, NumPy 2.2.6, pandas 2.3.3, pytest 9.1.1 and
SciPy 1.16.2 are present; **pybind11 is not installed**, and 3.1.0 resolves and
downloads, which is why Task 1 Step 1 pins it before anything else.

**The measurement determined the shape of the phase.** A plan written without it would
have specified "backtest a signal and report Sharpe", and the executor would have
produced a positive Sharpe by crossing the spread with a lookahead bug and had no way
to know. Instead the cost arithmetic is established up front — QQQ's entire one-second
move is 0.24 bps against 0.52 bps of half-spread — so the negative result is the
*expected* outcome and a positive one is a test failure that points at lookahead.

**Two hazards for the executor.**

1. **`shift(-h)` must be grouped by symbol and gap-checked.** Ungrouped, it reads the
   next symbol's price at each boundary. Un-gap-checked, a 40-second hole in the sample
   grid becomes a "1-second" return. Either one manufactures a result out of nothing.
2. **If the backtest comes out profitable, that is a bug report, not a discovery.**
   The arithmetic says it cannot be, so look for lookahead first. This is the one place
   in the project where a good-looking number is the warning sign.
