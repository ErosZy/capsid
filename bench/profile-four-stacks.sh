#!/usr/bin/env bash
# bench/profile-four-stacks.sh — perf profile of capsid under a
# representative json16k load (30s measured), evidence-rule profiles:
# capsid host and worker profiled separately.  Profiles are only taken
# for capsid — it is the stack under test; the php/flask/fastapi stacks
# serve as comparison references and need no perf profiles.
#
# Usage: bash bench/profile-four-stacks.sh [OUT_DIR]
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${1:-bench/results/four-qps-profile-$(date +%Y%m%dT%H%M%S)}
mkdir -p "$OUT"
HOST_BIN=${HOST_BIN:-build-m1d/capsid-host}
WORKER=${WORKER:-build-m1d/capsid-worker}
BUNDLE=${BUNDLE:-/tmp/hono-bench-bundle.mjs}
LOADGEN=${LOADGEN:-bench/bin/loadgen}
SUT_CPUSET=${SUT_CPUSET:-0-3}
LOADGEN_CPUSET=${LOADGEN_CPUSET:-4-7}
PROFILE_S=${PROFILE_S:-30}

{
    echo "profile_s: $PROFILE_S workload: json16k"
    sha256sum "$HOST_BIN" "$WORKER" "$BUNDLE" "$LOADGEN" "$(command -v perf)"
} | tee "$OUT/manifest.txt"

profile_loadgen() {
    # loadgen run that stays up for the profile window; correctness not
    # enforced here (profiling session only).
    env CAPSID_BENCH_TARGET="$1" CAPSID_BENCH_WORKLOAD=json16k \
        CAPSID_BENCH_WARMUP_S=5 CAPSID_BENCH_DURATION_S="$PROFILE_S" \
        CAPSID_BENCH_CONNECTIONS=64 CAPSID_BENCH_INFLIGHT=64 \
        CAPSID_BENCH_SIDE=profile CAPSID_BENCH_ROUND=0 \
        CAPSID_BENCH_SAMPLES_OUT="$2" CAPSID_BENCH_CORRECTNESS_OUT="$3" \
        taskset -c "$LOADGEN_CPUSET" "$LOADGEN" >/dev/null 2>&1
}

profile_run() {
    local side="$1"; shift
    # pids: remaining args; sleep 4s so the load reaches steady state
    ( sleep 4; taskset -c "$LOADGEN_CPUSET" \
        perf record -o "$OUT/perf.$side.data" -F 99 \
        -p "$(echo "$@" | tr ' ' ',')" -- sleep "$PROFILE_S" ) &
    local rec=$!
    sleep 1
    profile_loadgen "http://127.0.0.1:$PORT" "$OUT/samples.$side.jsonl" "$OUT/correctness.$side.json" || true
    wait "$rec" 2>/dev/null || true
    perf report -i "$OUT/perf.$side.data" --stdio --no-children 2>/dev/null \
        > "$OUT/profile.$side.txt"
    echo "$side profile -> $OUT/profile.$side.txt"
}

# --- capsid: host + 2 workers profiled separately ---
PORT=18102
taskset -c "$SUT_CPUSET" "$HOST_BIN" --mode static-pool --workers 2 \
    --worker "$WORKER" --source-bundle "$BUNDLE" \
    --source-name "file://$BUNDLE" \
    --application orders --listen 127.0.0.1:18102 --routing path \
    --public-scheme http --public-authority public.example \
    --request-timeout-ms 10000 --initial-stream-window 16384 \
    --strict-sandbox off --ready-fd 1 >/dev/null 2>"$OUT/capsid-host.log" &
HP=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null -m 2 http://127.0.0.1:18102/@capsid/orders/fixed && break
    sleep 0.2
done
sleep 1
WORKER_PIDS=$(pgrep -f "^${WORKER#"$PWD/"}" 2>/dev/null | tr '\n' ' ' || true)
# fallback: any capsid-worker child of the host
[ -n "$WORKER_PIDS" ] || WORKER_PIDS=$(pgrep -P "$HP" | tr '\n' ' ')
echo "capsid host=$HP workers=$WORKER_PIDS" | tee -a "$OUT/manifest.txt"
( sleep 4; perf record -o "$OUT/perf.capsid-host.data" -F 99 -p "$HP" -- sleep "$PROFILE_S" ) &
R1=$!
( sleep 4; perf record -o "$OUT/perf.capsid-worker.data" -F 99 -p "$(echo "$WORKER_PIDS" | tr ' ' ',')" -- sleep "$PROFILE_S" ) &
R2=$!
sleep 1
profile_loadgen "http://127.0.0.1:18102" "$OUT/samples.capsid.jsonl" "$OUT/correctness.capsid.json" || true
wait "$R1" "$R2" 2>/dev/null || true
perf report -i "$OUT/perf.capsid-host.data" --stdio --no-children 2>/dev/null > "$OUT/profile.capsid-host.txt"
perf report -i "$OUT/perf.capsid-worker.data" --stdio --no-children 2>/dev/null > "$OUT/profile.capsid-worker.txt"
kill "$HP" 2>/dev/null || true
wait "$HP" 2>/dev/null || true
echo "capsid profiles done"
echo "results in $OUT"
