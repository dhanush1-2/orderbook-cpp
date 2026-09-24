#!/usr/bin/env bash
# Deterministic instruction and simulated-cache counts via Cachegrind, in Docker.
#
# MEASURED determinism: repeat runs of an identical binary differ by at most ~13
# instructions out of 153 million, which is why the CI regression gate uses this
# and not wall-clock time.
#
# --cache-sim=yes is required for D refs and miss rates; Cachegrind reports only
# I refs without it. Those miss rates are SIMULATED against a modelled cache, not
# measured on silicon. There is no PMU anywhere in this project to measure them.
set -euo pipefail

TARGET="${1:-ob_cachegrind_probe}"
shift || true
OUT="${OB_CG_OUT:-bench/results/cachegrind}"
mkdir -p "$OUT"

docker run --rm -v "$PWD":/w -w /w alpine:3.20 sh -euc "
  apk add --no-cache cmake ninja g++ musl-dev valgrind >/dev/null 2>&1
  cmake -S . -B /tmp/bc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DOB_BUILD_BENCH=ON -DOB_BUILD_TESTS=OFF >/dev/null 2>&1
  cmake --build /tmp/bc >/dev/null 2>&1
  valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes \
    --cachegrind-out-file=/w/$OUT/cachegrind.out \
    /tmp/bc/bench/$TARGET $* 2>&1 | tee /w/$OUT/summary.txt
"
echo "Cachegrind output in $OUT" >&2
