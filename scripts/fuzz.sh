#!/usr/bin/env bash
# Run the differential fuzzer in Docker.
#
# Docker is not a convenience here: Apple clang ships no libFuzzer runtime, so the
# target cannot be linked on the host at all.
#
# ASAN_OPTIONS=detect_leaks=0 suppresses a libFuzzer-runtime false positive on
# musl: a fuzz target whose body is `return 0;` reports the same 56-byte leak.
set -euo pipefail

SECS="${1:-300}"
CORPUS="${OB_FUZZ_CORPUS:-fuzz/corpus_min}"
mkdir -p "$CORPUS"

docker run --rm -e ASAN_OPTIONS=detect_leaks=0 -v "$PWD":/w -w /w alpine:3.20 sh -euc '
  apk add --no-cache cmake ninja clang lld compiler-rt >/dev/null 2>&1
  CC=clang CXX=clang++ cmake -S . -B /tmp/bf -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DOB_BUILD_FUZZ=ON -DOB_BUILD_TESTS=OFF >/dev/null
  cmake --build /tmp/bf >/dev/null
  /tmp/bf/fuzz/ob_fuzz_differential -max_total_time='"$SECS"' -print_final_stats=1 \
    -artifact_prefix=/w/fuzz/ "/w/'"$CORPUS"'"
'
