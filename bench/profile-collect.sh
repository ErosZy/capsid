#!/usr/bin/env bash
# A2: opcode-profile collection over the bench fixtures (tier-3 plan §3.4).
# Runs the profiling worker (built with CAPSID_ENABLE_OPCODE_PROFILE) in
# source and opt modes and captures the per-runtime profile JSON — one line
# per runtime, on the worker's stderr, which the bench harness passes
# through. Production performance is never measured with this build.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/opcode-profile-$(date +%Y%m%dT%H%M%S)}
mkdir -p "$OUT"
BUILD=${BUILD:-build-profile}
WORKER=${WORKER:-$BUILD/capsid-worker}
COMPILE=${COMPILE:-$BUILD/capsid-bytecode-compile}
THROUGHPUT=${THROUGHPUT:-bench/bin/exec-throughput}
SUT_CPUSET=${SUT_CPUSET:-0-3}
FIXTURES=${FIXTURES:-"arith-rt cascade-rt matrix-rt sieve-rt string-rt fib-rt json-rt prop-loop-rt prop-hoist-rt copy-chain-rt branch-const-rt cse-loop-rt licm-rt v8-suite-rt"}

echo "fixtures: $FIXTURES" | tee "$OUT/manifest.txt"
{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "worker: $WORKER"
    echo "worker_sha256: $(sha256sum "$WORKER" | cut -d' ' -f1)"
} >> "$OUT/manifest.txt"

for name in $FIXTURES; do
    src="bench/fixtures/$name.js"
    source_name="file:///app/$name.js"
    opt_qjsb="$OUT/$name.opt.qjsb"
    "$COMPILE" \
        --source "$src" --source-name "$source_name" \
        --application "bench" --version "bench-1" --key-id "bench-key" \
        --bytecode-out "$opt_qjsb" \
        --attestation-out "$OUT/$name.attestation.json" \
        --signing-message-out "$OUT/$name.signing-message.bin" \
        2>"$OUT/$name.compile.stderr" || {
        echo "$name: compile failed" >&2; continue; }
    taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
        --mode source --input "$src" --source-name "$source_name" \
        --rounds 1 --warmup 0 \
        >"$OUT/$name.source.jsonl" 2>"$OUT/$name.source.profile.jsonl" || {
        echo "$name: source mode failed" >&2; continue; }
    body=$(grep -o '"body":"[^"]*"' "$OUT/$name.source.jsonl" |
           head -1 | sed 's/"body":"\(.*\)"/\1/')
    taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
        --mode opt --input "$opt_qjsb" --source-name "$source_name" \
        --rounds 1 --warmup 0 --expect-body "$body" \
        >"$OUT/$name.opt.jsonl" 2>"$OUT/$name.opt.profile.jsonl" || {
        echo "$name: opt mode failed" >&2; continue; }
    src_n=$(grep -c '"schema":"quickjs-ng-opcode-profile-v1"' \
        "$OUT/$name.source.profile.jsonl" || true)
    opt_n=$(grep -c '"schema":"quickjs-ng-opcode-profile-v1"' \
        "$OUT/$name.opt.profile.jsonl" || true)
    echo "$name: $src_n source profiles, $opt_n opt profiles"
done
