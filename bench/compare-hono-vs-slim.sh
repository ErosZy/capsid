#!/usr/bin/env bash
set -euo pipefail
cd /capsid
OUT="bench/results/hono-vs-slim-$(date +%Y%m%dT%H%M%S)"
mkdir -p "$OUT"
HOST_BIN=/capsid/build-linux/capsid-host
WORKER=/capsid/build-linux/capsid-worker
BUNDLE=/tmp/hono-bench-bundle.mjs
LOADGEN=/capsid/bench/bin/loadgen
PHP_TARGET=http://127.0.0.1:18101
CAPSID_TARGET=http://127.0.0.1:18102
HOST_PID=""
start_capsid() {
  "$HOST_BIN" --mode single-worker \
    --worker "$WORKER" --source-bundle "$BUNDLE" \
    --source-name "file://$BUNDLE" \
    --application orders --listen 127.0.0.1:18102 --routing path \
    --public-scheme http --public-authority public.example \
    --request-timeout-ms 10000 --initial-stream-window 16384 --strict-sandbox off \
    --ready-fd 1 >/dev/null 2>&1 &
  HOST_PID=$!
  for i in $(seq 1 50); do
    curl -s -o /dev/null "$CAPSID_TARGET/@capsid/orders/bench/json" && return 0
    sleep 0.1
  done
  echo "capsid host failed to start" >&2
  exit 1
}
run_loadgen() {
  local target="$1" workload="$2" conn="$3" side="$4"
  local samples="$OUT/samples.$side.$workload.$conn.jsonl"
  local corr="$OUT/correctness.$side.$workload.$conn.json"
  env CAPSID_BENCH_TARGET="$target" CAPSID_BENCH_WORKLOAD="$workload" \
    CAPSID_BENCH_WARMUP_S=3 CAPSID_BENCH_DURATION_S=8 \
    CAPSID_BENCH_CONNECTIONS="$conn" CAPSID_BENCH_INFLIGHT="$conn" \
    CAPSID_BENCH_SIDE="$side" CAPSID_BENCH_ROUND=1 \
    CAPSID_BENCH_SAMPLES_OUT="$samples" CAPSID_BENCH_CORRECTNESS_OUT="$corr" \
    "$LOADGEN" >/dev/null
}
echo "side,workload,connections,qps,p50_ms,p95_ms,p99_ms,completed,errors" > "$OUT/results.csv"
for workload in json bytes stream; do
  for conn in 1 4 16 32 64 128; do
    # capsid side
    start_capsid
    run_loadgen "$CAPSID_TARGET" "$workload" "$conn" capsid
    kill $HOST_PID 2>/dev/null || true
    # php side (already running)
    run_loadgen "$PHP_TARGET" "$workload" "$conn" php
  done
done
# summarize
python3 - "$OUT" <<'PY'
import json, sys, csv, glob, os
out = sys.argv[1]
rows = []
for path in sorted(glob.glob(os.path.join(out, "samples.*.jsonl"))):
    base = os.path.basename(path).replace("samples.", "").replace(".jsonl", "")
    side, workload, conn = base.split(".")
    for line in open(path):
        s = json.loads(line)
        if s.get("phase") == "measured":
            rows.append((side, workload, int(conn), s["qps"], s["p50_ms"], s["p95_ms"], s["p99_ms"], s["completed"], s["errors"]))
rows.sort(key=lambda r: (r[1], r[2], r[0]))
print(f"{'side':6s} {'workload':8s} {'conn':>4s} {'QPS':>9s} {'p50':>7s} {'p95':>7s} {'p99':>7s} {'completed':>9s} err")
for r in rows:
    print(f"{r[0]:6s} {r[1]:8s} {r[2]:4d} {r[3]:9.1f} {r[4]:7.2f} {r[5]:7.2f} {r[6]:7.2f} {r[7]:9d} {r[8]:3d}")
PY
echo "results in $OUT"
