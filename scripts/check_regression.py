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
import platform
import re
import subprocess
import sys

THRESHOLD = 0.02  # 2% rise fails the build

# Instruction counts are ARCHITECTURE-SPECIFIC. The same source compiled for arm64
# and x86-64 executes a different number of instructions, so one flat baseline
# cannot serve both. An earlier version stored a single set recorded on arm64 and
# compared it against x86-64 CI, which could never have passed. The baseline is
# therefore keyed by machine architecture, and a run against an architecture with no
# recorded baseline says so instead of failing as a phantom regression.

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

    arch = platform.machine()
    print(f"architecture: {arch}\n")
    measured = {s: measure(args.binary, s, args.ops, args.capacity)
                for s in SCENARIOS}
    total_ops = args.ops
    for s, n in measured.items():
        print(f"{s:<18} {n:>14,} instructions  ({n / total_ops:>9.1f} per op)")

    path = pathlib.Path(args.baseline)
    if args.update or not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        existing = json.loads(path.read_text()) if path.exists() else {}
        arches = existing.get("arch", {})
        arches[arch] = {"ops": args.ops, "capacity": args.capacity,
                        "instructions": measured}
        path.write_text(json.dumps({"arch": arches}, indent=2) + "\n")
        print(f"\nbaseline for {arch} written to {path}")
        return 0

    doc = json.loads(path.read_text())
    if "arch" not in doc or arch not in doc["arch"]:
        print(f"\nNo baseline recorded for architecture '{arch}'. Instruction counts "
              f"are architecture-specific, so this is not a regression - it is a "
              f"missing measurement. Record it with --update on this machine.")
        return 0
    baseline = doc["arch"][arch]
    if baseline["ops"] != args.ops or baseline.get("capacity") != args.capacity:
        sys.exit(f"baseline for {arch} was taken at ops={baseline['ops']} "
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
