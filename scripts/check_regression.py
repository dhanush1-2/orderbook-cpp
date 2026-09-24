#!/usr/bin/env python3
"""Compare Cachegrind instruction counts against a committed baseline.

Gates on INSTRUCTION COUNT, not wall-clock time. Measured justification: two
Cachegrind runs of one identical binary gave 1,842,733 and 1,842,734 I refs, or
about 1 part in 2,000,000. The 2% threshold therefore has ~40,000x margin and
cannot flake. Wall-clock on a shared CI runner varies by tens of percent, so a
wall-clock gate either flakes constantly or is set so loose it catches nothing --
and a flaky gate gets disabled, after which nobody notices the real regression.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

THRESHOLD = 0.02  # 2% rise fails the build

SCENARIOS = ["rest_only", "cross_shallow", "cross_deep",
             "cancel_heavy", "mixed_realistic", "worst_case_sweep"]


def measure(binary: str, scenario: str, ops: int, capacity: int) -> int:
    out = subprocess.run(
        ["valgrind", "--tool=cachegrind", "--cache-sim=no",
         f"--cachegrind-out-file=/tmp/cg.{scenario}",
         binary, "--scenario", scenario, "--ops", str(ops),
         "--capacity", str(capacity)],
        capture_output=True, text=True, check=True,
    ).stderr
    m = re.search(r"I\s+refs:\s+([\d,]+)", out)
    if not m:
        sys.exit(f"could not parse I refs for {scenario} from:\n{out}")
    return int(m.group(1).replace(",", ""))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--ops", type=int, default=200_000)
    ap.add_argument("--capacity", type=int, default=1 << 18,
                    help="engine capacity. Sized to the workload so construction, "
                         "which value-initializes the pool/ladders/index, does not "
                         "dominate the instruction count")
    ap.add_argument("--update", action="store_true",
                    help="overwrite the baseline with the measured values")
    args = ap.parse_args()

    measured = {s: measure(args.binary, s, args.ops, args.capacity)
                for s in SCENARIOS}
    total_ops = args.ops
    for s, n in measured.items():
        print(f"{s:<18} {n:>14,} instructions  ({n / total_ops:>9.1f} per op)")

    path = pathlib.Path(args.baseline)
    if args.update or not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"ops": args.ops, "capacity": args.capacity,
                                    "instructions": measured},
                                   indent=2) + "\n")
        print(f"\nbaseline written to {path}")
        return 0

    baseline = json.loads(path.read_text())
    if baseline["ops"] != args.ops or baseline.get("capacity") != args.capacity:
        sys.exit(f"baseline was taken at ops={baseline['ops']} "
                 f"capacity={baseline.get('capacity')}, this run used ops={args.ops} "
                 f"capacity={args.capacity}; counts are not comparable")

    failed, missing = [], []
    print()
    for s in SCENARIOS:
        if s not in baseline["instructions"]:
            missing.append(s)
            continue
        old, new = baseline["instructions"][s], measured[s]
        delta = (new - old) / old
        mark = "FAIL" if delta > THRESHOLD else "ok"
        print(f"{s:<18} {old:>14,} -> {new:>14,}  {delta:+7.3%}  {mark}")
        if delta > THRESHOLD:
            failed.append(s)

    if missing:
        print(f"\nnew scenarios not in the baseline: {missing}. "
              f"Re-run with --update to record them.")
    if failed:
        print(f"\nREGRESSION: {len(failed)} scenario(s) exceeded "
              f"{THRESHOLD:.0%} more instructions per operation.")
        print("If this rise is intentional, re-run with --update and say why in "
              "the commit message and in docs/OPTIMIZATION-LOG.md.")
        return 1

    print(f"\nAll scenarios within {THRESHOLD:.0%}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
