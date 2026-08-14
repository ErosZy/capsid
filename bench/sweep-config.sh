#!/usr/bin/env bash
# bench/sweep-config.sh — one-round QPS probe per stack configuration.
# Usage: bash bench/sweep-config.sh STACK CONFIG_LABEL CONFIG_ARGS...
#   capsid  N   -> starts static-pool --workers N, probes, tears down
#   python  N   -> starts gunicorn --workers N, probes, tears down
#   php     N   -> restarts fpm with max_children N (container), probes
# Probe: json16k, conns=128, warmup 3s + measured 8s, loadgen 6-7.
set -euo pipefail
cd "$(dirname "$0")/.."

STACK="$1"; N="$2"
HOST_BIN=${HOST_BIN:-build-m1d/capsid-host}
WORKER=${WORKER:-build-m1d/capsid-worker}
BUNDLE=${BUNDLE:-/tmp/hono-bench-bundle.mjs}
LOADGEN=${LOADGEN:-bench/bin/loadgen}
SUT=${SUT_CPUSET:-0-5}
LG=${LOADGEN_CPUSET:-6-7}
TARGET=""; PID=""

case "$STACK" in
capsid)
    TARGET=http://127.0.0.1:18102
    taskset -c "$SUT" "$HOST_BIN" --mode static-pool --workers "$N" \
        --worker "$WORKER" --source-bundle "$BUNDLE" \
        --source-name "file://$BUNDLE" \
        --application orders --listen 127.0.0.1:18102 --routing path \
        --public-scheme http --public-authority public.example \
        --request-timeout-ms 10000 --initial-stream-window 16384 \
        --strict-sandbox off --ready-fd 1 >/dev/null 2>&1 &
    PID=$!
    ;;
python)
    TARGET=http://127.0.0.1:8000
    taskset -c "$SUT" /tmp/capsid-bench-venv/bin/gunicorn --workers "$N" \
        --bind 127.0.0.1:8000 flask_app:app \
        --chdir bench/bench-apps >/dev/null 2>&1 &
    PID=$!
    ;;
php)
    TARGET=http://127.0.0.1:8080
    sed "s/pm.max_children = .*/pm.max_children = $N/" \
        bench/php-fpm-bench.conf > /tmp/fpm-sweep.conf
    docker cp /tmp/fpm-sweep.conf capsid-php-bench:/usr/local/etc/php-fpm.d/www.conf
    docker exec capsid-php-bench bash -c 'for p in /proc/[0-9]*/cmdline; do tr "\0" " " < "$p" 2>/dev/null | grep -q "^php-fpm: master" || continue; pid="${p#/proc/}"; pid="${pid%/cmdline}"; kill "$pid" 2>/dev/null || true; done'
    sleep 1
    docker exec -d capsid-php-bench php-fpm
    ;;
*) echo "unknown stack $STACK" >&2; exit 2 ;;
esac

for _ in $(seq 1 60); do
    curl -s -o /dev/null -m 2 "$TARGET/@capsid/orders/fixed" && break
    sleep 0.2
done

env CAPSID_BENCH_TARGET="$TARGET" CAPSID_BENCH_WORKLOAD=json16k \
    CAPSID_BENCH_WARMUP_S=3 CAPSID_BENCH_DURATION_S=8 \
    CAPSID_BENCH_CONNECTIONS=128 CAPSID_BENCH_INFLIGHT=128 \
    CAPSID_BENCH_SIDE="$STACK" CAPSID_BENCH_ROUND=1 \
    CAPSID_BENCH_SAMPLES_OUT="/tmp/sweep.$STACK.$N.jsonl" \
    CAPSID_BENCH_CORRECTNESS_OUT="/tmp/sweep.$STACK.$N.json" \
    taskset -c "$LG" "$LOADGEN" >/dev/null 2>&1
QPS=$(tail -1 "/tmp/sweep.$STACK.$N.jsonl" | sed -n 's/.*"qps":\([0-9.]*\).*/\1/p')
echo "$STACK $N -> QPS=$QPS"

if [ -n "$PID" ]; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
fi
