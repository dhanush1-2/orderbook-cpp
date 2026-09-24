#!/usr/bin/env bash
# Median-of-5 independent process runs, with the spread published.
#
# Five separate PROCESSES, not five loops inside one: a single process shares a
# warmed cache, a settled frequency state and one allocator arena across all five,
# which is exactly the variance this is supposed to expose.
#
# A spread over 10% INVALIDATES the run. Reporting the median of five wildly
# different numbers as though it were a measurement is how results become fiction.
set -euo pipefail

BIN="${1:?usage: run_bench.sh <binary> [extra args...]}"
shift || true
RUNS="${OB_BENCH_RUNS:-5}"
OUT_DIR="${OB_BENCH_OUT:-bench/results/runs}"
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/run_*.jsonl

echo "running $BIN, $RUNS independent processes" >&2
for i in $(seq 1 "$RUNS"); do
    "$BIN" "$@" > "$OUT_DIR/run_$i.jsonl" 2> "$OUT_DIR/run_$i.clock"
    echo "  run $i done" >&2
done

python3 - "$OUT_DIR" "$RUNS" <<'PY'
import json, statistics, sys, pathlib
out_dir, runs = pathlib.Path(sys.argv[1]), int(sys.argv[2])

by_key = {}
for i in range(1, runs + 1):
    for line in (out_dir / f"run_{i}.jsonl").read_text().splitlines():
        if line.strip():
            rec = json.loads(line)
            by_key.setdefault((rec["scenario"], rec["engine"]), []).append(rec)

failed, summary = False, []
for (scenario, engine), recs in sorted(by_key.items()):
    if "ops_per_sec" in recs[0]:
        metric, vals = "ops_per_sec", [r["ops_per_sec"] for r in recs]
    else:
        metric, vals = "engine_ns_per_op", [r["batched_ns_per_op"] for r in recs]

    med = statistics.median(vals)
    spread = (max(vals) - min(vals)) / med if med else 0.0
    ok = spread <= 0.10
    failed = failed or not ok
    summary.append({"scenario": scenario, "engine": engine, "metric": metric,
                    "median": med, "min": min(vals), "max": max(vals),
                    "spread": round(spread, 4), "runs": len(vals), "valid": ok})
    flag = "" if ok else "  <-- SPREAD OVER 10 PERCENT, RUN INVALID"
    print(f"{scenario:<18} {engine:<10} {metric:<17} median={med:>14,.1f} "
          f"spread={spread:6.2%}{flag}", file=sys.stderr)

(out_dir.parent / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
if failed:
    print("\nAt least one measurement had over 10% spread across runs. Close other "
          "applications, let the machine cool, and re-run. Do not publish this.",
          file=sys.stderr)
    sys.exit(1)
PY
