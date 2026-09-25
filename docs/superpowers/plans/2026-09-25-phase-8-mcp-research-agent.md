# Phase 8: The MCP Research Agent — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the replay, the book and the backtest as MCP tools so an LLM can run market-data research, with every answer grounded in numbers the tools computed rather than prose the model produced.

**Architecture:** A Python MCP server wrapping the Phase 7 bindings. Four tools — `replay`, `book_snapshot`, `run_backtest`, `explain_pnl` — each returning **structured evidence**: numbers, units, sample sizes and the assumptions used. The server never asks the model to compute anything it could compute itself.

**Tech Stack:** Python 3.13, the `mcp` SDK, the Phase 7 `ob_replay` module, pytest.

**Spec:** [`../specs/2026-09-25-itch-replay-and-research-agent-design.md`](../specs/2026-09-25-itch-replay-and-research-agent-design.md) — section 6.5 governs this phase.

**Depends on:** Phase 7.

## The design rule for this phase

> **Tools return evidence. The model does the narration. The model never does the
> arithmetic.**

An LLM asked "what was the P&L and why" will produce a fluent answer whether or not it
has the numbers, and the fluency is identical either way. So every tool returns the
decomposition already computed — each component with its value, its unit, and its share
of the total — and the model's job is to put it in sentences.

This is not a style preference. It is the difference between a research tool and a
plausible-sounding text generator pointed at a database.

Three consequences, each enforced by a test:

1. **No tool returns a bare number without its units and sample size.** `{"sharpe": 1.8}`
   is unusable; `{"sharpe": 1.8, "n_days": 1, "note": "single-day estimate, no
   out-of-sample"}` is honest.
2. **Every tool that can fail returns a structured error**, never an empty result that
   reads as "nothing happened".
3. **`explain_pnl` returns a decomposition that sums to the total**, and a test asserts
   it sums, because a decomposition whose parts do not add up is worse than none.

## Global Constraints

- **Read-only.** No tool mutates data, downloads files, or writes outside a scratch dir.
- **Bounded work.** Every tool takes an explicit limit and refuses unbounded requests;
  a full-day replay of 300 M messages must not be triggerable by a vague prompt.
- **Deterministic.** The same arguments give the same result. No wall-clock, no RNG
  without a seed in the arguments.
- **Inputs validated at the boundary.** A symbol that is not in the directory is an
  error, not an empty book. A date with no data file is an error naming the file.
- **Every number carries its assumptions.** A backtest result is meaningless without its
  cost model, so the cost model is part of the response, not a server-side default the
  caller cannot see.

---

## Task 1: Server skeleton and tool registration

**Files:** Create `mcp_server/__init__.py`, `mcp_server/server.py`, `mcp_server/errors.py`, `tests/mcp/test_server.py`, `mcp_server/README.md`. Modify `requirements.txt`.

**Interfaces:** Produces `create_server() -> Server`, `ToolError` (with `code`, `message`, `hint`), and the four registered tool names.

- [ ] **Step 1: Write the failing test**

```python
# tests/mcp/test_server.py
import pytest
from mcp_server.server import create_server

EXPECTED = {"replay", "book_snapshot", "run_backtest", "explain_pnl"}


@pytest.mark.asyncio
async def test_exactly_the_four_specified_tools_are_registered():
    tools = {t.name for t in await create_server().list_tools()}
    assert tools == EXPECTED


@pytest.mark.asyncio
async def test_every_tool_documents_its_arguments_and_units():
    for t in await create_server().list_tools():
        assert t.description, f"{t.name} has no description"
        schema = t.inputSchema
        assert schema.get("required") is not None, f"{t.name} declares nothing required"
        for name, prop in schema["properties"].items():
            assert prop.get("description"), f"{t.name}.{name} is undocumented"


@pytest.mark.asyncio
async def test_unknown_symbol_is_a_structured_error_not_an_empty_book():
    r = await call(create_server(), "book_snapshot",
                   {"symbol": "NOTREAL", "date": "2019-12-30", "at_time": "10:00:00"})
    assert r["error"]["code"] == "unknown_symbol"
    assert "NOTREAL" in r["error"]["message"]
    assert r["error"]["hint"], "an error the model cannot act on is a dead end"


@pytest.mark.asyncio
async def test_unbounded_request_is_refused_rather_than_replaying_the_whole_day():
    r = await call(create_server(), "replay",
                   {"symbol": "AAPL", "date": "2019-12-30"})  # no limit
    assert r["error"]["code"] in ("limit_required", "limit_too_large")
```

- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement the skeleton with the four tools stubbed to return `not_implemented`.**
- [ ] **Step 4: Run.** Expected: the registration and error tests pass.
- [ ] **Step 5: Commit.**

---

## Task 2: `replay`

**Interfaces:** `replay(symbol: str, date: str, limit_messages: int, oracle: bool = True) -> dict` returning `symbol`, `date`, `messages_processed`, `elapsed_ms`, `messages_per_second`, `decode_errors`, `counters` (the `BookCounters` fields), `oracle` (`checked`, `mismatches`, `rate`), and `warnings: list[str]`.

**The oracle result is part of the response, not a log line.** If reconstruction
disagreed with the exchange's own executions, every downstream answer is suspect, and
the model must be able to see that rather than be told everything is fine.

- [ ] **Step 1: Write the failing test**, asserting: the committed slice yields 354,869 messages and zero decode errors; `oracle.mismatches == 0`; `messages_per_second > 0`; a nonzero `unknown_ref` produces a `warnings` entry; and `limit_messages` is actually respected.
- [ ] **Step 2: Run, verify it fails. Step 3: Implement. Step 4: Run. Step 5: Commit.**

---

## Task 3: `book_snapshot`

**Interfaces:** `book_snapshot(symbol, date, at_time: str, depth: int = 10) -> dict` returning `symbol`, `timestamp_ns`, `bid`/`ask` as lists of `{price, size, orders}`, `mid`, `spread_bps`, `imbalance`, `total_bid_qty`, `total_ask_qty`, `crossed: bool`, and `levels_in_overflow: int`.

Three things this must get right, each from a measured fact:

- **Prices in the response are decimal strings**, not floats. `"55.36"`, never
  `55.359999999999999`.
- **`crossed` is a reported field, not an error.** Real books cross; Phase 6 reports it
  rather than matching it, and so does this.
- **`levels_in_overflow` is surfaced**, because a book whose only bid is a $0.01 stub is
  a very different thing from one with a real bid, and the model cannot tell from the
  top-of-book number alone.

- [ ] **Step 1: Write the failing test**, asserting: bids descend and asks ascend; `depth` is respected; `mid` sits between them when not crossed; a crossed book sets `crossed` and still returns both sides; prices round-trip as exact decimal strings; and **a time before the first message returns an error rather than an empty book**.
- [ ] **Step 2: Run, verify it fails. Step 3: Implement. Step 4: Run. Step 5: Commit.**

---

## Task 4: `run_backtest`

**Interfaces:** `run_backtest(symbols: list[str], date: str, signal: str, horizon_s: int, cost_model: dict, execution: str = "cross") -> dict` returning `per_symbol` (IC, t-stat, n, gross_bps, net_bps, sharpe, max_drawdown, turnover, hit_rate), `aggregate`, `cost_model` (echoed back in full), `execution`, `assumptions: list[str]`, and `caveats: list[str]`.

- [ ] **Step 1: Write the failing test**

```python
@pytest.mark.asyncio
async def test_the_cost_model_is_echoed_so_the_number_is_interpretable():
    r = await call(srv, "run_backtest", {..., "cost_model": {"half_spread_bps": 0.5}})
    assert r["cost_model"]["half_spread_bps"] == 0.5


@pytest.mark.asyncio
async def test_caveats_are_always_present_and_never_empty():
    r = await call(srv, "run_backtest", {...})
    assert r["caveats"], "a backtest result without caveats invites overreading"
    assert any("single day" in c.lower() or "one day" in c.lower() for c in r["caveats"])


@pytest.mark.asyncio
async def test_passive_execution_carries_its_extra_assumption():
    r = await call(srv, "run_backtest", {..., "execution": "passive"})
    joined = " ".join(r["assumptions"]).lower()
    assert "fill" in joined and "adverse selection" in joined, (
        "the passive number looks better and is less trustworthy; the response must "
        "say why, because the model will not know to ask"
    )


@pytest.mark.asyncio
async def test_crossing_the_spread_reports_a_negative_net_result():
    # Measured: QQQ's mean 1s move is 0.24 bps against 0.52 bps of half-spread.
    r = await call(srv, "run_backtest", {"symbols": ["QQQ"], "horizon_s": 1,
                                         "execution": "cross", ...})
    assert r["per_symbol"]["QQQ"]["net_bps"] < 0
    assert r["per_symbol"]["QQQ"]["gross_bps"] > 0
```

- [ ] **Step 2: Run, verify it fails. Step 3: Implement. Step 4: Run. Step 5: Commit.**

---

## Task 5: `explain_pnl`

**Interfaces:** `explain_pnl(backtest_id: str | None, symbols, date, signal, horizon_s, cost_model, execution) -> dict` returning `total_bps`, `components: list[{name, value_bps, share_of_total, direction}]`, `reconciliation: {sum_of_components, total, residual_bps, tolerance_bps, reconciles: bool}`, `largest_driver`, and `narrative_inputs`.

**This is the tool most at risk of becoming a prose generator, so it is the one with the
strictest contract.** It returns no narrative. It returns a decomposition — gross signal
P&L, half-spread paid, fees, rebates, impact, residual — with each component's value and
share, plus an explicit reconciliation.

**The reconciliation is the test.** A decomposition whose components do not sum to the
total is worse than no decomposition, because it is confidently wrong and nothing
downstream can detect it.

- [ ] **Step 1: Write the failing test**

```python
@pytest.mark.asyncio
async def test_components_sum_to_the_total():
    r = await call(srv, "explain_pnl", {...})
    rec = r["reconciliation"]
    assert rec["reconciles"], (
        f"components sum to {rec['sum_of_components']} but total is {rec['total']}; "
        f"residual {rec['residual_bps']} exceeds tolerance {rec['tolerance_bps']}"
    )
    assert abs(sum(c["value_bps"] for c in r["components"]) - r["total_bps"]) < 1e-6


@pytest.mark.asyncio
async def test_shares_sum_to_one_and_signs_are_explicit():
    r = await call(srv, "explain_pnl", {...})
    assert abs(sum(abs(c["share_of_total"]) for c in r["components"]) - 1.0) < 1e-6
    for c in r["components"]:
        assert c["direction"] in ("gain", "cost")


@pytest.mark.asyncio
async def test_it_returns_no_prose():
    r = await call(srv, "explain_pnl", {...})
    assert "narrative" not in r and "explanation" not in r and "summary" not in r, (
        "this tool supplies evidence; the model writes the sentences"
    )


@pytest.mark.asyncio
async def test_the_largest_driver_under_spread_crossing_is_the_spread():
    # Measured: half-spread exceeds the entire average 1-second move.
    r = await call(srv, "explain_pnl", {"execution": "cross", "horizon_s": 1, ...})
    assert r["largest_driver"] == "half_spread_paid"
```

- [ ] **Step 2: Run, verify it fails. Step 3: Implement. Step 4: Run. Step 5: Commit.**

---

## Task 6: End-to-end, documentation and CI

**Files:** Create `tests/mcp/test_end_to_end.py`, `docs/MCP.md`. Modify `.github/workflows/ci.yml`, `README.md`.

- [ ] **Step 1: Write an end-to-end test** that runs the realistic sequence against the committed slice — `replay` → `book_snapshot` → `run_backtest` → `explain_pnl` — and asserts the numbers agree across tools: the `book_snapshot` mid at time T matches the panel's mid at T, and `explain_pnl`'s total matches `run_backtest`'s `net_bps`. **Cross-tool disagreement is the failure mode that makes an agent confidently wrong**, and nothing else catches it.
- [ ] **Step 2: Write `docs/MCP.md`** with each tool's schema, a worked transcript, and an explicit statement of what the server will not do: it does not produce narrative, does not hide assumptions, and does not return a number without its sample size.
- [ ] **Step 3: Add a CI job** running the MCP tests against the committed slice.
- [ ] **Step 4: Update `README.md` and the phase index. Commit.**

---

## Self-review

**Spec coverage.** Spec section 5 names exactly four tools — `replay`, `book_snapshot`,
`run_backtest`, `explain_pnl` — and Tasks 2–5 implement one each, with Task 1 asserting
the set is exactly those four. Spec 6.5 ("MCP tools return structured evidence, not
prose") is the design rule at the top and is enforced by `test_it_returns_no_prose` and
the reconciliation tests.

**Placeholder scan.** Tasks 1, 4 and 5 carry their tests in full because that is where
the contract lives. Tasks 2, 3 and 6 give the response schema in the Interfaces block
and enumerate the assertions rather than writing them out; each is concrete enough to
implement without a judgement call. No step says "handle errors appropriately".

**Type consistency.** The four tool names, `ToolError`'s `code`/`message`/`hint`, the
`cost_model` keys, `gross_bps`/`net_bps`, and `components[].value_bps`/`share_of_total`
are spelled identically across tasks and match Phase 7's `CostModel` and
`BacktestResult` field names.

**One thing deliberately not built.** There is no tool that asks the model to pick a
signal, tune a parameter, or decide whether a result is good. That is a product
decision, not an omission: the moment a tool returns "is this strategy good?", the
answer is the model's prior rather than the data, and the whole evidence-not-prose rule
collapses.

---

## Verification of this plan

The facts this phase asserts in its tests come from Phases 6 and 7's measurements, not
from expectation:

| Assertion in a test here | Where the number came from |
|---|---|
| 354,869 messages, 0 decode errors | Decoding the committed slice (Phase 5) |
| `oracle.mismatches == 0` | 78,683 / 78,683 executions at the best price (Phase 6) |
| `net_bps < 0` under `"cross"` | QQQ's 0.24 bps mean 1s move vs 0.52 bps half-spread (Phase 7) |
| `gross_bps > 0` | IC 0.123 at t = 8.8 on QQQ (Phase 7) |
| `largest_driver == "half_spread_paid"` | The same cost arithmetic |

**Two hazards for the executor.**

1. **`explain_pnl` is the tool that will drift.** It is the one whose output reads like
   English, and the temptation is to have it return a sentence. The reconciliation test
   is what holds the line: if the components stop summing to the total, the tool has
   started producing description instead of decomposition.
2. **Cross-tool agreement is not automatic.** `book_snapshot` and `run_backtest` reach
   the same data by different paths, and a sampling-phase or rounding difference will
   make them disagree slightly. Task 6 Step 1 is the only test that catches it, and an
   agent quoting two different mids for the same instant is precisely the failure that
   destroys trust in the whole stack.
