#!/usr/bin/env bash
# Cold-start comparison: capsid source vs capsid trusted bytecode vs node vs
# deno, at 10k/100k/1M fixture sizes. Per (stack,size) cell: 1 warmup run
# (discarded) + ROUNDS measured runs; median total_ms (process start ->
# READY -> first response) reported. capsid host/worker and node/deno all
# pinned to SUT_CPUSET (4C protocol).
#
# Usage: bash bench/cold-start.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="bench/results/cold-start-$(date +%Y%m%dT%H%M%S)"
mkdir -p "$OUT"
HOST_BIN=${HOST_BIN:-build-m1d/capsid-host}
WORKER=${WORKER:-build-m1d/capsid-worker}
COLD=${COLD:-bench/bin/cold-start}
COMPILE=${COMPILE:-build-m1d/capsid-bytecode-compile}
SUT_CPUSET=${SUT_CPUSET:-0-3}
ROUNDS=${ROUNDS:-5}
APPS=${APPS:-/tmp/cold-start-apps}
SIZES=${SIZES:-10k 100k 1m}

echo "rounds: $ROUNDS sut_cpuset: $SUT_CPUSET" | tee "$OUT/manifest.txt"
{
    sha256sum "$WORKER" "$COLD" "$COMPILE" "$(command -v node)" "$(command -v deno)"
    echo "node: $(node --version)"
    echo "deno: $(deno --version | head -1)"
    echo "worker: $WORKER"
} >> "$OUT/manifest.txt"

median() {
    sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

# --- capsid source / bytecode ---
for mode in source bytecode; do
    for label in $SIZES; do
        input="$APPS/app-capsid-$label.$([ "$mode" = source ] && echo mjs || echo qjsb)"
        source_name="file://$APPS/app-capsid-$label.mjs"
        # warmup run discarded, then ROUNDS measured runs
        taskset -c "$SUT_CPUSET" "$COLD" --worker "$WORKER" --mode "$mode" \
            --input "$input" --source-name "$source_name" \
            --iterations 1 >/dev/null 2>"$OUT/err.$mode.$label"
        taskset -c "$SUT_CPUSET" "$COLD" --worker "$WORKER" --mode "$mode" \
            --input "$input" --source-name "$source_name" \
            --iterations "$ROUNDS" >"$OUT/raw.capsid-$mode.$label.jsonl"
        total=$(grep -o '"total_ms":[0-9.]*' "$OUT/raw.capsid-$mode.$label.jsonl" |
                cut -d: -f2 | median)
        ready=$(grep -o '"ready_ms":[0-9.]*' "$OUT/raw.capsid-$mode.$label.jsonl" |
                cut -d: -f2 | median)
        printf '%s %-6s %-11s total_ms=%8.2f ready_ms=%8.2f\n' \
            capsid "$mode" "$label" "$total" "$ready" | tee -a "$OUT/cells.txt"
    done
done

# --- node / deno ---
measure_vm() {
    local port="$1" label="$2" name="$3"
    shift 3
    local cmd=("$@")
    local vals ready_vals=()
    for round in $(seq 1 "$((ROUNDS + 1))"); do
        local outfile=$(mktemp)
        local start_ns end_ns total_ms ready_ms
        start_ns=$(date +%s%N)
        taskset -c "$SUT_CPUSET" "${cmd[@]}" >"$outfile" 2>&1 &
        local pid=$!
        local ready_seen=0
        for _ in $(seq 1 500); do
            if grep -q READY "$outfile" 2>/dev/null; then
                ready_seen=1
                break
            fi
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.01
        done
        if [ "$ready_seen" = 1 ]; then
            ready_ms=$(( ($(date +%s%N) - start_ns) / 1000000 ))
            ready_vals+=("$ready_ms")
            curl -s -o /dev/null -m 5 "http://127.0.0.1:$port/" || true
        fi
        end_ns=$(date +%s%N)
        total_ms=$(( (end_ns - start_ns) / 1000000 ))
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -f "$outfile"
        if [ "$round" -gt 1 ]; then
            vals+=("$total_ms")
        fi
    done
    local med=$(printf '%s\n' "${vals[@]}" | median)
    local rmed=$(printf '%s\n' "${ready_vals[@]:1}" | median)
    printf '%s %-6s %-11s total_ms=%8.2f ready_ms=%8.2f\n' \
        "$name" "" "$label" "$med" "$rmed" | tee -a "$OUT/cells.txt"
}

for label in $SIZES; do
    measure_vm 18990 "$label" node "$(command -v node)" "$APPS/app-node-$label.mjs"
    measure_vm 18991 "$label" deno "$(command -v deno)" run --allow-net "$APPS/app-deno-$label.mjs"
done

echo "results in $OUT"
