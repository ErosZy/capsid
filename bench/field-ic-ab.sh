#!/usr/bin/env bash
# Same-binary OFF/ADAPTIVE A/B for the compile-gated runtime field IC.
# Seven balanced ABBA/BAAB pairs cover a long-lived mono receiver, a fresh
# receiver per request, and the bundled Hono JSON route.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/field-ic-ab-$(date +%Y%m%dT%H%M%S)}
WORKER=${WORKER:?set WORKER to the O3+LTO IC worker}
HARNESS=${HARNESS:?set HARNESS to exec-throughput}
HONO_BUNDLE=${HONO_BUNDLE:-/tmp/hono-bench-bundle.mjs}
CPUSET=${CPUSET:-2-3}
PAIRS=${PAIRS:-7}
mkdir -p "$OUT"

{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "worktree_dirty: $(test -n "$(git status --porcelain)" && echo true || echo false)"
    echo "cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "kernel: $(uname -srvo)"
    echo "cpuset: $CPUSET"
    echo "pairs: $PAIRS"
    echo "worker: $WORKER"
    echo "harness: $HARNESS"
    echo "hono_bundle: $HONO_BUNDLE"
    sha256sum "$WORKER" "$HARNESS" "$HONO_BUNDLE"
} >"$OUT/manifest.txt"

run_one() {
    local case_name=$1 pair=$2 slot=$3 mode=$4 input=$5 source_name=$6
    local url=$7 warmup=$8 rounds=$9
    CAPSID_SHAPE_IC_MODE="$mode" CAPSID_SHAPE_IC_REPORT=1 \
        taskset -c "$CPUSET" "$HARNESS" --worker "$WORKER" \
        --mode source --input "$input" --source-name "$source_name" \
        --url "$url" --warmup "$warmup" --rounds "$rounds" \
        >"$OUT/$case_name.$pair.$slot.$mode.jsonl" \
        2>"$OUT/$case_name.$pair.$slot.$mode.err"
}

for pair in $(seq 1 "$PAIRS"); do
    if (( pair % 2 == 1 )); then
        order=(off adaptive adaptive off)
    else
        order=(adaptive off off adaptive)
    fi
    for slot in 0 1 2 3; do
        mode=${order[$slot]}
        run_one mono "$pair" "$slot" "$mode" \
            bench/fixtures/field-ic-mono-rt.js file:///app/field-ic-mono-rt.js \
            https://example.test/sync 1 1
        run_one fresh "$pair" "$slot" "$mode" \
            bench/fixtures/prop-hoist-rt.js file:///app/prop-hoist-rt.js \
            https://example.test/sync 1 1
        run_one hono "$pair" "$slot" "$mode" \
            "$HONO_BUNDLE" file:///app/hono-bench.mjs \
            https://example.test/bench/json 256 200
    done
done

python3 bench/field-ic-analyze.py "$OUT" | tee "$OUT/summary.txt"
find "$OUT" -maxdepth 1 -type f ! -name sha256sums.txt -print0 | sort -z | \
    xargs -0 sha256sum >"$OUT/sha256sums.txt"
echo "results in $OUT"
