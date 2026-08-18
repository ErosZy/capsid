#!/usr/bin/env bash
# bench/compare-four-qps.sh — capsid+hono vs php-fpm+nginx+slim vs
# python+flask+gunicorn vs python+fastapi+uvicorn, workload matrix
# 1k/4k/8k/16k/32k/64k x json/bytes/stream, plus per-process resource
# sampling (capsid host/worker listed separately).
#
# 公平性协议（同 docs/performance-benchmarks.md 的 bench 约定）：
#   双进程（capsid static-pool --workers 2 / php-fpm pm=static max_children=2
#   / gunicorn --workers 2 / uvicorn --workers 2）；被测栈 taskset 0-3，
#   loadgen taskset 4-7；全栈常驻一轮；workload 间轮转起始栈抵消漂移；
#   每格 3 轮（warmup 3s + measured 8s, conns=64）；correctness 逐轮校验。
#
# 前置：php 容器 capsid-php-bench 已起（8080）、/tmp/hono-bench-bundle.mjs
#   已构建、venv /tmp/capsid-bench-venv 已装（flask/gunicorn/fastapi/uvicorn）。
# 用法：bash bench/compare-four-qps.sh [WORKLOADS]
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="bench/results/four-qps-$(date +%Y%m%dT%H%M%S)"
mkdir -p "$OUT"
HOST_BIN=${HOST_BIN:-build-m1d/capsid-host}
WORKER=${WORKER:-build-m1d/capsid-worker}
BUNDLE=${BUNDLE:-/tmp/hono-bench-bundle.mjs}
LOADGEN=${LOADGEN:-bench/bin/loadgen}
SUT_CPUSET=${SUT_CPUSET:-0-3}
LOADGEN_CPUSET=${LOADGEN_CPUSET:-4-7}
ROUNDS=${ROUNDS:-3}
CONNS=${CONNS:-64}
CAPSID_WORKERS=${CAPSID_WORKERS:-2}
CAPSID_WINDOW=${CAPSID_WINDOW:-16384}
RESAMPLE=${RESAMPLE:-5}
WORKLOADS=${WORKLOADS:-json matrix-json-4k json16k json32k bytes matrix-bytes-4k bytes16k bytes32k stream matrix-stream-4k stream16k stream32k}
# 完整阶梯见 compare-three-stacks.sh；默认走 1k/4k/16k/32k 四尺寸省时间，
# 需要全 6 尺寸时覆盖 WORKLOADS。4k 档必须用 matrix-* 前缀（loadgen 命名）。

CAPSID_TARGET=http://127.0.0.1:18102
PHP_TARGET=http://127.0.0.1:8080
PY_TARGET=http://127.0.0.1:8000
FASTAPI_TARGET=http://127.0.0.1:8001

echo "targets: capsid=$CAPSID_TARGET php=$PHP_TARGET flask=$PY_TARGET fastapi=$FASTAPI_TARGET"
{
    echo "workloads: $WORKLOADS"
    echo "rounds: $ROUNDS conns: $CONNS warmup: 3s measured: 8s"
    echo "sut_cpuset: $SUT_CPUSET loadgen_cpuset: $LOADGEN_CPUSET"
    echo "capsid_workers: $CAPSID_WORKERS"
    echo "capsid_initial_stream_window: $CAPSID_WINDOW"
    sha256sum "$HOST_BIN" "$WORKER" "$BUNDLE" "$LOADGEN"
    echo "php: $(docker exec capsid-php-bench php -v 2>/dev/null | head -1)"
    echo "nginx: $(docker exec capsid-php-bench nginx -v 2>&1)"
    echo "slim: $(docker exec capsid-php-bench bash -c 'cd /app && composer show slim/slim --no-ansi 2>/dev/null | grep versions | head -1')"
    echo "python: $(/tmp/capsid-bench-venv/bin/python -c 'import sys; print(sys.version.split()[0])')"
    echo "flask: $(/tmp/capsid-bench-venv/bin/pip show flask 2>/dev/null | grep Version)"
    echo "gunicorn: $(/tmp/capsid-bench-venv/bin/pip show gunicorn 2>/dev/null | grep Version)"
    echo "fastapi: $(/tmp/capsid-bench-venv/bin/pip show fastapi 2>/dev/null | grep Version)"
    echo "uvicorn: $(/tmp/capsid-bench-venv/bin/pip show uvicorn 2>/dev/null | grep Version)"
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
taskset -c "$SUT_CPUSET" /tmp/capsid-bench-venv/bin/gunicorn --workers 2 \
    --bind 127.0.0.1:8000 flask_app:app \
    --chdir bench/bench-apps >"$OUT/gunicorn.log" 2>&1 &
GUNI_PID=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null -m 2 "$PY_TARGET/@capsid/orders/fixed" && break
    sleep 0.2
done
echo "gunicorn pid=$GUNI_PID" | tee -a "$OUT/manifest.txt"

# --- uvicorn 2 workers (uvloop+httptools), 常驻 ---
taskset -c "$SUT_CPUSET" /tmp/capsid-bench-venv/bin/uvicorn --workers 2 \
    --host 127.0.0.1 --port 8001 fastapi_app:app \
    --app-dir bench/bench-apps >"$OUT/uvicorn.log" 2>&1 &
UVI_PID=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null -m 2 "$FASTAPI_TARGET/@capsid/orders/fixed" && break
    sleep 0.2
done
echo "uvicorn pid=$UVI_PID" | tee -a "$OUT/manifest.txt"

# --- php 容器就绪检查（容器常驻，此处只验活） ---
for _ in $(seq 1 30); do
    curl -s -o /dev/null -m 2 "$PHP_TARGET/@capsid/orders/fixed" && break
    sleep 0.2
done

# --- 每进程资源采样（5s 窗口，host/worker 分开） ---
python3 bench/sample-resources.py --out "$OUT/resources.jsonl" \
    --interval "$RESAMPLE" --pid "$HOST_PID" >/dev/null 2>"$OUT/resources.err" &
SAMPLE_PID=$!

declare -A TARGET=(
    [capsid]="$CAPSID_TARGET"
    [php]="$PHP_TARGET"
    [flask]="$PY_TARGET"
    [fastapi]="$FASTAPI_TARGET"
)
# STACKS 环境变量可限制只跑某几个栈（如 STACKS=fastapi 重测修复后的栈）
read -r -a STACK_ORDER <<< "${STACKS:-capsid php flask fastapi}"

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
NSTACKS=${#STACK_ORDER[@]}
for workload in $WORKLOADS; do
    for round in $(seq 1 "$ROUNDS"); do
        for offset in $(seq 0 $((NSTACKS - 1))); do
            side="${STACK_ORDER[$(((workload_i + round - 1 + offset) % NSTACKS))]}"
            run_loadgen "${TARGET[$side]}" "$workload" "$side" "$round" "$CONNS"
        done
    done
    echo "[$(date +%H:%M:%S)] workload $workload done (4 stacks x $ROUNDS rounds)" >>"$OUT/progress.log"
    workload_i=$((workload_i + 1))
done

kill "$SAMPLE_PID" 2>/dev/null || true
kill "$HOST_PID" "$GUNI_PID" "$UVI_PID" 2>/dev/null || true
wait "$HOST_PID" "$GUNI_PID" "$UVI_PID" 2>/dev/null || true

echo "=== progress ===" | tee -a "$OUT/progress.log"
python3 bench/summarize-four-qps.py "$OUT" 2>/dev/null | tee "$OUT/summary.txt" || \
    python3 bench/summarize4.py "$OUT" | tee "$OUT/summary.txt"
echo "results in $OUT"
