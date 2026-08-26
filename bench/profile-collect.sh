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
REQUEST_TIMEOUT_SECONDS=${REQUEST_TIMEOUT_SECONDS:-900}
FIXTURES=${FIXTURES:-"arith-rt cascade-rt matrix-rt sieve-rt string-rt fib-rt json-rt prop-loop-rt prop-hoist-rt copy-chain-rt branch-const-rt cse-loop-rt licm-rt v8-suite-mod v8-suite-rt"}
# Self-benchmarking suites whose output scores are timing-derived
# (ops/sec over a fixed window): source vs opt bodies differ by run noise,
# so byte equality is meaningless; they get a structural check instead.
TIMING=${TIMING:-"v8-suite-rt v8-suite-mod"}

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
    rewrite_qjsb="$OUT/$name.rewrite.qjsb"
    "$COMPILE" \
        --source "$src" --source-name "$source_name" \
        --application "bench" --version "bench-1" --key-id "bench-key" \
        --bytecode-out "$rewrite_qjsb" \
        --attestation-out "$OUT/$name.attestation.json" \
        --signing-message-out "$OUT/$name.signing-message.bin" \
        2>"$OUT/$name.compile.stderr" || {
        echo "$name: compile failed" >&2; continue; }
    taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
        --mode source --input "$src" --source-name "$source_name" \
        --rounds 1 --warmup 0 --timeout-seconds "$REQUEST_TIMEOUT_SECONDS" \
        >"$OUT/$name.source.jsonl" 2>"$OUT/$name.source.profile.jsonl" || {
        echo "$name: source mode failed" >&2; continue; }
    # exec-throughput writes the body with literal newlines inside the JSON
    # string (not \n escapes), so per-line grep cannot see the closing
    # quote; extract with a dotall regex instead (works for single-line
    # bodies too, e.g. the numeric-response fixtures).
    body=$(python3 -c 'import re,sys; d=sys.stdin.read(); m=re.search(r"\"body\":\"(.*)\",\"ok\":true\}", d, re.S); print(m.group(1) if m else "")' \
        < "$OUT/$name.source.jsonl")
    expect_arg=()
    if [[ " $TIMING " != *" $name "* ]]; then
        expect_arg=(--expect-body "$body")
    fi
    taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
        --mode opt --input "$rewrite_qjsb" --source-name "$source_name" \
        --rounds 1 --warmup 0 --timeout-seconds "$REQUEST_TIMEOUT_SECONDS" \
        "${expect_arg[@]}" \
        >"$OUT/$name.opt.jsonl" 2>"$OUT/$name.opt.profile.jsonl" || {
        echo "$name: opt mode failed" >&2; continue; }
    if [[ " $TIMING " == *" $name "* ]]; then
        # Structural check for timing-based fixtures: the opt run must
        # report the same benchmark lines as the source run (a missing
        # line would mean a crash or a broken benchmark, e.g. a null
        # field access aborting DeltaBlue).
        src_marks=$(grep -cE '^[A-Za-z]+: [0-9]+' \
            "$OUT/$name.source.jsonl" || true)
        opt_marks=$(grep -cE '^[A-Za-z]+: [0-9]+' \
            "$OUT/$name.opt.jsonl" || true)
        if [[ "$src_marks" != "$opt_marks" ]]; then
            echo "$name: benchmark line count mismatch (source $src_marks, opt $opt_marks)" >&2
            continue
        fi
    fi
    src_n=$(grep -c '"schema":"quickjs-ng-opcode-profile-v4"' \
        "$OUT/$name.source.profile.jsonl" || true)
    opt_n=$(grep -c '"schema":"quickjs-ng-opcode-profile-v4"' \
        "$OUT/$name.opt.profile.jsonl" || true)
    echo "$name: $src_n source profiles, $opt_n opt profiles"
done

# Runtime function ids are deliberately local to one dump. Aggregate only
# opcode patterns here; any concrete lowering must rediscover and re-prove a
# matching site in the serialized bundle's CFG+SSA form.
python3 bench/profile_sequences.py "$OUT" --mode source \
    --source-template 'file:///app/{name}.js' \
    --json-out "$OUT/sequences.source.json" \
    | tee "$OUT/sequences.source.txt"
python3 bench/profile_sequences.py "$OUT" --mode opt \
    --source-template 'file:///app/{name}.js' \
    --json-out "$OUT/sequences.opt.json" \
    | tee "$OUT/sequences.opt.txt"

find "$OUT" -maxdepth 1 -type f ! -name sha256sums.txt -print0 | sort -z | \
    xargs -0 sha256sum >"$OUT/sha256sums.txt"
