#!/usr/bin/env bash
# bench/compare-hardening-regression.sh — 加固批次性能回归 A/B。
#
# 对比 5dc40ec（加固前，2026-08-06）与 HEAD（加固后）的 capsid-host：
# 同一 hono bundle、同一 loadgen（taskset 锁核）。每轮交错运行两侧以抵消
# 机器漂移，轮间交替起始侧（奇数轮 old→new，偶数轮 new→old）。
#
# worker 兼容 ID 随提交演进（WP-09 §13.1/§13.2 改动 client.cc 后旧 host
# 拒绝新 worker），A/B 两侧必须各自配对同代 worker：默认 OLD_WORKER 取
# /tmp/capsid-old/build-linux/capsid-worker、NEW_WORKER 取
# /capsid/build-linux/capsid-worker；设 WORKER 可同时覆盖两侧（旧行为）。
#
# 使用（容器内）：
#   bash bench/compare-hardening-regression.sh
# 产物：bench/results/hardening-regression-<ts>/（gitignored）
set -euo pipefail
cd /capsid

OUT=${OUT:-"bench/results/hardening-regression-$(date +%Y%m%dT%H%M%S)"}
mkdir -p "$OUT"
OLD_HOST=${OLD_HOST:-/tmp/capsid-old/build-linux/capsid-host}
NEW_HOST=${NEW_HOST:-/capsid/build-linux/capsid-host}
OLD_WORKER=${OLD_WORKER:-${WORKER:-/tmp/capsid-old/build-linux/capsid-worker}}
NEW_WORKER=${NEW_WORKER:-${WORKER:-/capsid/build-linux/capsid-worker}}
BUNDLE=${BUNDLE:-/tmp/hono-bench-bundle.mjs}
LOADGEN=${LOADGEN:-/capsid/bench/bin/loadgen}
LOADGEN_CORE=${LOADGEN_CORE:-3}
SERVICE_CORESET=${SERVICE_CORESET:-0-2}
WORKERS=${WORKERS:-1}
WORKLOADS=${WORKLOADS:-"json json16k json64k"}
ROUNDS=${ROUNDS:-3}
WARMUP_S=${WARMUP_S:-3}
DURATION_S=${DURATION_S:-8}
LATENCY_ROUNDS=${LATENCY_ROUNDS:-2}

case "$WORKERS" in
1|2|4|6|8) ;;
*) echo "WORKERS must be 1, 2, 4, 6 or 8 (got $WORKERS)" >&2; exit 2 ;;
esac
for numeric in ROUNDS WARMUP_S DURATION_S LATENCY_ROUNDS; do
    value=${!numeric}
    if ! [[ "$value" =~ ^[0-9]+$ ]]; then
        echo "$numeric must be a non-negative integer (got $value)" >&2
        exit 2
    fi
done
if [ "$ROUNDS" -lt 1 ] || [ "$DURATION_S" -lt 1 ]; then
    echo "ROUNDS and DURATION_S must be positive" >&2
    exit 2
fi

echo "old host: $OLD_HOST"
echo "new host: $NEW_HOST"
echo "service cpuset: $SERVICE_CORESET; loadgen core: $LOADGEN_CORE"
echo "workers: $WORKERS; rounds: $ROUNDS; warmup/duration: $WARMUP_S/$DURATION_S"
sha256sum "$OLD_HOST" "$NEW_HOST" "$OLD_WORKER" "$NEW_WORKER" "$BUNDLE" "$LOADGEN" | tee "$OUT/identity.sha256"

PORT_OLD=18111
PORT_NEW=18112
HOST_PID=""

start_host() {
    local bin="$1" port="$2" worker="$3"
    local mode_args=(--mode single-worker)
    if [ "$WORKERS" -gt 1 ]; then
        mode_args=(--mode static-pool --workers "$WORKERS")
    fi
    taskset -c "$SERVICE_CORESET" "$bin" "${mode_args[@]}" \
        --worker "$worker" --source-bundle "$BUNDLE" \
        --source-name "file://$BUNDLE" \
        --application orders --listen "127.0.0.1:$port" --routing path \
        --public-scheme http --public-authority public.example \
        --request-timeout-ms 10000 --initial-stream-window 16384 \
        --strict-sandbox off --ready-fd 1 >/dev/null 2>&1 &
    HOST_PID=$!
    for _ in $(seq 1 60); do
        curl -s -o /dev/null "http://127.0.0.1:$port/@capsid/orders/fixed" && return 0
        sleep 0.1
    done
    echo "host failed to start: $bin" >&2
    kill "$HOST_PID" 2>/dev/null || true
    exit 1
}

stop_host() {
    kill "$HOST_PID" 2>/dev/null || true
    wait "$HOST_PID" 2>/dev/null || true
}

run_loadgen() {
    local target="$1" workload="$2" side="$3" round="$4" conns="$5"
    local samples="$OUT/samples.$side.$workload.c$conns.r$round.jsonl"
    local corr="$OUT/correctness.$side.$workload.c$conns.r$round.json"
    env CAPSID_BENCH_TARGET="$target" CAPSID_BENCH_WORKLOAD="$workload" \
        CAPSID_BENCH_WARMUP_S="$WARMUP_S" CAPSID_BENCH_DURATION_S="$DURATION_S" \
        CAPSID_BENCH_CONNECTIONS="$conns" CAPSID_BENCH_INFLIGHT="$conns" \
        CAPSID_BENCH_SIDE="$side" CAPSID_BENCH_ROUND="$round" \
        CAPSID_BENCH_SAMPLES_OUT="$samples" CAPSID_BENCH_CORRECTNESS_OUT="$corr" \
        taskset -c "$LOADGEN_CORE" "$LOADGEN" >/dev/null
}

echo "side,workload,conn,round,qps,p50_ms,p95_ms,p99_ms,completed,errors" > "$OUT/results.csv"

side_host() {
    case "$1" in
        old) printf '%s\n' "$OLD_HOST" ;;
        new) printf '%s\n' "$NEW_HOST" ;;
    esac
}

side_worker() {
    case "$1" in
        old) printf '%s\n' "$OLD_WORKER" ;;
        new) printf '%s\n' "$NEW_WORKER" ;;
    esac
}

# 主矩阵：所选 workload × 64 并发 × ROUNDS 轮交错。
for workload in $WORKLOADS; do
    for round in $(seq 1 "$ROUNDS"); do
        if [ $((round % 2)) -eq 1 ]; then
            FIRST=old SECOND=new
        else
            FIRST=new SECOND=old
        fi
        start_host "$(side_host "$FIRST")" $PORT_OLD "$(side_worker "$FIRST")"
        run_loadgen "http://127.0.0.1:$PORT_OLD" "$workload" "$FIRST" "$round" 64
        stop_host
        start_host "$(side_host "$SECOND")" $PORT_NEW "$(side_worker "$SECOND")"
        run_loadgen "http://127.0.0.1:$PORT_NEW" "$workload" "$SECOND" "$round" 64
        stop_host
    done
done

# 延迟探针：64k × 1 并发（credit 往返路径）。
for round in $(seq 1 "$LATENCY_ROUNDS"); do
    start_host "$NEW_HOST" $PORT_NEW "$NEW_WORKER"
    run_loadgen "http://127.0.0.1:$PORT_NEW" json64k new "$round" 1
    stop_host
    start_host "$OLD_HOST" $PORT_OLD "$OLD_WORKER"
    run_loadgen "http://127.0.0.1:$PORT_OLD" json64k old "$round" 1
    stop_host
done

# 汇总：每侧每个 workload×conn 取全部完成轮次的均值。
python3 - "$OUT" <<'PY'
import json, sys, os, glob
out = sys.argv[1]
rows = {}
for path in sorted(glob.glob(os.path.join(out, "samples.*.jsonl"))):
    base = os.path.basename(path).replace("samples.", "").replace(".jsonl", "")
    side, workload, conn, round_ = base.split(".")
    conn = int(conn.lstrip("c"))
    for line in open(path):
        s = json.loads(line)
        if s.get("phase") != "measured":
            continue
        key = (side, workload, conn)
        rows.setdefault(key, []).append(s)
print(f"{'side':4s} {'workload':8s} {'conn':>4s} {'rounds':>6s} {'QPS':>9s} {'p50_ms':>8s} {'p95_ms':>8s} {'p99_ms':>8s} err")
summary = {}
for key, samples in sorted(rows.items()):
    side, workload, conn = key
    n = len(samples)
    qps = sum(s["qps"] for s in samples) / n
    p50 = sum(s["p50_ms"] for s in samples) / n
    p95 = sum(s["p95_ms"] for s in samples) / n
    p99 = sum(s["p99_ms"] for s in samples) / n
    err = sum(s["errors"] for s in samples)
    summary[key] = (qps, p50, p95, p99, err)
    print(f"{side:4s} {workload:8s} {conn:4d} {n:6d} {qps:9.1f} {p50:8.2f} {p95:8.2f} {p99:8.2f} {err:3d}")
print()
print("delta (new vs old, %): QPS  p50   p95   p99")
for workload, conn in [(w, c) for w in ("json", "json16k", "json64k") for c in (64,)] + [("json64k", 1)]:
    if ("old", workload, conn) not in summary or ("new", workload, conn) not in summary:
        continue
    o, n_ = summary[("old", workload, conn)], summary[("new", workload, conn)]
    dq = (n_[0] - o[0]) / o[0] * 100
    dp = tuple((b - a) / a * 100 for a, b in zip(o[1:4], n_[1:4]))
    print(f"{workload:8s} c{conn:<3d} {dq:8.1f}% {dp[0]:7.1f}% {dp[1]:7.1f}% {dp[2]:7.1f}%")
PY
echo "results in $OUT"
