#!/usr/bin/env bash
# Cold-start comparison: capsid source vs capsid trusted bytecode vs node vs
# deno vs workerd, at 10k/100k/1M fixture sizes. Per (stack,size) cell:
# 1 warmup run (discarded) + ROUNDS measured runs; median total_ms
# (process start -> READY -> first response), median warm_ms (per-request
# latency on the warmed process) and median PSS are reported. capsid
# host/worker, node/deno/workerd are pinned to SUT_CPUSET (4C protocol).
#
# workerd semantics: the process owns the listener, so READY is the first
# successful TCP connect to 127.0.0.1:18992 (listener bound) and the first
# curl completes the worker's first request like node/deno. Note that
# workerd lazily creates the isolate on the first request, so the
# ready_ms -> first-response span carries the V8/isolate init cost. The
# workerd binary is recorded in the manifest (WORKERD_BIN); recorded
# conclusions must use a release matching the ROADMAP-pinned workerd
# commit.
#
# warm_ms caveat: node/deno/workerd warm requests go through curl, so
# values near the curl process floor (~5 ms) are not meaningful; only
# capsid's warm_ms (in-process IPC) resolves sub-millisecond latency.
#
# Usage: bash bench/cold-start.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/cold-start-$(date +%Y%m%dT%H%M%S)}
mkdir -p "$OUT"
BUILD_DIR=${BUILD_DIR:-build-m1d}
HOST_BIN=${HOST_BIN:-$BUILD_DIR/capsid-host}
WORKER=${WORKER:-$BUILD_DIR/capsid-worker}
COLD=${COLD:-bench/bin/cold-start}
COMPILE=${COMPILE:-$BUILD_DIR/capsid-bytecode-compile}
WORKERD_BIN=${WORKERD_BIN:-}
SUT_CPUSET=${SUT_CPUSET:-0-3}
ROUNDS=${ROUNDS:-5}
WARM=${WARM:-50}
APPS=${APPS:-/tmp/cold-start-apps}
SIZES=${SIZES:-10k 100k 1m}
# STACKS selects which comparison stacks run: capsid always runs; node,
# deno, workerd are skipped (with a manifest-absent note) when the binary
# is missing or the name is omitted from STACKS.
STACKS=${STACKS:-capsid node deno workerd}

echo "rounds: $ROUNDS warm: $WARM sut_cpuset: $SUT_CPUSET" | tee "$OUT/manifest.txt"
{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "tag: $(git describe --exact-match --tags HEAD)"
    echo "runner_sha256: $(sha256sum bench/cold-start.sh | cut -d' ' -f1)"
    echo "runner_diff_sha256: $(git diff --no-ext-diff --binary -- bench/cold-start.sh | sha256sum | cut -d' ' -f1)"
    echo "command: OUT=$OUT BUILD_DIR=$BUILD_DIR COLD=$COLD WORKERD_BIN=$WORKERD_BIN APPS=$APPS ROUNDS=$ROUNDS WARM=$WARM bash bench/cold-start.sh"
    echo "uname: $(uname -a)"
    echo "cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "nproc: $(nproc)"
    echo "mem_total: $(sed -n 's/^MemTotal:[[:space:]]*//p' /proc/meminfo)"
    sha256sum "$WORKER" "$COLD" "$COMPILE"
    if command -v node >/dev/null 2>&1; then
        sha256sum "$(command -v node)"
        echo "node: $(node --version)"
    else
        echo "node: absent"
    fi
    if command -v deno >/dev/null 2>&1; then
        sha256sum "$(command -v deno)"
        echo "deno: $(deno --version | head -1)"
    else
        echo "deno: absent"
    fi
    if [ -n "$WORKERD_BIN" ]; then
        sha256sum "$WORKERD_BIN"
        echo "workerd_version: $("$WORKERD_BIN" --version 2>&1 | head -1)"
    else
        echo "workerd_version: absent (WORKERD_BIN not set)"
    fi
    find "$APPS" -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum
    echo "worker: $WORKER"
    echo "build_info_begin"
    sed -n '1,45p' "$BUILD_DIR/generated/build-info.txt"
    echo "build_info_end"
} >> "$OUT/manifest.txt"

median() {
    sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'
}

# PSS/RSS (kB) of a process tree, sampled from /proc smaps_rollup.
proc_tree_pss_kb() {
    local root=$1 field=$2 total=0 pid kb
    local pgid
    pgid=$(ps -o pgid= -p "$root" 2>/dev/null | tr -d ' ') || pgid=""
    for pid in $( { [ -n "$pgid" ] && pgrep -g "$pgid" 2>/dev/null; echo "$root"; } | sort -un ); do
        kb=$(awk -v f="$field" '$1==f {print $2; exit}' "/proc/$pid/smaps_rollup" 2>/dev/null)
        [ -n "${kb:-}" ] && total=$((total + kb))
    done
    echo "$total"
}

# Median warm latency over the LAST measured round only (the warmup round's
# samples are excluded by the offset).
last_round_warm_median() {
    local -a vals=("$@")
    local off=$((WARM * ROUNDS))
    local -a last=("${vals[@]:$off}")
    [ ${#last[@]} -eq 0 ] && last=("${vals[@]}")
    if [ ${#last[@]} -eq 0 ]; then
        echo -1
        return
    fi
    printf '%s\n' "${last[@]}" | median
}

# --- capsid source / bytecode ---
# Trusted-bytecode artifacts are compiled here so the runner is
# self-contained; application/version/key-id are fixed benchmark identity.
for label in $SIZES; do
    if [ ! -f "$APPS/app-capsid-$label.qjsb" ]; then
        "$COMPILE" --source "$APPS/app-capsid-$label.mjs" \
            --source-name "file://$APPS/app-capsid-$label.mjs" \
            --application coldbench --version 1 --key-id bench \
            --bytecode-out "$APPS/app-capsid-$label.qjsb" \
            --attestation-out "$APPS/app-capsid-$label.attest.json" \
            --signing-message-out "$APPS/app-capsid-$label.msg.bin"
    fi
done
for mode in source bytecode; do
    for label in $SIZES; do
        input="$APPS/app-capsid-$label.$([ "$mode" = source ] && echo mjs || echo qjsb)"
        source_name="file://$APPS/app-capsid-$label.mjs"
        # warmup run discarded, then ROUNDS measured runs. CAPSID_STRICT=1
        # installs the full Linux strict sandbox (namespaces, Landlock,
        # seccomp) in the spawned worker - the production isolation
        # posture - instead of the unsandboxed embedded default.
        strict_flag=""
        [ "${CAPSID_STRICT:-0}" = "1" ] && strict_flag="--strict"
        taskset -c "$SUT_CPUSET" "$COLD" --worker "$WORKER" --mode "$mode" \
            --input "$input" --source-name "$source_name" \
            --iterations 1 --warm "$WARM" $strict_flag >/dev/null 2>"$OUT/err.$mode.$label"
        taskset -c "$SUT_CPUSET" "$COLD" --worker "$WORKER" --mode "$mode" \
            --input "$input" --source-name "$source_name" \
            --iterations "$ROUNDS" --warm "$WARM" $strict_flag >"$OUT/raw.capsid-$mode.$label.jsonl"
        total=$(grep -o '"total_ms":[0-9.]*' "$OUT/raw.capsid-$mode.$label.jsonl" |
                cut -d: -f2 | median)
        ready=$(grep -o '"ready_ms":[0-9.]*' "$OUT/raw.capsid-$mode.$label.jsonl" |
                cut -d: -f2 | median)
        warm=$(grep -o '"warm_ms":[0-9.]*' "$OUT/raw.capsid-$mode.$label.jsonl" |
               cut -d: -f2 | median)
        pss=$(grep -o '"worker_pss_kb":[-0-9]*' "$OUT/raw.capsid-$mode.$label.jsonl" |
              cut -d: -f2 | awk '$1>0' | median)
        printf '%s %-6s %-11s total_ms=%8.2f ready_ms=%8.2f warm_ms=%7.3f pss_mb=%s\n' \
            capsid "$mode" "$label" "$total" "$ready" "$warm" \
            "$([ -n "${pss:-}" ] && awk -v k="$pss" 'BEGIN{printf "%.1f", k/1024}' || echo n/a)" \
            | tee -a "$OUT/cells.txt"
    done
done

# --- node / deno / workerd ---
# measure_vm <port> <label> <name> <cmd...>
# READY: node/deno print READY to stdout; a third argument form is not used
# for workerd (it uses measure_workerd below). After the first response the
# warm phase issues WARM sequential curls (median per-request latency) and
# samples the process-tree PSS.
measure_vm() {
    local port="$1" label="$2" name="$3"
    shift 3
    local cmd=("$@")
    local vals ready_vals=() warm_vals=() pss_vals=()
    local raw="$OUT/raw.$name.$label.jsonl"
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
            if ! curl --fail --silent --output /dev/null --max-time 5 \
                    "http://127.0.0.1:$port/"; then
                kill "$pid" 2>/dev/null || true
                wait "$pid" 2>/dev/null || true
                rm -f "$outfile"
                echo "$name $label failed first-response validation" >&2
                return 1
            fi
            # total = spawn -> first validated response (the cold-start.cc
            # definition). Everything after this point (warm rounds, PSS
            # walk) is reported separately and must not leak into total.
            end_ns=$(date +%s%N)
            total_ms=$(( (end_ns - start_ns) / 1000000 ))
        else
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            rm -f "$outfile"
            echo "$name $label did not become ready" >&2
            return 1
        fi
        local warm_start warm_ms
        for _ in $(seq 1 "$WARM"); do
            warm_start=$(date +%s%N)
            curl --fail --silent --output /dev/null --max-time 5 \
                "http://127.0.0.1:$port/" || true
            warm_vals+=( $(( ($(date +%s%N) - warm_start) / 1000000 )) )
        done
        local pss_kb
        pss_kb=$(proc_tree_pss_kb "$pid" Pss:)
        [ "${pss_kb:-0}" -gt 0 ] 2>/dev/null && pss_vals+=("$pss_kb")
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -f "$outfile"
        if [ "$round" -gt 1 ]; then
            vals+=("$total_ms")
            printf '{"round":%d,"ready_ms":%d,"total_ms":%d,"warm_ms":%s,"pss_kb":%s,"ok":true}\n' \
                "$((round - 1))" "$ready_ms" "$total_ms" \
                "$(last_round_warm_median "${warm_vals[@]}")" \
                "$([ ${#pss_vals[@]} -gt 0 ] && printf '%s' "${pss_vals[-1]}" || echo -1)" \
                >> "$raw"
        fi
    done
    local med=$(printf '%s\n' "${vals[@]}" | median)
    local rmed=$(printf '%s\n' "${ready_vals[@]:1}" | median)
    local wmed
    wmed=$(last_round_warm_median "${warm_vals[@]}")
    local pmed
    pmed=$(printf '%s\n' "${pss_vals[@]}" | median 2>/dev/null || echo 0)
    printf '%s %-6s %-11s total_ms=%8.2f ready_ms=%8.2f warm_ms=%7.3f pss_mb=%s\n' \
        "$name" "" "$label" "$med" "$rmed" "$wmed" \
        "$([ "${pmed:-0}" -gt 0 ] 2>/dev/null && awk -v k="$pmed" 'BEGIN{printf "%.1f", k/1024}' || echo n/a)" \
        | tee -a "$OUT/cells.txt"
}

# measure_workerd <label> — same phases as measure_vm, but READY is the
# first successful TCP connect (workerd owns the listener and prints no
# READY marker). Memory is the workerd process tree PSS after the warm
# phase.
measure_workerd() {
    local label="$1"
    local name=workerd
    [ -n "$WORKERD_BIN" ] || { echo "WORKERD_BIN not set; skipping workerd" >&2; return 0; }
    local config="$APPS/workerd-$label.capnp"
    local vals ready_vals=() warm_vals=() pss_vals=()
    local raw="$OUT/raw.$name.$label.jsonl"
    for round in $(seq 1 "$((ROUNDS + 1))"); do
        local start_ns end_ns total_ms ready_ms
        start_ns=$(date +%s%N)
        taskset -c "$SUT_CPUSET" "$WORKERD_BIN" serve "$config" \
            >"$OUT/err.workerd.$label" 2>&1 &
        local pid=$!
        local ready_seen=0
        for _ in $(seq 1 1000); do
            if (exec 3<>"/dev/tcp/127.0.0.1/18992") 2>/dev/null; then
                ready_seen=1
                break
            fi
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.005
        done
        if [ "$ready_seen" = 1 ]; then
            ready_ms=$(( ($(date +%s%N) - start_ns) / 1000000 ))
            ready_vals+=("$ready_ms")
            if ! curl --fail --silent --output /dev/null --max-time 5 \
                    "http://127.0.0.1:18992/"; then
                kill "$pid" 2>/dev/null || true
                wait "$pid" 2>/dev/null || true
                echo "$name $label failed first-response validation" >&2
                return 1
            fi
            # total = spawn -> first validated response; warm/PSS follow.
            end_ns=$(date +%s%N)
            total_ms=$(( (end_ns - start_ns) / 1000000 ))
        else
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            echo "$name $label did not become ready" >&2
            return 1
        fi
        local warm_start warm_ms
        for _ in $(seq 1 "$WARM"); do
            warm_start=$(date +%s%N)
            curl --fail --silent --output /dev/null --max-time 5 \
                "http://127.0.0.1:18992/" || true
            warm_vals+=( $(( ($(date +%s%N) - warm_start) / 1000000 )) )
        done
        local pss_kb
        pss_kb=$(proc_tree_pss_kb "$pid" Pss:)
        [ "${pss_kb:-0}" -gt 0 ] 2>/dev/null && pss_vals+=("$pss_kb")
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        if [ "$round" -gt 1 ]; then
            vals+=("$total_ms")
            printf '{"round":%d,"ready_ms":%d,"total_ms":%d,"warm_ms":%s,"pss_kb":%s,"ok":true}\n' \
                "$((round - 1))" "$ready_ms" "$total_ms" \
                "$(last_round_warm_median "${warm_vals[@]}")" \
                "$([ ${#pss_vals[@]} -gt 0 ] && printf '%s' "${pss_vals[-1]}" || echo -1)" \
                >> "$raw"
        fi
    done
    local med=$(printf '%s\n' "${vals[@]}" | median)
    local rmed=$(printf '%s\n' "${ready_vals[@]:1}" | median)
    local wmed
    wmed=$(last_round_warm_median "${warm_vals[@]}")
    local pmed
    pmed=$(printf '%s\n' "${pss_vals[@]}" | median 2>/dev/null || echo 0)
    printf '%s %-6s %-11s total_ms=%8.2f ready_ms=%8.2f warm_ms=%7.3f pss_mb=%s\n' \
        "$name" "" "$label" "$med" "$rmed" "$wmed" \
        "$([ "${pmed:-0}" -gt 0 ] 2>/dev/null && awk -v k="$pmed" 'BEGIN{printf "%.1f", k/1024}' || echo n/a)" \
        | tee -a "$OUT/cells.txt"
}

stack_enabled() {
    case " $STACKS " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

for label in $SIZES; do
    if stack_enabled node && command -v node >/dev/null 2>&1; then
        measure_vm 18990 "$label" node "$(command -v node)" "$APPS/app-node-$label.mjs"
    fi
    if stack_enabled deno && command -v deno >/dev/null 2>&1; then
        measure_vm 18991 "$label" deno "$(command -v deno)" run --allow-net "$APPS/app-deno-$label.mjs"
    fi
    if stack_enabled workerd; then
        measure_workerd "$label"
    fi
done

find "$OUT" -maxdepth 1 -type f ! -name sha256sums.txt -print0 | \
    sort -z | xargs -0 sha256sum > "$OUT/sha256sums.txt"
echo "results in $OUT"
