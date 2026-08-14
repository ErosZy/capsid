#!/usr/bin/env bash
# bench/compare-three-stacks.sh — capsid+hono vs php-fpm+nginx+slim vs
# python+flask+gunicorn, workload matrix 1k/8k/16k/32k x json/bytes/stream.
#
# 公平性协议（见 docs/performance-benchmarks.md 的 bench 约定）：
#   双进程（capsid static-pool --workers 2 / php-fpm pm=static max_children=2
#   / gunicorn --workers 2）；被测栈 taskset 0-5，loadgen taskset 6-7；
#   全栈常驻一轮；workload 间轮转起始栈抵消漂移；每格 3 轮
#   （warmup 3s + measured 8s, conns=64）；correctness 逐轮校验。
#
# 前置：php 容器 capsid-php-bench 已起（端口 8080 发布）、
#   /tmp/hono-bench-bundle.mjs 已构建、venv /tmp/capsid-bench-venv 已装。
# 用法：bash bench/compare-three-stacks.sh [WORKLOADS]
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="bench/results/three-stack-$(date +%Y%m%dT%H%M%S)"
mkdir -p "$OUT"
HOST_BIN=${HOST_BIN:-build-m1d/capsid-host}
WORKER=${WORKER:-build-m1d/capsid-worker}
BUNDLE=${BUNDLE:-/tmp/hono-bench-bundle.mjs}
LOADGEN=${LOADGEN:-bench/bin/loadgen}
SUT_CPUSET=${SUT_CPUSET:-0-5}
LOADGEN_CPUSET=${LOADGEN_CPUSET:-6-7}
ROUNDS=${ROUNDS:-3}
CONNS=${CONNS:-64}
CAPSID_WORKERS=${CAPSID_WORKERS:-2}
GUNI_WORKERS=${GUNI_WORKERS:-2}
CAPSID_WINDOW=${CAPSID_WINDOW:-16384}
WORKLOADS=${WORKLOADS:-json json8k json16k json32k bytes bytes8k bytes16k bytes32k stream stream8k stream16k stream32k}

CAPSID_TARGET=http://127.0.0.1:18102
PHP_TARGET=http://127.0.0.1:8080
PY_TARGET=http://127.0.0.1:8000

echo "targets: capsid=$CAPSID_TARGET php=$PHP_TARGET python=$PY_TARGET"
{
    echo "workloads: $WORKLOADS"
    echo "rounds: $ROUNDS conns: $CONNS warmup: 3s measured: 8s"
    echo "sut_cpuset: $SUT_CPUSET loadgen_cpuset: $LOADGEN_CPUSET"
    echo "capsid_workers: $CAPSID_WORKERS gunicorn_workers: $GUNI_WORKERS"
    echo "capsid_initial_stream_window: $CAPSID_WINDOW"
    sha256sum "$HOST_BIN" "$WORKER" "$BUNDLE" "$LOADGEN"
    echo "php: $(docker exec capsid-php-bench php -v 2>/dev/null | head -1)"
    echo "nginx: $(docker exec capsid-php-bench nginx -v 2>&1)"
    echo "slim: $(docker exec capsid-php-bench bash -c 'cd /app && composer show slim/slim --no-ansi 2>/dev/null | grep versions | head -1')"
    echo "python: $(/tmp/capsid-bench-venv/bin/python -c 'import sys; print(sys.version.split()[0])')"
    echo "flask: $(/tmp/capsid-bench-venv/bin/pip show flask 2>/dev/null | grep Version)"
    echo "gunicorn: $(/tmp/capsid-bench-venv/bin/pip show gunicorn 2>/dev/null | grep Version)"
} | tee "$OUT/manifest.txt"

# --- capsid static-pool 2 workers, 常驻 ---
taskset -c "$SUT_CPUSET" "$HOST_BIN" --mode static-pool --workers "$CAPSID_WORKERS" \
    --worker "$WORKER" --source-bundle "$BUNDLE" \
    --source-name "file://$BUNDLE" \
    --application orders --listen 127.0.0.1:18102 --routing path \
    --public-scheme http --public-authority public.example \
    --request-timeout-ms 10000 --initial-stream-window "$CAPSID_WINDOW" \
    --strict-sandbox off --ready-fd 1 >/dev/null 2>"$OUT/capsid-host.log" &
HOST_PID=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null -m 2 "$CAPSID_TARGET/@capsid/orders/fixed" && break
    sleep 0.2
done
echo "capsid host pid=$HOST_PID" | tee -a "$OUT/manifest.txt"

# --- gunicorn 2 workers, 常驻 ---
taskset -c "$SUT_CPUSET" /tmp/capsid-bench-venv/bin/gunicorn --workers "$GUNI_WORKERS" \
    --bind 127.0.0.1:8000 flask_app:app \
    --chdir bench/bench-apps >"$OUT/gunicorn.log" 2>&1 &
GUNI_PID=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null -m 2 "$PY_TARGET/@capsid/orders/fixed" && break
    sleep 0.2
done
echo "gunicorn pid=$GUNI_PID" | tee -a "$OUT/manifest.txt"

# --- php 容器就绪检查（容器常驻，此处只验活） ---
for _ in $(seq 1 30); do
    curl -s -o /dev/null -m 2 "$PHP_TARGET/@capsid/orders/fixed" && break
    sleep 0.2
done

declare -A TARGET=(
    [capsid]="$CAPSID_TARGET"
    [php]="$PHP_TARGET"
    [python]="$PY_TARGET"
)
read -r -a STACK_ORDER <<< "capsid php python"

run_loadgen() {
    local target="$1" workload="$2" side="$3" round="$4" conns="$5"
    local samples="$OUT/samples.$side.$workload.c$conns.r$round.jsonl"
    local corr="$OUT/correctness.$side.$workload.c$conns.r$round.json"
    echo "[$(date +%H:%M:%S)] $side $workload c$conns r$round" >>"$OUT/progress.log"
    if ! timeout 60 env CAPSID_BENCH_TARGET="$target" CAPSID_BENCH_WORKLOAD="$workload" \
        CAPSID_BENCH_WARMUP_S=3 CAPSID_BENCH_DURATION_S=8 \
        CAPSID_BENCH_CONNECTIONS="$conns" CAPSID_BENCH_INFLIGHT="$conns" \
        CAPSID_BENCH_SIDE="$side" CAPSID_BENCH_ROUND="$round" \
        CAPSID_BENCH_SAMPLES_OUT="$samples" CAPSID_BENCH_CORRECTNESS_OUT="$corr" \
        taskset -c "$LOADGEN_CPUSET" "$LOADGEN" >/dev/null 2>"$OUT/loadgen.$side.$workload.err"; then
        echo "FAILED: $side $workload c$conns r$round (exit $?)" >>"$OUT/progress.log"
    fi
}

workload_i=0
for workload in $WORKLOADS; do
    for round in $(seq 1 "$ROUNDS"); do
        for offset in 0 1 2; do
            side="${STACK_ORDER[$(((workload_i + offset) % 3))]}"
            run_loadgen "${TARGET[$side]}" "$workload" "$side" "$round" "$CONNS"
        done
    done
    workload_i=$((workload_i + 1))
done

kill "$HOST_PID" "$GUNI_PID" 2>/dev/null || true
wait "$HOST_PID" "$GUNI_PID" 2>/dev/null || true

echo "=== progress ===" | tee -a "$OUT/progress.log"
python3 bench/summarize4.py "$OUT" | tee "$OUT/summary.txt"
echo "results in $OUT"
