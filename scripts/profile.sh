#!/usr/bin/env bash
# Sampled profile via perf, inside Docker.
#
# perf does not exist on macOS and Apple Silicon exposes no userspace PMU, so this
# is the only way to profile this project locally. It uses the SOFTWARE cpu-clock
# event, because the Docker VM has no PMU either: /sys/bus/event_source/devices
# lists only breakpoint, kprobe, software, tracepoint and uprobe, and cache-misses
# returns <not supported>.
#
# Consequence: this tells you WHERE the time goes. It cannot give you cache misses
# or branch mispredictions. Use scripts/cachegrind.sh for simulated versions.
set -euo pipefail

TARGET="${1:-ob_bench_throughput}"
OPS="${2:-2000000}"
OUT="${OB_PROFILE_OUT:-bench/results/profile}"
mkdir -p "$OUT"

docker run --rm --privileged -v "$PWD":/w -w /w alpine:3.20 sh -euc "
  apk add --no-cache cmake ninja g++ musl-dev perf >/dev/null 2>&1
  cmake -S . -B /tmp/bp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DOB_BUILD_BENCH=ON -DOB_BUILD_TESTS=OFF \
        -DCMAKE_CXX_FLAGS='-fno-omit-frame-pointer -g' >/dev/null 2>&1
  cmake --build /tmp/bp >/dev/null 2>&1

  perf record -q -F 999 -e cpu-clock -g --call-graph fp \
    -o /tmp/perf.data /tmp/bp/bench/$TARGET --ops $OPS >/dev/null 2>&1

  echo '=== top symbols by self time (software cpu-clock; no PMU in this VM) ==='
  perf report -i /tmp/perf.data --stdio --no-children --percent-limit 0.5 2>/dev/null \
    | grep -v '^#' | grep -v '^\$' | head -30

  # Folded stacks: dependency-free, and enough for any flamegraph tool later.
  perf script -i /tmp/perf.data > /w/$OUT/perf-script.txt 2>/dev/null || true
  perf report -i /tmp/perf.data --stdio --no-children 2>/dev/null > /w/$OUT/report.txt || true
"
echo "Profile artifacts in $OUT" >&2
