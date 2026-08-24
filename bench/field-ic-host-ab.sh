#!/usr/bin/env bash
# End-to-end Hono screen using two same-identity O3+LTO host/worker pools.
# Only CAPSID_SHAPE_IC_MODE differs; loadgen samples arms in balanced order.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/field-ic-host-ab-$(date +%Y%m%dT%H%M%S)}
HOST=${HOST:?set HOST to the matching capsid-host}
WORKER=${WORKER:?set WORKER to the O3+LTO IC worker}
BUNDLE=${BUNDLE:-/tmp/hono-bench-bundle.mjs}
LOADGEN=${LOADGEN:-bench/bin/loadgen}
PAIRS=${PAIRS:-7}
WARMUP_S=${WARMUP_S:-1}
DURATION_S=${DURATION_S:-3}
WORKLOADS=${WORKLOADS:-"json matrix-bytes-4k matrix-stream-4k"}
mkdir -p "$OUT"

start_host() {
    local mode=$1 port=$2 log=$3
    CAPSID_SHAPE_IC_MODE="$mode" CAPSID_SHAPE_IC_REPORT=1 \
        taskset -c 0-3 "$HOST" --mode static-pool --workers 2 \
        --worker "$WORKER" --source-bundle "$BUNDLE" \
        --source-name file://$BUNDLE --application orders \
        --listen 127.0.0.1:"$port" --routing path --public-scheme http \
        --public-authority public.example --request-timeout-ms 10000 \
        --initial-stream-window 16384 --strict-sandbox off --ready-fd 1 \
        >"$OUT/$log.out" 2>"$OUT/$log.err" &
    echo $!
}

off_pid=$(start_host off 18121 host-off)
adaptive_pid=$(start_host adaptive 18122 host-adaptive)
cleanup() {
    kill "$off_pid" "$adaptive_pid" 2>/dev/null || true
    wait "$off_pid" "$adaptive_pid" 2>/dev/null || true
}
trap cleanup EXIT

for endpoint in \
    http://127.0.0.1:18121/@capsid/orders/bench/json \
    http://127.0.0.1:18122/@capsid/orders/bench/json; do
    ready=0
    for attempt in $(seq 1 60); do
        if curl -fsS -o /dev/null "$endpoint"; then ready=1; break; fi
        sleep 0.2
    done
    test "$ready" -eq 1
done

{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "pairs: $PAIRS warmup_s: $WARMUP_S duration_s: $DURATION_S"
    echo "workloads: $WORKLOADS"
    sha256sum "$HOST" "$WORKER" "$BUNDLE" "$LOADGEN"
} >"$OUT/manifest.txt"

run_one() {
    local workload=$1 pair=$2 slot=$3 mode=$4 port
    if [[ $mode == off ]]; then port=18121; else port=18122; fi
    CAPSID_BENCH_TARGET=http://127.0.0.1:$port \
    CAPSID_BENCH_WORKLOAD="$workload" \
    CAPSID_BENCH_WARMUP_S="$WARMUP_S" \
    CAPSID_BENCH_DURATION_S="$DURATION_S" \
    CAPSID_BENCH_CONNECTIONS=64 CAPSID_BENCH_INFLIGHT=64 \
    CAPSID_BENCH_SIDE="$mode" CAPSID_BENCH_ROUND="$pair" \
    CAPSID_BENCH_SAMPLES_OUT="$OUT/samples.$workload.$pair.$slot.$mode.jsonl" \
    CAPSID_BENCH_CORRECTNESS_OUT="$OUT/correctness.$workload.$pair.$slot.$mode.json" \
        taskset -c 4-7 "$LOADGEN" \
        >"$OUT/loadgen.$workload.$pair.$slot.$mode.out" \
        2>"$OUT/loadgen.$workload.$pair.$slot.$mode.err"
}

for workload in $WORKLOADS; do
    for pair in $(seq 1 "$PAIRS"); do
        if (( pair % 2 == 1 )); then
            order=(off adaptive adaptive off)
        else
            order=(adaptive off off adaptive)
        fi
        for slot in 0 1 2 3; do
            run_one "$workload" "$pair" "$slot" "${order[$slot]}"
        done
    done
done

cleanup
trap - EXIT
python3 bench/field-ic-host-analyze.py "$OUT" | tee "$OUT/summary.txt"
find "$OUT" -maxdepth 1 -type f ! -name sha256sums.txt -print0 | sort -z | \
    xargs -0 sha256sum >"$OUT/sha256sums.txt"
echo "results in $OUT"
