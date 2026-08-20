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
PROFILE_WARMUP_S=${PROFILE_WARMUP_S:-5}
PERF=${PERF:-$(command -v perf || true)}
HP=
LOADGEN_PID=
R1=
R2=
WORKER_PID_ARRAY=()

cleanup_components() {
    local pid
    for pid in "${LOADGEN_PID:-}" "${R1:-}" "${R2:-}" "${WORKER_PID_ARRAY[@]}" "${HP:-}"; do
        [ -n "$pid" ] || continue
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${LOADGEN_PID:-}" "${R1:-}" "${R2:-}" "${WORKER_PID_ARRAY[@]}" "${HP:-}"; do
        [ -n "$pid" ] || continue
        wait "$pid" 2>/dev/null || true
    done
    LOADGEN_PID=
    R1=
    R2=
    HP=
    WORKER_PID_ARRAY=()
}
trap cleanup_components EXIT INT TERM

if [ -z "$PERF" ] || [ ! -x "$PERF" ]; then
    echo "profile-four-stacks: perf is required but not found on PATH" >&2
    exit 2
fi

{
    echo "profile_s: $PROFILE_S warmup_s: $PROFILE_WARMUP_S workload: json16k"
    echo "perf: $PERF"
    sha256sum "$HOST_BIN" "$WORKER" "$BUNDLE" "$LOADGEN" "$PERF"
} | tee "$OUT/manifest.txt"

profile_loadgen() {
    # The caller observes the load generator's exit status and retains its
    # correctness output even though this is a profiling-only session.
    env CAPSID_BENCH_TARGET="$1" CAPSID_BENCH_WORKLOAD=json16k \
        CAPSID_BENCH_WARMUP_S="$PROFILE_WARMUP_S" \
        CAPSID_BENCH_DURATION_S="$PROFILE_S" \
        CAPSID_BENCH_CONNECTIONS=64 CAPSID_BENCH_INFLIGHT=64 \
        CAPSID_BENCH_SIDE=profile CAPSID_BENCH_ROUND=0 \
        CAPSID_BENCH_SAMPLES_OUT="$2" CAPSID_BENCH_CORRECTNESS_OUT="$3" \
        taskset -c "$LOADGEN_CPUSET" "$LOADGEN"
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
ready=0
for _ in $(seq 1 60); do
    if curl --fail --silent --output /dev/null --max-time 2 \
        http://127.0.0.1:18102/@capsid/orders/fixed; then
        ready=1
        break
    fi
    sleep 0.2
done
if [ "$ready" -ne 1 ]; then
    echo "profile-four-stacks: capsid host did not become ready" >&2
    exit 2
fi
sleep 1
mapfile -t WORKER_PID_ARRAY < <(pgrep -P "$HP" || true)
[ "${#WORKER_PID_ARRAY[@]}" -gt 0 ] || {
    echo "profile-four-stacks: cannot find capsid worker pids" >&2
    exit 2
}
WORKER_PIDS="${WORKER_PID_ARRAY[*]}"
WORKER_PID_CSV=$(IFS=,; echo "${WORKER_PID_ARRAY[*]}")
echo "capsid host=$HP workers=$WORKER_PIDS" | tee -a "$OUT/manifest.txt"

profile_loadgen "http://127.0.0.1:18102" "$OUT/samples.capsid.jsonl" "$OUT/correctness.capsid.json" \
    >"$OUT/loadgen.stdout.log" 2>"$OUT/loadgen.stderr.log" &
LOADGEN_PID=$!
sleep "$PROFILE_WARMUP_S"
if ! kill -0 "$LOADGEN_PID" 2>/dev/null; then
    wait "$LOADGEN_PID"
fi

"$PERF" record -o "$OUT/perf.capsid-host.data" -F 99 -p "$HP" -- sleep "$PROFILE_S" \
    >/dev/null 2>"$OUT/perf.capsid-host.record.err" &
R1=$!
"$PERF" record -o "$OUT/perf.capsid-worker.data" -F 99 \
    -p "$WORKER_PID_CSV" -- sleep "$PROFILE_S" \
    >/dev/null 2>"$OUT/perf.capsid-worker.record.err" &
R2=$!
wait "$LOADGEN_PID"
LOADGEN_PID=
wait "$R1"
R1=
wait "$R2"
R2=
"$PERF" report -i "$OUT/perf.capsid-host.data" --stdio --no-children \
    > "$OUT/profile.capsid-host.txt"
"$PERF" report -i "$OUT/perf.capsid-worker.data" --stdio --no-children \
    > "$OUT/profile.capsid-worker.txt"
cleanup_components
trap - EXIT INT TERM
echo "capsid profiles done"
echo "results in $OUT"
