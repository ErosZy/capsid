#!/usr/bin/env bash
# QPS comparison lane: capsid (single worker) vs workerd vs
# FastAPI/uvicorn (1 worker) vs Flask/gunicorn (1 worker) vs PHP
# Slim (php-fpm, 1 child) + nginx, all serving the frozen
# /@capsid/orders/bench/* matrix contract with the repository loadgen.
# Every stack runs exactly one serving worker so the comparison is
# process-for-process.
#
# Stack notes:
#   - capsid   : capsid-host --mode single-worker (host + 1 tenant worker)
#   - workerd  : single process (its own model), Module Worker fixture
#   - fastapi  : uvicorn, default single process
#   - flask    : gunicorn -w 1 (sync worker)
#   - php-slim : unprivileged php-fpm (pm=static, 1 child, opcache on)
#     behind unprivileged nginx; the app is copied to a native-FS scratch
#     dir (WSL drvfs stat storms collapse opcache throughput)
#
# Protocol mirrors bench/run-ab.sh: warmup 5s + N rounds x 15s,
# connections=64 inflight=64, servers and client pinned to SUT_CPUSET,
# exact-body correctness verdict every round, serving-path process-tree PSS
# sampled after the last round of each stack.
set -euo pipefail
cd "$(dirname "$0")/.."

SUT_CPUSET=${SUT_CPUSET:-0-3}
OUT=${OUT:?OUT must point to a results dir}
HOST_BIN=${HOST_BIN:?}
WORKER=${WORKER:?}
WORKERD_BIN=${WORKERD_BIN:?}
LOADGEN=${LOADGEN:-bench/bin/loadgen}
VENV_PYTHON=${VENV_PYTHON:-/tmp/opencode/venv/bin/python}
BENCH_APPS=${BENCH_APPS:-$PWD/bench/bench-apps}
QPS_FIXDIR=${QPS_FIXDIR:-/tmp/opencode/qps}
CONNECTIONS=${CONNECTIONS:-64}
INFLIGHT=${INFLIGHT:-64}
WARMUP=${WARMUP:-5}
DURATION=${DURATION:-15}
ROUNDS=${ROUNDS:-3}
WORKLOADS=${WORKLOADS:-matrix-json-1k matrix-json-16k stream}
STACKS=${STACKS:-capsid workerd fastapi flask php}

mkdir -p "$OUT"

# Fail closed when any lane port is already taken: a leftover server from a
# previous session silently taints every measurement on that port.
require_free_port() {
    local p
    for p in "$@"; do
        if (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
            echo "port $p is already in use; refusing to measure against an unknown server" >&2
            exit 1
        fi
    done
}
require_free_port 9102 18993 18994 18996 18997 18998

{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "command: OUT=$OUT CONNECTIONS=$CONNECTIONS INFLIGHT=$INFLIGHT WARMUP=$WARMUP DURATION=$DURATION ROUNDS=$ROUNDS STACKS='$STACKS' bash bench/qps-lane.sh"
    echo "uname: $(uname -a)"
    echo "cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "nproc: $(nproc)"
    echo "python: $($VENV_PYTHON --version 2>&1)"
    echo "python_pkgs: $($VENV_PYTHON -m pip list --format=freeze 2>/dev/null | grep -Ei '^(fastapi|uvicorn|flask|gunicorn|starlette)=' | tr '\n' ' ')"
    echo "php_fpm: $(/usr/sbin/php-fpm8.3 -v 2>/dev/null | head -1)"
    echo "php_pool: static max_children=1, opcache on, validate_timestamps=0"
    echo "nginx: $(nginx -v 2>&1)"
    echo "workerd_version: $("$WORKERD_BIN" --version 2>&1 | head -1)"
    sha256sum "$HOST_BIN" "$WORKER" "$WORKERD_BIN" "$LOADGEN" \
        "$QPS_FIXDIR"/app-workerd.mjs "$QPS_FIXDIR"/workerd-qps.capnp \
        "$BENCH_APPS"/fastapi_app.py "$BENCH_APPS"/flask_app.py \
        "$BENCH_APPS"/php-slim/public/index.php \
        "$BENCH_APPS"/php-slim/composer.json
} > "$OUT/manifest.txt"

pss_mb_of() { # pid -> MB (one decimal), empty when unreadable
    local kb
    kb=$(awk '$1=="Pss:" {print $2; exit}' "/proc/$1/smaps_rollup" 2>/dev/null)
    [ -n "${kb:-}" ] && awk -v k="$kb" 'BEGIN{printf "%.1f", k/1024}'
}

# usage: breakdown_pss "role:pid[:childrole]"...
# Emits role=mb pairs ("; " separated). When childrole is given, direct
# children of pid are appended with that role — this is how capsid
# (host -> workers), php (nginx/fpm masters -> children) and the python
# masters (-> workers) are decomposed.
breakdown_pss() {
    local spec out="" role pid childrole kb c
    for spec in "$@"; do
        IFS=':' read -r role pid childrole <<< "$spec"
        kb=$(pss_mb_of "$pid")
        [ -n "$kb" ] && out="${out:+$out;}${role}=${kb}"
        if [ -n "${childrole:-}" ]; then
            for c in $(pgrep -P "$pid" 2>/dev/null); do
                kb=$(pss_mb_of "$c")
                [ -n "$kb" ] && out="${out:+$out;}${childrole}=${kb}"
            done
        fi
    done
    echo "$out"
}

tree_pss_mb() {
    # Root process plus its direct children only. Never use the process
    # group: every server started by this script shares the script's pgid,
    # which would sum unrelated stacks into one number.
    local root=$1 total=0 pid kb
    for pid in $( { echo "$root"; pgrep -P "$root" 2>/dev/null; } | sort -un ); do
        kb=$(awk '$1=="Pss:" {print $2; exit}' "/proc/$pid/smaps_rollup" 2>/dev/null)
        [ -n "${kb:-}" ] && total=$((total + kb))
    done
    awk -v k="$total" 'BEGIN{printf "%.1f", k/1024}'
}

measure_stack() { # name "pid..." port ["breakdown specs"]
    local name=$1 pidlist=$2 port=$3 breakdown=${4:-} workload round qps p50 p95 vals=()
    # --- low-load phase: 10 req/s trickle for 15 s on the FRESH process,
    # sampled before the high-load rounds so grown heaps cannot leak back
    # into this number (V8 never returns memory once grown).
    if [ "${LOWQPS:-1}" = "1" ]; then
        local trickle_url="http://127.0.0.1:$port/@capsid/orders/bench/matrix-json-1k"
        curl -s -o /dev/null --max-time 3 "$trickle_url" || true
        local i
        for i in $(seq 1 150); do
            curl -s -o /dev/null --max-time 2 "$trickle_url" || true
            sleep 0.1
        done
        local lp=0 q
        for q in $pidlist; do
            lp=$(awk -v a="$lp" -v b="$(tree_pss_mb "$q")" 'BEGIN{printf "%.1f", a+b}')
        done
        printf '%s %-16s lowqps_pss_mb=%s\n' "$name" "" "$lp" \
            | tee -a "$OUT/cells.txt"
        if [ -n "$breakdown" ]; then
            # shellcheck disable=SC2086
            printf '%s %-16s lowqps_pss_detail=%s\n' "$name" "" \
                "$(breakdown_pss $breakdown)" | tee -a "$OUT/cells.txt"
        fi
    fi
    for workload in $WORKLOADS; do
        vals=()
        for round in $(seq 1 "$ROUNDS"); do
            local sample="raw.$name.$workload.r$round.jsonl"
            local verdict="correctness.$name.$workload.r$round.json"
            CAPSID_BENCH_TARGET="http://127.0.0.1:$port" \
            CAPSID_BENCH_WORKLOAD="$workload" \
            CAPSID_BENCH_SIDE="$name" \
            CAPSID_BENCH_ROUND="$round" \
            CAPSID_BENCH_WARMUP_S="$WARMUP" \
            CAPSID_BENCH_DURATION_S="$DURATION" \
            CAPSID_BENCH_CONNECTIONS="$CONNECTIONS" \
            CAPSID_BENCH_INFLIGHT="$INFLIGHT" \
            CAPSID_BENCH_SAMPLES_OUT="$OUT/$sample" \
            CAPSID_BENCH_CORRECTNESS_OUT="$OUT/$verdict" \
                taskset -c "$SUT_CPUSET" "$LOADGEN" >/dev/null 2>&1
            grep -q '"ok"[[:space:]]*:[[:space:]]*true' "$OUT/$verdict" || {
                echo "$name $workload round $round correctness FAILED" >&2
                return 1
            }
            qps=$(grep '"phase":"measured"' "$OUT/$sample" |
                grep -o '"qps":[0-9.]*' | cut -d: -f2)
            [ -n "$qps" ] || { echo "$name $workload round $round no sample" >&2; return 1; }
            vals+=("$qps")
        done
        local med
        med=$(printf '%s\n' "${vals[@]}" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
        local last="raw.$name.$workload.r$ROUNDS.jsonl"
        p50=$(grep '"phase":"measured"' "$OUT/$last" |
            grep -o '"p50_ms":[0-9.]*' | cut -d: -f2)
        p95=$(grep '"phase":"measured"' "$OUT/$last" |
            grep -o '"p95_ms":[0-9.]*' | cut -d: -f2)
        printf '%s %-16s qps_med=%7.1f p50_ms=%s p95_ms=%s\n' \
            "$name" "$workload" "$med" "${p50:-n/a}" "${p95:-n/a}" \
            | tee -a "$OUT/cells.txt"
    done
    local pss_total=0 p
    for p in $pidlist; do
        pss_total=$(awk -v a="$pss_total" -v b="$(tree_pss_mb "$p")" 'BEGIN{printf "%.1f", a+b}')
    done
    printf '%s %-16s serving_pss_mb=%s\n' "$name" "" "$pss_total" \
        | tee -a "$OUT/cells.txt"
    if [ -n "$breakdown" ]; then
        # shellcheck disable=SC2086
        printf '%s %-16s serving_pss_detail=%s\n' "$name" "" \
            "$(breakdown_pss $breakdown)" | tee -a "$OUT/cells.txt"
    fi
}

wait_port() { # pid port [tries]
    local pid=$1 port=$2 tries=${3:-400} i
    for i in $(seq 1 "$tries"); do
        (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    return 1
}

PIDS=()
cleanup() {
    if stack_enabled php && [ -f "$OUT/php-run/nginx.pid" ]; then
        nginx -s stop -c "$OUT/nginx.conf" 2>/dev/null || true
    fi
    local p c
    for p in "${PIDS[@]}"; do
        # TERM the master; uvicorn/gunicorn/php-fpm masters reap their own
        # workers. Direct children are killed as a fallback for stacks
        # whose master does not reap (nginx workers, capsid workers).
        for c in $(pgrep -P "$p" 2>/dev/null); do
            kill "$c" 2>/dev/null || true
        done
        kill "$p" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT

stack_enabled() {
    case " $STACKS " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

# --- capsid: static-pool, 2 workers ---
if stack_enabled capsid; then
    : >"$OUT/ready.capsid"
    taskset -c "$SUT_CPUSET" "$HOST_BIN" --mode single-worker \
        --worker "$WORKER" --source-bundle "$QPS_FIXDIR/app-capsid.mjs" \
        --source-name "file://$QPS_FIXDIR/app-capsid.mjs" \
        --application orders --listen 127.0.0.1:18993 --routing path \
        --public-scheme http --public-authority bench.invalid \
        --request-timeout-ms "${REQUEST_TIMEOUT_MS:-10000}" \
        --initial-stream-window "${STREAM_WINDOW:-1024}" \
        --strict-sandbox off --ready-fd 3 \
        3>>"$OUT/ready.capsid" >"$OUT/err.capsid" 2>&1 &
    CAPSID_PID=$!
    PIDS+=("$CAPSID_PID")
    wait_port "$CAPSID_PID" 18993 400 || { echo "capsid did not become ready" >&2; exit 1; }
fi

# --- workerd ---
if stack_enabled workerd; then
    taskset -c "$SUT_CPUSET" "$WORKERD_BIN" serve "$QPS_FIXDIR/workerd-qps.capnp" \
        >"$OUT/err.workerd" 2>&1 &
    WORKERD_PID=$!
    PIDS+=("$WORKERD_PID")
    wait_port "$WORKERD_PID" 18994 400 || { echo "workerd did not become ready" >&2; exit 1; }
fi

# --- fastapi (uvicorn, one async worker - parity with flask's -w 1) ---
# --no-access-log: gunicorn logs no per-request lines either; without this
# uvicorn's default INFO access log (hundreds of MB when redirected onto a
# drvfs mount) drags the event loop down several-fold.
if stack_enabled fastapi; then
    (cd "$BENCH_APPS" && taskset -c "$SUT_CPUSET" \
        "$(dirname "$VENV_PYTHON")/uvicorn" fastapi_app:app \
        --workers 1 --no-access-log --host 127.0.0.1 --port 18997 \
        >"$OUT/err.fastapi" 2>&1 & echo $! >"$OUT/.uvicorn.pid")
    FASTAPI_PID=$(cat "$OUT/.uvicorn.pid")
    PIDS+=("$FASTAPI_PID")
    wait_port "$FASTAPI_PID" 18997 400 || { echo "fastapi did not become ready" >&2; exit 1; }
fi

# --- flask (gunicorn, one sync process) ---
# sync is the single-process optimum: gthread measured slower (GIL
# contention beats the keep-alive it would add; the sync worker closes
# each connection after responding).
if stack_enabled flask; then
    (cd "$BENCH_APPS" && taskset -c "$SUT_CPUSET" \
        "$(dirname "$VENV_PYTHON")/gunicorn" -w 1 -b 127.0.0.1:18998 \
        flask_app:app >"$OUT/err.flask" 2>&1 & echo $! >"$OUT/.gunicorn.pid")
    FLASK_PID=$(cat "$OUT/.gunicorn.pid")
    PIDS+=("$FLASK_PID")
    wait_port "$FLASK_PID" 18998 400 || { echo "flask did not become ready" >&2; exit 1; }
fi

# --- php-slim (one php-fpm static child behind unprivileged nginx) ---
# The app tree is copied to a native-FS scratch dir first: on WSL/drvfs
# mounts opcache revalidation stats every vendor file per request and
# collapses throughput. opcache.validate_timestamps=0 keeps the pool hot.
if stack_enabled php; then
    PHP_APP="$OUT/php-app"
    rm -rf "$PHP_APP"
    cp -r "$BENCH_APPS/php-slim" "$PHP_APP"
    mkdir -p "$OUT/php-run"
    cat >"$OUT/php-fpm.conf" <<PHPFPM
[global]
error_log = $OUT/php-run/fpm-error.log
pid = $OUT/php-run/fpm.pid
daemonize = no
[www]
listen = 127.0.0.1:9102
pm = static
pm.max_children = 1
user = $USER
group = $(id -gn)
access.log = $OUT/php-run/access.log
php_admin_value[opcache.enable] = 1
php_admin_value[opcache.enable_cli] = 1
php_admin_value[opcache.validate_timestamps] = 0
php_admin_value[opcache.jit] = tracing
php_admin_value[opcache.jit_buffer_size] = 32M
php_admin_value[error_log] = $OUT/php-run/php-error.log
PHPFPM
    cat >"$OUT/nginx.conf" <<NGINX
worker_processes 1;
error_log $OUT/php-run/nginx-error.log;
pid $OUT/php-run/nginx.pid;
events { worker_connections 4096; }
http {
    access_log off;
    client_body_temp_path $OUT/php-run/body;
    proxy_temp_path $OUT/php-run/proxy;
    fastcgi_temp_path $OUT/php-run/fastcgi;
    uwsgi_temp_path $OUT/php-run/uwsgi;
    scgi_temp_path $OUT/php-run/scgi;
    server {
        listen 127.0.0.1:18996;
        root $PHP_APP/public;
        location / {
            fastcgi_keep_conn on;
            fastcgi_pass 127.0.0.1:9102;
            fastcgi_param SCRIPT_FILENAME $PHP_APP/public/index.php;
            fastcgi_param SCRIPT_NAME /index.php;
            include /etc/nginx/fastcgi_params;
        }
    }
}
NGINX
    taskset -c "$SUT_CPUSET" /usr/sbin/php-fpm8.3 -F -y "$OUT/php-fpm.conf" \
        >"$OUT/err.php" 2>&1 &
    PHP_FPM_PID=$!
    PIDS+=("$PHP_FPM_PID")
    for _ in $(seq 1 100); do
        (exec 3<>/dev/tcp/127.0.0.1/9102) 2>/dev/null && break
        kill -0 "$PHP_FPM_PID" 2>/dev/null || break
        sleep 0.05
    done
    kill -0 "$PHP_FPM_PID" 2>/dev/null || {
        echo "php-fpm failed to start (see $OUT/err.php)" >&2; exit 1; }
    nginx -c "$OUT/nginx.conf" 2>>"$OUT/err.php"
    PHP_NGINX_PID=$(cat "$OUT/php-run/nginx.pid")
    PIDS+=("$PHP_NGINX_PID")
    wait_port "$PHP_NGINX_PID" 18996 400 || { echo "php did not become ready" >&2; exit 1; }
    PHP_PID="$PHP_FPM_PID $PHP_NGINX_PID"
fi

for stack in $STACKS; do
    case "$stack" in
        capsid)  measure_stack capsid "$CAPSID_PID" 18993 "host:$CAPSID_PID:worker" ;;
        workerd) measure_stack workerd "$WORKERD_PID" 18994 "process:$WORKERD_PID" ;;
        fastapi) measure_stack fastapi "$FASTAPI_PID" 18997 "master:$FASTAPI_PID:worker" ;;
        flask)   measure_stack flask "$FLASK_PID" 18998 "master:$FLASK_PID:worker" ;;
        php)     measure_stack php "$PHP_PID" 18996 "nginx:$PHP_NGINX_PID:nginx-worker fpm-master:$PHP_FPM_PID:fpm-worker" ;;
    esac
done

find "$OUT" -maxdepth 1 -type f ! -name sha256sums.txt -print0 |
    sort -z | xargs -0 sha256sum > "$OUT/sha256sums.txt"
echo "results in $OUT"
