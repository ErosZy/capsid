#!/usr/bin/env bash
# Same-source/same-flags patchless versus an IC feature build. By default the
# feature build runs with the IC OFF to attribute compiled-in tax. Set
# FEATURE_MODE=adaptive and FEATURE_LABEL=adaptive for end-to-end net benefit.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/field-ic-off-tax-$(date +%Y%m%dT%H%M%S)}
PATCHLESS_WORKER=${PATCHLESS_WORKER:?set PATCHLESS_WORKER}
OFF_WORKER=${OFF_WORKER:?set OFF_WORKER}
HARNESS=${HARNESS:?set HARNESS}
HONO_BUNDLE=${HONO_BUNDLE:-/tmp/hono-bench-bundle.mjs}
FEATURE_MODE=${FEATURE_MODE:-off}
FEATURE_LABEL=${FEATURE_LABEL:-off}
PAIRS=${PAIRS:-7}
CPUSET=${CPUSET:-2-3}
if [[ $FEATURE_LABEL == patchless ]]; then
    echo "FEATURE_LABEL must not be patchless" >&2
    exit 2
fi
mkdir -p "$OUT"

run_one() {
    local case_name=$1 pair=$2 slot=$3 mode=$4 input=$5 source_name=$6
    local url=$7 warmup=$8 rounds=$9 worker runtime_mode
    if [[ $mode == patchless ]]; then worker=$PATCHLESS_WORKER; else worker=$OFF_WORKER; fi
    if [[ $mode == patchless ]]; then
        runtime_mode=off
    else
        runtime_mode=$FEATURE_MODE
    fi
    CAPSID_SHAPE_IC_MODE="$runtime_mode" taskset -c "$CPUSET" \
        "$HARNESS" --worker "$worker" \
        --mode source --input "$input" --source-name "$source_name" \
        --url "$url" --warmup "$warmup" --rounds "$rounds" \
        >"$OUT/$case_name.$pair.$slot.$mode.jsonl" \
        2>"$OUT/$case_name.$pair.$slot.$mode.err"
}

for pair in $(seq 1 "$PAIRS"); do
    if (( pair % 2 == 1 )); then
        order=(patchless "$FEATURE_LABEL" "$FEATURE_LABEL" patchless)
    else
        order=("$FEATURE_LABEL" patchless patchless "$FEATURE_LABEL")
    fi
    for slot in 0 1 2 3; do
        mode=${order[$slot]}
        run_one mono "$pair" "$slot" "$mode" \
            bench/fixtures/field-ic-mono-rt.js file:///app/field-ic-mono-rt.js \
            https://example.test/sync 1 1
        run_one fresh "$pair" "$slot" "$mode" \
            bench/fixtures/prop-hoist-rt.js file:///app/prop-hoist-rt.js \
            https://example.test/sync 1 1
        run_one hono "$pair" "$slot" "$mode" "$HONO_BUNDLE" \
            file:///app/hono-bench.mjs https://example.test/bench/json 256 200
    done
done

{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "cpuset: $CPUSET"
    echo "pairs: $PAIRS"
    echo "feature_mode: $FEATURE_MODE"
    echo "feature_label: $FEATURE_LABEL"
    sha256sum "$PATCHLESS_WORKER" "$OFF_WORKER" "$HARNESS" "$HONO_BUNDLE"
} >"$OUT/manifest.txt"
python3 bench/field-ic-tax-analyze.py "$OUT" | tee "$OUT/summary.txt"
find "$OUT" -maxdepth 1 -type f ! -name sha256sums.txt -print0 | sort -z | \
    xargs -0 sha256sum >"$OUT/sha256sums.txt"
echo "results in $OUT"
