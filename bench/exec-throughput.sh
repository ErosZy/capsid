#!/usr/bin/env bash
# Execution-throughput comparison for the bytecode AOT optimizer (Step 8,
# G3/G5): source vs unoptimized bytecode vs optimized bytecode, per
# compute-dense fixture. Protocol mirrors cold-start.sh (4C): CPU pinned
# to SUT_CPUSET, 1 warmup run discarded + ROUNDS measured runs, median
# reported; manifest records commit, runner hashes, and environment.
#
# Usage: bash bench/exec-throughput.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/exec-throughput-$(date +%Y%m%dT%H%M%S)}
mkdir -p "$OUT"
BUILD=${BUILD:-build-release}
WORKER=${WORKER:-$BUILD/capsid-worker}
COMPILE=${COMPILE:-$BUILD/capsid-bytecode-compile}
RAW=${RAW:-bench/bin/bytecode-raw}
THROUGHPUT=${THROUGHPUT:-bench/bin/exec-throughput}
ANALYZE=${ANALYZE:-bench/bin/analyze}
SUT_CPUSET=${SUT_CPUSET:-0-3}
ROUNDS=${ROUNDS:-5}
FIXTURES=${FIXTURES:-"arith-rt cascade-rt matrix-rt sieve-rt string-rt fib-rt json-rt prop-loop-rt prop-hoist-rt copy-chain-rt branch-const-rt cse-loop-rt licm-rt"}

echo "rounds: $ROUNDS sut_cpuset: $SUT_CPUSET" | tee "$OUT/manifest.txt"
{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "tag: $(git describe --exact-match --tags HEAD 2>/dev/null || echo untagged)"
    echo "runner_sha256: $(sha256sum bench/exec-throughput.sh | cut -d' ' -f1)"
    echo "runner_diff_sha256: $(git diff --no-ext-diff --binary -- bench/exec-throughput.sh | sha256sum | cut -d' ' -f1)"
    echo "command: OUT=$OUT BUILD=$BUILD WORKER=$WORKER COMPILE=$COMPILE RAW=$RAW THROUGHPUT=$THROUGHPUT ANALYZE=$ANALYZE FIXTURES=$FIXTURES bash bench/exec-throughput.sh"
    echo "uname: $(uname -a)"
    echo "cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "nproc: $(nproc)"
    echo "mem_total: $(sed -n 's/^MemTotal:[[:space:]]*//p' /proc/meminfo)"
    sha256sum "$WORKER" "$COMPILE" "$RAW" "$THROUGHPUT" "$ANALYZE"
    echo "worker: $WORKER"
    echo "compiler: $COMPILE"
} >> "$OUT/manifest.txt"

median() {
    sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

# --- per fixture: static analysis + three-state timing ---
for name in $FIXTURES; do
    src="bench/fixtures/$name.js"
    source_name="file:///app/$name.js"
    raw_qjsb="$OUT/$name.raw.qjsb"
    opt_qjsb="$OUT/$name.opt.qjsb"

    # 1. Static evidence: unoptimized bundle + analyze_only ceiling +
    #    the compiler's report line (raw -> optimized insns/bytes).
    "$RAW" "$src" "$source_name" "$raw_qjsb" 2>"$OUT/$name.raw.stderr"
    "$ANALYZE" "$raw_qjsb" 2>"$OUT/$name.analyze.stderr" || {
        echo "$name: analyze failed" >&2; continue; }
    "$COMPILE" \
        --source "$src" --source-name "$source_name" \
        --application "bench" --version "bench-1" --key-id "bench-key" \
        --bytecode-out "$opt_qjsb" \
        --attestation-out "$OUT/$name.attestation.json" \
        --signing-message-out "$OUT/$name.signing-message.bin" \
        2>"$OUT/$name.opt.stderr"

    # 2. Timing: source run first (establishes the expected body), then
    #    raw and opt with the body cross-checked byte-for-byte.
    taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
        --mode source --input "$src" --source-name "$source_name" \
        --rounds "$ROUNDS" --warmup 1 \
        >"$OUT/$name.source.jsonl" 2>"$OUT/$name.source.err" || {
        echo "$name: source mode failed" >&2; continue; }
    body=$(grep -o '"body":"[^"]*"' "$OUT/$name.source.jsonl" |
           head -1 | sed 's/"body":"\(.*\)"/\1/')
    taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
        --mode raw --input "$raw_qjsb" --source-name "$source_name" \
        --rounds "$ROUNDS" --warmup 1 --expect-body "$body" \
        >"$OUT/$name.raw.jsonl" 2>"$OUT/$name.raw.err" || {
        echo "$name: raw mode failed (body mismatch?)" >&2; continue; }
    taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
        --mode opt --input "$opt_qjsb" --source-name "$source_name" \
        --rounds "$ROUNDS" --warmup 1 --expect-body "$body" \
        >"$OUT/$name.opt.jsonl" 2>"$OUT/$name.opt.err" || {
        echo "$name: opt mode failed (body mismatch?)" >&2; continue; }

    src_med=$(grep -o '"ms":[0-9.]*' "$OUT/$name.source.jsonl" |
              cut -d: -f2 | median)
    raw_med=$(grep -o '"ms":[0-9.]*' "$OUT/$name.raw.jsonl" |
              cut -d: -f2 | median)
    opt_med=$(grep -o '"ms":[0-9.]*' "$OUT/$name.opt.jsonl" |
              cut -d: -f2 | median)

    # G3 delta: optimized vs unoptimized bytecode (the honest baseline).
    g3_delta=$(awk -v a="$raw_med" -v b="$opt_med" \
        'BEGIN { printf "%.2f", 100.0 * (a - b) / a }')
    # Parse-skip noise reference: bytecode vs source (should be ~0).
    load_delta=$(awk -v a="$raw_med" -v b="$src_med" \
        'BEGIN { printf "%.2f", 100.0 * (b - a) / b }')

    printf '%s source_ms=%8.3f raw_ms=%8.3f opt_ms=%8.3f  G3_opt_vs_raw=+%s%%  load_noise=%s%%\n' \
        "$name" "$src_med" "$raw_med" "$opt_med" "$g3_delta" "$load_delta" \
        | tee -a "$OUT/cells.txt"
    { echo "=== $name raw stderr ==="; cat "$OUT/$name.raw.stderr";
      echo "=== $name analyze stderr ==="; cat "$OUT/$name.analyze.stderr";
      echo "=== $name opt stderr (compiler report) ==="; cat "$OUT/$name.opt.stderr"; } \
        >> "$OUT/static.txt"
done

find "$OUT" -maxdepth 1 -type f -print0 | sort -z | \
    xargs -0 sha256sum > "$OUT/sha256sums.txt"
echo "results in $OUT"
