#!/usr/bin/env bash
# bench/compare-four-stacks.sh — capsid+hono vs php-fpm+slim+nginx vs
# Python+FastAPI+uvloop(uvicorn) vs Ruby4+Sinatra+Falcon。
#
# 公平性协议：全部双进程（capsid static-pool --workers 2 / php-fpm pm=static
# max_children=2 / uvicorn --workers 2 / falcon --count 2），全栈常驻一轮，
# loadgen taskset 锁核，栈顺序按 workload 轮转抵消漂移。payload 四栈对齐
# （status/app/pad 结构，pad 大小 1k/16k/64k 相同）。
#
# 使用（capsid-linux-bench 容器内）：
#   PY_TARGET=http://172.17.0.3:8000 RUBY_TARGET=http://172.17.0.4:9292 \
#     bash /capsid/bench/compare-four-stacks.sh
set -euo pipefail
cd /capsid

OUT="bench/results/four-stack-$(date +%Y%m%dT%H%M%S)"
mkdir -p "$OUT"
HOST_BIN=${HOST_BIN:-/capsid/build-linux/capsid-host}
WORKER=${WORKER:-/capsid/build-linux/capsid-worker}
BUNDLE=${BUNDLE:-/tmp/hono-bench-bundle.mjs}
LOADGEN=${LOADGEN:-/capsid/bench/bin/loadgen}
LOADGEN_CORE=${LOADGEN_CORE:-3}
PY_TARGET=${PY_TARGET:?need PY_TARGET (e.g. http://172.17.0.3:8000)}
RUBY_TARGET=${RUBY_TARGET:-}
PHP_TARGET=${PHP_TARGET:-http://php-web:80}
CAPSID_TARGET=http://127.0.0.1:18102
# STACKS 环境变量可限制只跑某几个栈（如 STACKS=capsid 重测被污染的格子）
STACKS=${STACKS:-capsid php python ruby}
# WORKLOADS 环境变量可换负载阶梯（如 1k/4k/16k/32k/64k）
WORKLOADS=${WORKLOADS:-json json16k json64k}

echo "targets: capsid=$CAPSID_TARGET php=$PHP_TARGET python=$PY_TARGET ruby=$RUBY_TARGET"
sha256sum "$HOST_BIN" "$WORKER" "$BUNDLE" "$LOADGEN" | tee "$OUT/identity.sha256"

# --- capsid static-pool 2 workers, 常驻整个跑测 ---
"$HOST_BIN" --mode static-pool --workers 2 \
    --worker "$WORKER" --source-bundle "$BUNDLE" \
    --source-name "file://$BUNDLE" \
    --application orders --listen 127.0.0.1:18102 --routing path \
    --public-scheme http --public-authority public.example \
    --request-timeout-ms 10000 --initial-stream-window 65536 \
    --strict-sandbox off --ready-fd 1 >/dev/null 2>&1 &
HOST_PID=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null "$CAPSID_TARGET/@capsid/orders/fixed" && break
    sleep 0.1
done
echo "capsid host pid=$HOST_PID"

# 其它栈就绪检查（仅限 STACKS 声明的栈；RUBY_TARGET 未设则跳过 ruby）
for pair in "php:$PHP_TARGET" "python:$PY_TARGET" "ruby:$RUBY_TARGET"; do
    name="${pair%%:*}"; tgt="${pair#*:}"
    [[ -n "$tgt" ]] || continue
    case " $STACKS " in *" $name "*) ;; *) continue ;; esac
    if ! curl -s -o /dev/null "$tgt/@capsid/orders/bench/json"; then
        echo "FAIL: $name target $tgt not responding" >&2
        kill "$HOST_PID" 2>/dev/null || true
        exit 1
    fi
done

run_loadgen() {
    local target="$1" workload="$2" side="$3" round="$4" conns="$5"
    local samples="$OUT/samples.$side.$workload.c$conns.r$round.jsonl"
    local corr="$OUT/correctness.$side.$workload.c$conns.r$round.json"
    echo "[$(date +%H:%M:%S)] $side $workload c$conns" >> "$OUT/progress.log"
    # timeout 60 防单轮挂死拖住整个矩阵；失败时记录并继续，汇总可见缺口。
    if ! timeout 60 env CAPSID_BENCH_TARGET="$target" CAPSID_BENCH_WORKLOAD="$workload" \
        CAPSID_BENCH_WARMUP_S=3 CAPSID_BENCH_DURATION_S=8 \
        CAPSID_BENCH_CONNECTIONS="$conns" CAPSID_BENCH_INFLIGHT="$conns" \
        CAPSID_BENCH_SIDE="$side" CAPSID_BENCH_ROUND="$round" \
        CAPSID_BENCH_SAMPLES_OUT="$samples" CAPSID_BENCH_CORRECTNESS_OUT="$corr" \
        taskset -c "$LOADGEN_CORE" "$LOADGEN" >/dev/null 2>"$OUT/loadgen.$side.$workload.c$conns.err"; then
        echo "FAILED: $side $workload c$conns (exit $?)" >> "$OUT/progress.log"
    fi
}

declare -A TARGET=(
    [capsid]="$CAPSID_TARGET"
    [php]="$PHP_TARGET"
    [python]="$PY_TARGET"
    [ruby]="$RUBY_TARGET"
)

# workload 间轮转起始栈，conn 64 先行、1 后行。
read -r -a STACK_ORDER <<< "$STACKS"
workload_i=0
for workload in $WORKLOADS; do
    for conns in 64 1; do
        for offset in $(seq 0 $((${#STACK_ORDER[@]} - 1))); do
            side="${STACK_ORDER[$(((workload_i + offset) % ${#STACK_ORDER[@]}))]}"
            run_loadgen "${TARGET[$side]}" "$workload" "$side" "$workload_i" "$conns"
        done
    done
    workload_i=$((workload_i + 1))
done

kill "$HOST_PID" 2>/dev/null || true
wait "$HOST_PID" 2>/dev/null || true

echo "=== progress ==="
cat "$OUT/progress.log"
python3 /capsid/bench/summarize4.py "$OUT"
echo "results in $OUT"
