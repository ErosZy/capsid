#!/usr/bin/env bash
# Density lane: grow a real serving pool — capsid static-pool workers or
# workerd cells — until the process tree's PSS reaches the memory budget,
# then report the largest count that fit. Per (stack, size): double the pool
# until over budget, binary-search the boundary, and record the count and
# the PSS at that count. Every worker/cell is touched with real requests
# before sampling (workerd materializes isolates lazily on first request).
#
# Output: OUT/cells.txt with one line per (stack, size):
#   capsid  <size>  workers=<N> pss_mb=<X> spans/budget/limit notes
#   workerd <size>  cells=<N>   pss_mb=<X>
# plus OUT/probes.jsonl (every probe), OUT/manifest.txt, sha256sums.txt.
#
# Usage: OUT=... HOST_BIN=... WORKER=... WORKERD_BIN=... \
#        bash bench/density-lane.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BUDGET_MB=${BUDGET_MB:-2048}
SIZES=${SIZES:-"10k 100k 1m"}
STACKS=${STACKS:-"capsid workerd"}
APPS=${APPS:-/tmp/cold-start-apps}
HOST_BIN=${HOST_BIN:?}
WORKER=${WORKER:?}
WORKERD_BIN=${WORKERD_BIN:?}
OUT=${OUT:?OUT must point to a results dir}
SUT_CPUSET=${SUT_CPUSET:-0-3}
MAX_WORKERS=${MAX_WORKERS:-4096}   # capsid static-pool hard limit
MAX_CELLS=${MAX_CELLS:-1024}       # probe cap for workerd cells
CURL_PAR=${CURL_PAR:-64}
mkdir -p "$OUT"
: >"$OUT/cells.txt"
: >"$OUT/probes.jsonl"

{
    echo "rounds: density fill-to-budget budget_mb=$BUDGET_MB sut_cpuset: $SUT_CPUSET"
    echo "generated_at: $(date +%Y-%m-%dT%H:%M:%S%:z)"
    echo "commit: $(git rev-parse HEAD 2>/dev/null || echo '')"
    echo "tag: $(git describe --exact-match --tags HEAD 2>/dev/null || echo '')"
    echo "command: OUT=$OUT BUDGET_MB=$BUDGET_MB SIZES='$SIZES' STACKS='$STACKS' bash bench/density-lane.sh"
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')"
    echo "uname: $(uname -r)"
    echo "nproc: $(nproc)"
    echo "host_sha256: $(sha256sum "$HOST_BIN" | cut -d' ' -f1)"
    echo "worker_sha256: $(sha256sum "$WORKER" | cut -d' ' -f1)"
    echo "workerd_version: $("$WORKERD_BIN" --version 2>&1 | head -1)"
    echo "workerd_sha256: $(sha256sum "$WORKERD_BIN" | cut -d' ' -f1)"
} >"$OUT/manifest.txt"

PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do
        [ -n "$p" ] || continue
        for c in $(pgrep -P "$p" 2>/dev/null); do kill -9 "$c" 2>/dev/null || true; done
        kill -9 "$p" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT

probe_index=0
next_port() { probe_index=$((probe_index + 1)); echo $((19400 + probe_index)); }

tree_pss_mb() { # $1 = parent pid
    local pids="$1"
    local kids
    kids=$(pgrep -P "$1" 2>/dev/null | tr '\n' ' ')
    pids="$pids $kids"
    local files=""
    for p in $pids; do
        [ -r "/proc/$p/smaps_rollup" ] && files="$files /proc/$p/smaps_rollup"
    done
    [ -n "$files" ] || { echo 0; return; }
    awk '/^Pss:/{s+=$2} END{printf "%.1f", (s+0)/1024}' $files
}

touch_pool() { # $1 = url-list file; one curl per URL so every response is
    # discarded (-o binds to the first URL only when curl gets a list)
    xargs -a "$1" -P "$CURL_PAR" -I{} \
        curl -s -o /dev/null --max-time 5 {} || true
}

probe_capsid() { # $1 size  $2 workers  -> pss_mb or "fail"
    local size=$1 n=$2
    local port=$(next_port)
    local ready="$OUT/ready.$size.$n"
    : >"$ready"
    taskset -c "$SUT_CPUSET" "$HOST_BIN" --mode static-pool --workers "$n" \
        --worker "$WORKER" --source-bundle "$APPS/app-capsid-$size.mjs" \
        --source-name "file://$APPS/app-capsid-$size.mjs" \
        --application orders --listen 127.0.0.1:$port --routing path \
        --public-scheme http --public-authority bench.invalid \
        --request-timeout-ms 10000 --strict-sandbox off --ready-fd 3 \
        3>>"$ready" >"$OUT/err.capsid.$size.$n" 2>&1 &
    local hpid=$!
    PIDS+=("$hpid")
    local up=0
    for _ in $(seq 1 2400); do
        grep -q 'capsid-host-ready-v1' "$ready" 2>/dev/null && { up=1; break; }
        kill -0 "$hpid" 2>/dev/null || break
        sleep 0.05
    done
    if [ "$up" != 1 ]; then
        for c in $(pgrep -P "$hpid" 2>/dev/null); do kill -9 "$c" 2>/dev/null || true; done
        kill -9 "$hpid" 2>/dev/null || true
        wait "$hpid" 2>/dev/null || true
        echo fail
        return
    fi
    seq 1 $((n * 2)) | sed "s|.*|http://127.0.0.1:$port/@capsid/orders/bench/json|" \
        >"$OUT/urls.$size.$n"
    touch_pool "$OUT/urls.$size.$n"
    sleep 1
    if ! kill -0 "$hpid" 2>/dev/null; then
        wait "$hpid" 2>/dev/null || true
        echo fail
        return
    fi
    local pss
    pss=$(tree_pss_mb "$hpid")
    for c in $(pgrep -P "$hpid" 2>/dev/null); do kill -9 "$c" 2>/dev/null || true; done
    kill -9 "$hpid" 2>/dev/null || true
    wait "$hpid" 2>/dev/null || true
    echo "$pss"
}

probe_workerd() { # $1 size  $2 cells -> pss_mb or "fail"
    local size=$1 n=$2
    local conf="$APPS/density-$size-$n.capnp"
    {
        echo 'using Workerd = import "/workerd/workerd.capnp";'
        echo ""
        echo "const config :Workerd.Config = ("
        printf '  services = [ (name = "w0", worker = .w0)'
        local i
        for i in $(seq 1 $((n - 1))); do printf ', (name = "w%d", worker = .w%d)' "$i" "$i"; done
        echo " ],"
        echo "  sockets = [ (name = \"s0\", address = \"127.0.0.1:$(next_port_cell 0)\", http = (), service = \"w0\")"
        for i in $(seq 1 $((n - 1))); do
            printf ', (name = "s%d", address = "127.0.0.1:%d", http = (), service = "w%d")' "$i" "$(next_port_cell "$i")" "$i"
        done
        echo " ],"
        echo ");"
        for i in $(seq 0 $((n - 1))); do
            echo "const w$i :Workerd.Worker = ("
            echo '  compatibilityDate = "2026-08-20",'
            echo "  modules = [ (name = \"main\", esModule = embed \"app-workerd-$size.mjs\") ],"
            echo ");"
        done
    } >"$conf"
    taskset -c "$SUT_CPUSET" "$WORKERD_BIN" serve "$conf" \
        >"$OUT/err.workerd.$size.$n" 2>&1 &
    local wpid=$!
    PIDS+=("$wpid")
    # cell 0 owns 19700; that is the socket to wait on
    local up=0
    for _ in $(seq 1 600); do
        (exec 3<>"/dev/tcp/127.0.0.1/19700") 2>/dev/null && { up=1; break; }
        kill -0 "$wpid" 2>/dev/null || break
        sleep 0.05
    done
    if [ "$up" != 1 ]; then
        kill -9 "$wpid" 2>/dev/null || true
        wait "$wpid" 2>/dev/null || true
        echo fail
        return
    fi
    for i in $(seq 0 $((n - 1))); do
        echo "http://127.0.0.1:$((19700 + i))/"
    done >"$OUT/urls.w.$size.$n"
    touch_pool "$OUT/urls.w.$size.$n"
    # A pool that died under the touch (OOM, internal cap) must not yield a
    # half-loaded PSS sample: require the process alive, then a stable read.
    sleep 1
    if ! kill -0 "$wpid" 2>/dev/null; then
        wait "$wpid" 2>/dev/null || true
        echo fail
        return
    fi
    local pss pss2
    pss=$(tree_pss_mb "$wpid")
    sleep 1
    pss2=$(tree_pss_mb "$wpid")
    kill -9 "$wpid" 2>/dev/null || true
    wait "$wpid" 2>/dev/null || true
    if [ "$pss2" != "$pss" ] || awk -v a="$pss" 'BEGIN{exit !(a>0)}'; then
        echo "$pss"
    else
        echo fail
    fi
}

next_port_cell() { echo $((19700 + $1)); }

fill_stack() { # $1 stack  $2 size
    local stack=$1 size=$2
    local cap=$MAX_WORKERS
    [ "$stack" = workerd ] && cap=$MAX_CELLS
    local lo=0 hi=0 n=1 pss
    while :; do
        if [ "$n" -gt "$cap" ]; then
            # cap+1 is unbuildable by definition; binary-search the real
            # boundary between the last fitting count and the cap
            hi=$((cap + 1)); break
        fi
        if [ "$stack" = capsid ]; then pss=$(probe_capsid "$size" "$n"); else pss=$(probe_workerd "$size" "$n"); fi
        printf '{"probe":"%s","size":"%s","n":%d,"pss_mb":%s}\n' \
            "$stack" "$size" "$n" "$pss" >>"$OUT/probes.jsonl"
        if [ "$pss" = "fail" ]; then
            hi=$n; break
        fi
        if awk -v a="$pss" -v b="$BUDGET_MB" 'BEGIN{exit !(a>b)}'; then
            hi=$n; break
        fi
        lo=$n
        n=$((n * 2))
    done
    if [ "$lo" -eq 0 ]; then
        printf '%s %-6s workers=0 pss_mb=0 note=no pool fit under budget\n' \
            "$stack" "$size" | tee -a "$OUT/cells.txt"
        return
    fi
    if [ "$hi" -gt 0 ]; then
        local mid
        while [ $((hi - lo)) -gt 1 ]; do
            mid=$(((lo + hi) / 2))
            if [ "$stack" = capsid ]; then pss=$(probe_capsid "$size" "$mid"); else pss=$(probe_workerd "$size" "$mid"); fi
            printf '{"probe":"%s","size":"%s","n":%d,"pss_mb":%s}\n' \
                "$stack" "$size" "$mid" "$pss" >>"$OUT/probes.jsonl"
            if [ "$pss" = "fail" ] || awk -v a="$pss" -v b="$BUDGET_MB" 'BEGIN{exit !(a>b)}'; then
                hi=$mid
            else
                lo=$mid
            fi
        done
        if [ "$stack" = capsid ]; then pss=$(probe_capsid "$size" "$lo"); else pss=$(probe_workerd "$size" "$lo"); fi
        # Jitter guard: PSS at the boundary wobbles with GC timing and
        # isolate-init variance, so the recorded count must VERIFY under
        # the budget; step down until it does (or the pool is empty).
        local tries=0
        while [ "$pss" != "fail" ] && [ "$tries" -lt 6 ] && [ "$lo" -gt 1 ] \
                && awk -v a="$pss" -v b="$BUDGET_MB" 'BEGIN{exit !(a>b)}'; do
            lo=$((lo - 1))
            if [ "$stack" = capsid ]; then pss=$(probe_capsid "$size" "$lo"); else pss=$(probe_workerd "$size" "$lo"); fi
            tries=$((tries + 1))
        done
        printf '{"probe":"%s","size":"%s","n":%d,"pss_mb":%s,"final":true}\n' \
            "$stack" "$size" "$lo" "$pss" >>"$OUT/probes.jsonl"
    fi
    local unit=workers
    [ "$stack" = workerd ] && unit=cells
    printf '%s %-6s %s=%d pss_mb=%s note=largest pool under %d MB budget\n' \
        "$stack" "$size" "$unit" "$lo" "$pss" "$BUDGET_MB" \
        | tee -a "$OUT/cells.txt"
}

for stack in $STACKS; do
    for size in $SIZES; do
        fill_stack "$stack" "$size"
    done
done

find "$OUT" -maxdepth 1 -type f \( -name 'cells.txt' -o -name 'probes.jsonl' -o -name 'manifest.txt' \) -print0 |
    xargs -0 sha256sum >"$OUT/sha256sums.txt"
echo "density lane complete"
