#!/usr/bin/env bash
# bench/run-ab.sh — the M1B A/B benchmark runner.
#
# Runs the same loadgen against a baseline gateway and the candidate host,
# interleaved for at least three rounds each, then one separate profile run
# per side (headline runs carry no profiling probes). Every run writes the
# fixed evidence layout under bench/results/<run-id>/ and refuses to report
# anything when the evidence is incomplete.
#
# Component contract (environment variables, set by this runner):
#   gateways (baseline and candidate): CAPSID_BENCH_LISTEN, CAPSID_BENCH_READY_FD,
#     CAPSID_BENCH_BUNDLE, CAPSID_BENCH_WORKER, CAPSID_BENCH_APPLICATION,
#     CAPSID_BENCH_PUBLIC_AUTHORITY, CAPSID_BENCH_TIMEOUT_MS, CAPSID_BENCH_WINDOW,
#     CAPSID_BENCH_SIDE, CAPSID_BENCH_CPU_PROFILE (profile runs only).
#     They write one ready record {"schema":...,"address":...,"port":N,
#     "bundle_sha256":...,"worker_sha256":...} to the ready fd; the sha fields
#     are optional, but when present they must match what the runner handed out.
#   loadgen: CAPSID_BENCH_TARGET, CAPSID_BENCH_WORKLOAD, CAPSID_BENCH_WARMUP_S,
#     CAPSID_BENCH_DURATION_S, CAPSID_BENCH_CONNECTIONS, CAPSID_BENCH_INFLIGHT,
#     CAPSID_BENCH_SIDE, CAPSID_BENCH_ROUND, CAPSID_BENCH_SAMPLES_OUT,
#     CAPSID_BENCH_CORRECTNESS_OUT. It appends raw sample lines (side/round/phase
#     tagged) to the samples file and writes one correctness verdict.
#
# Usage:
#   run-ab.sh --run-id ID --out DIR \
#     --baseline CMD --candidate CMD --worker CMD --worker-name NAME \
#     --loadgen CMD --bundle PATH \
#     [--workload fixed-1k|cpu-template] [--rounds N] [--warmup S] [--duration S]
#     [--connections N] [--inflight N] [--cpuset LIST] [--build-dir PATH]
#     [--host-bin PATH] [--application ID] [--public-authority A] \
#     [--timeout-ms N] [--window N] [--no-profile] \
#     [--baseline-env 'K=V K2=V2'] [--candidate-env 'K=V K2=V2'] \
#     [--statistic mean|median] [--require-ipc-counters] \
#     [--baseline-host-profile]
#     [--baseline-workers N] [--candidate-workers N]
#
# Multi-worker profiling (M2): --baseline-workers/--candidate-workers (1/2/4,
# default 1) make the profile run require EXACTLY that many direct worker
# children per side and attach one perf record/stat stream per shard; worker
# counts land in the manifest. Single-worker evidence naming is unchanged.
#
# Statistical methodology: --statistic is frozen into the manifest before
# the run starts (default mean). The report's verdict compares the sides on
# the frozen statistic only; the other statistic is reported alongside but
# never drives acceptance.
#
# --baseline-host-profile: the baseline side is also a capsid-host binary
# (bodyless off/on A/B), so it cannot produce a Go pprof; the runner
# perf-records the baseline gateway into baseline-gateway.perf.data instead,
# and the evidence gate accepts either file for the baseline gateway.
#
# IPC mechanism counters: headline rounds are zero-probe; the dedicated
# diagnostic round (round 0, the profile run) exports CAPSID_HOST_IPC_METRICS
# and the runner sums the per-pump counter deltas of that round's loadgen
# window (warmup + measured) per side. The windows are sequential per side,
# so the gate compares per-request ratios (counters ÷ completed requests of
# the same window) rather than raw totals — a machine drift between windows
# then cannot skew the mechanism verdict. Counters land in
# manifest.ipc_mechanism per side. --require-ipc-counters turns a side
# without counters into incomplete evidence (used by the
# bodyless off/on A/B, where both sides are the same capsid-host binary).
#
# Exit codes: 0 = complete evidence; 1 = component or correctness failure;
# 2 = incomplete evidence (missing samples/profiles/manifest fields).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The argument parser consumes the positional parameters; keep the original
# argv for the replayable command recorded in the manifest.
ORIGINAL_ARGV=("$@")

RUN_ID=""
OUT=""
BASELINE=""
CANDIDATE=""
WORKER=""
WORKER_NAME="capsid-worker"
LOADGEN=""
BUNDLE=""
WORKLOAD="fixed-1k"
ROUNDS=3
WARMUP=5
DURATION=10
CONNECTIONS=16
INFLIGHT=64
CPUSET=""
# Load-side cpuset, separate from the measured side (M2 pool sizing):
# host+worker run on CPUSET, the loadgen on LOADGEN_CPUSET, so the load
# side can never steal cores from the measured side. Defaults to CPUSET
# for backward compatibility. Recorded in the manifest.
LOADGEN_CPUSET=""
BUILD_DIR=""
HOST_BIN=""
# Per-side Host binaries (M2 optimization A/B): --host-bin means both
# sides use the same Host; the side-specific flags take precedence and let
# an optimization A/B bind each side to the exact implementation that
# produced its samples. A side without any Host stays unbound (fake
# components do not consume one).
BASELINE_HOST_BIN=""
CANDIDATE_HOST_BIN=""
APP="orders"
AUTHORITY="public.example"
TIMEOUT_MS=10000
WINDOW=1024
NO_PROFILE=0
BASELINE_ENV=""
CANDIDATE_ENV=""
STATISTIC="mean"
REQUIRE_IPC_COUNTERS=0
BASELINE_HOST_PROFILE=0
# Multi-worker profile: the fixed pool size per side (1/2/4). Default 1
# keeps every single-worker evidence contract unchanged; a pool side must
# expose exactly this many direct worker children for the profile run.
BASELINE_WORKERS=1
CANDIDATE_WORKERS=1
# The runner always exports CAPSID_TCP_NODELAY to components; only the exact
# value "0" disables it (default is on — single-connection latency drops
# 43ms→1.3ms with no throughput regression).
TCP_NODELAY_STATE="on"
[ "${CAPSID_TCP_NODELAY:-}" = "0" ] && TCP_NODELAY_STATE="off"

usage() {
    sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
    --run-id) RUN_ID="${2:?}"; shift 2 ;;
    --out) OUT="${2:?}"; shift 2 ;;
    --baseline) BASELINE="${2:?}"; shift 2 ;;
    --candidate) CANDIDATE="${2:?}"; shift 2 ;;
    --worker) WORKER="${2:?}"; shift 2 ;;
    --worker-name) WORKER_NAME="${2:?}"; shift 2 ;;
    --loadgen) LOADGEN="${2:?}"; shift 2 ;;
    --bundle) BUNDLE="${2:?}"; shift 2 ;;
    --workload) WORKLOAD="${2:?}"; shift 2 ;;
    --rounds) ROUNDS="${2:?}"; shift 2 ;;
    --warmup) WARMUP="${2:?}"; shift 2 ;;
    --duration) DURATION="${2:?}"; shift 2 ;;
    --connections) CONNECTIONS="${2:?}"; shift 2 ;;
    --inflight) INFLIGHT="${2:?}"; shift 2 ;;
    --cpuset) CPUSET="${2:?}"; shift 2 ;;
    --loadgen-cpuset) LOADGEN_CPUSET="${2:?}"; shift 2 ;;
    --build-dir) BUILD_DIR="${2:?}"; shift 2 ;;
    --host-bin) HOST_BIN="${2:?}"; shift 2 ;;
    --baseline-host-bin) BASELINE_HOST_BIN="${2:?}"; shift 2 ;;
    --candidate-host-bin) CANDIDATE_HOST_BIN="${2:?}"; shift 2 ;;
    --application) APP="${2:?}"; shift 2 ;;
    --public-authority) AUTHORITY="${2:?}"; shift 2 ;;
    --timeout-ms) TIMEOUT_MS="${2:?}"; shift 2 ;;
    --window) WINDOW="${2:?}"; shift 2 ;;
    --baseline-env) BASELINE_ENV="${2:?}"; shift 2 ;;
    --candidate-env) CANDIDATE_ENV="${2:?}"; shift 2 ;;
    --statistic) STATISTIC="${2:?}"; shift 2 ;;
    --require-ipc-counters) REQUIRE_IPC_COUNTERS=1; shift ;;
    --baseline-host-profile) BASELINE_HOST_PROFILE=1; shift ;;
    --baseline-workers) BASELINE_WORKERS="${2:?}"; shift 2 ;;
    --candidate-workers) CANDIDATE_WORKERS="${2:?}"; shift 2 ;;
    *) echo "run-ab: unknown argument: $1" >&2; usage ;;
    esac
done

for var in RUN_ID OUT BASELINE CANDIDATE WORKER LOADGEN BUNDLE; do
    if [ -z "${!var}" ]; then
        echo "run-ab: missing --$(echo "$var" | tr '[:upper:]' '[:lower:]' | tr '_' '-')" >&2
        usage
    fi
done

if [ "$ROUNDS" -lt 3 ]; then
    echo "run-ab: --rounds must be at least 3 (got $ROUNDS)" >&2
    exit 2
fi
if [ "$STATISTIC" != "mean" ] && [ "$STATISTIC" != "median" ]; then
    echo "run-ab: --statistic must be mean or median (got $STATISTIC)" >&2
    exit 2
fi
for side_workers in "$BASELINE_WORKERS" "$CANDIDATE_WORKERS"; do
    case "$side_workers" in
    1|2|4|6|8) ;;
    *) echo "run-ab: worker count must be 1, 2, 4, 6 or 8 (got $side_workers)" >&2; exit 2 ;;
    esac
done

# Per-side extra environment (K=V assignments). Validate every token so the
# word-split injection into the component environment cannot smuggle shell
# syntax through a flag.
validate_env_list() {
    local name="$1" value="$2" token
    for token in $value; do
        if ! printf '%s' "$token" | grep -qE '^[A-Za-z_][A-Za-z0-9_]*=[^ ]*$'; then
            echo "run-ab: invalid $name entry: $token" >&2
            exit 2
        fi
    done
}
validate_env_list --baseline-env "$BASELINE_ENV"
validate_env_list --candidate-env "$CANDIDATE_ENV"
[ -x "$BASELINE" ] || { echo "run-ab: baseline is not executable: $BASELINE" >&2; exit 2; }
[ -x "$CANDIDATE" ] || { echo "run-ab: candidate is not executable: $CANDIDATE" >&2; exit 2; }
# Per-side Host identity: side-specific flags win over the shared --host-bin;
# a provided Host must exist and be executable, an empty side stays unbound
# (fake components never consume CAPSID_BENCH_HOST_BIN).
BASELINE_HOST_BIN="${BASELINE_HOST_BIN:-$HOST_BIN}"
CANDIDATE_HOST_BIN="${CANDIDATE_HOST_BIN:-$HOST_BIN}"
for side_host in "$BASELINE_HOST_BIN" "$CANDIDATE_HOST_BIN"; do
    if [ -n "$side_host" ]; then
        [ -f "$side_host" ] && [ -x "$side_host" ] || {
            echo "run-ab: Host binary is not executable: $side_host" >&2
            exit 2
        }
    fi
done
[ -x "$LOADGEN" ] || { echo "run-ab: loadgen is not executable: $LOADGEN" >&2; exit 2; }
[ -f "$BUNDLE" ] || { echo "run-ab: bundle not found: $BUNDLE" >&2; exit 2; }
[ -f "$WORKER" ] || { echo "run-ab: worker not found: $WORKER" >&2; exit 2; }

if [ "$NO_PROFILE" != "1" ] && ! perf stat -e task-clock true >/dev/null 2>&1; then
    echo "run-ab: perf is not usable in this environment; evidence would be INCOMPLETE" >&2
    exit 2
fi

mkdir -p "$OUT" "$OUT/perf-stat" "$OUT/.tmp"
: >"$OUT/samples.jsonl"
: >"$OUT/.tmp/correctness.lines"
: >"$OUT/.tmp/ready.lines"

BUNDLE_SHA="$(sha256sum "$BUNDLE" | cut -d' ' -f1)"
WORKER_SHA="$(sha256sum "$WORKER" | cut -d' ' -f1)"
COMPONENT_SHA="$(sha256sum "$BASELINE" "$CANDIDATE" "$LOADGEN" | sha256sum | cut -d' ' -f1)"

NOW="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
COMMIT="$(git -C "$SCRIPT_DIR/.." rev-parse HEAD 2>/dev/null || echo unknown)"
UNAME="$(uname -srm)"
NPROC="$(nproc)"
CGROUP_CPU_MAX="$(cat /sys/fs/cgroup/cpu.max 2>/dev/null || echo n/a)"
PERF_PARANOID="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo n/a)"
PERF_AVAILABLE=1

BUILD_ARGS="{}"
if [ -n "$BUILD_DIR" ]; then
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
        BUILD_ARGS="$(grep -E '^(CMAKE_BUILD_TYPE|CAPSID_ENABLE_[A-Z]+|CAPSID_BUILD_[A-Z]+):' \
            "$BUILD_DIR/CMakeCache.txt" | sort | jq -R -s 'split("\n")[:-1]' 2>/dev/null \
            || echo '"'"$(grep -E '^(CMAKE_BUILD_TYPE|CAPSID_ENABLE_[A-Z]+):' "$BUILD_DIR/CMakeCache.txt" | tr '\n' ';')"'"')"
    else
        echo "run-ab: --build-dir has no CMakeCache.txt: $BUILD_DIR" >&2
        exit 2
    fi
fi

# ---------------------------------------------------------------------------
# Component lifecycle
# ---------------------------------------------------------------------------

declare -A COMPONENT_PID
declare -A COMPONENT_PORT
declare -A COMPONENT_IDENTITY

# start_component <side> <cmd> <round> [profile-out]
start_component() {
    local side="$1" cmd="$2" round="$3"
    local profile_out="${4:-}"
    local ready_fd=9
    local port pid line
    local identity_file="$OUT/.tmp/identity.$side.round$round.$$.json"
    # Per-side environment (--baseline-env/--candidate-env): the bodyless
    # off/on A/B runs the same capsid-host binary on both sides and differs
    # only in these assignments. Same for the fixed pool size: each side
    # carries its own worker count to the static-pool wrapper.
    local side_env=""
    local side_workers="$BASELINE_WORKERS"
    local side_host_bin="$BASELINE_HOST_BIN"
    if [ "$side" = "baseline" ]; then
        side_env="$BASELINE_ENV"
    else
        side_env="$CANDIDATE_ENV"
        side_workers="$CANDIDATE_WORKERS"
        side_host_bin="$CANDIDATE_HOST_BIN"
    fi

    local ready_file="$OUT/ready.$side.$$"
    : >"$ready_file"
    exec 9>"$ready_file"

    # CAPSID_HOST_IPC_METRICS arms the host's per-pump delta metrics line.
    # Headline rounds are zero-probe: metrics are on only for the dedicated
    # diagnostic round (round 0, the profile run), whose loadgen window
    # feeds the IPC mechanism counters. Metrics on a headline round would
    # add an asymmetric fprintf+JSON probe to whichever side pumps fewer
    # events, skewing the QPS comparison.
    metrics_env=""
    [ "$round" = "0" ] && metrics_env="CAPSID_HOST_IPC_METRICS=1"
    env \
        CAPSID_BENCH_LISTEN="127.0.0.1:0" \
        CAPSID_BENCH_READY_FD="$ready_fd" \
        CAPSID_BENCH_BUNDLE="$BUNDLE" \
        CAPSID_BENCH_WORKER="$WORKER" \
        CAPSID_BENCH_APPLICATION="$APP" \
        CAPSID_BENCH_PUBLIC_AUTHORITY="$AUTHORITY" \
        CAPSID_BENCH_TIMEOUT_MS="$TIMEOUT_MS" \
        CAPSID_BENCH_WINDOW="$WINDOW" \
        CAPSID_BENCH_SIDE="$side" \
        CAPSID_BENCH_HOST_BIN="$side_host_bin" \
        CAPSID_BENCH_CPU_PROFILE="$profile_out" \
        CAPSID_BENCH_IDENTITY_OUT="$identity_file" \
        CAPSID_BENCH_WORKERS="$side_workers" \
        CAPSID_TCP_NODELAY="${CAPSID_TCP_NODELAY:-1}" \
        ${metrics_env} \
        ${side_env} \
        ${CPUSET:+taskset -c "$CPUSET"} \
        "$cmd" >"$OUT/.tmp/$side.round$round.$RUN_ID.log" 2>&1 &
    pid=$!
    COMPONENT_PID[$side]=$pid
    COMPONENT_IDENTITY[$side]=$identity_file

    local deadline=$((SECONDS + 60))
    while [ $SECONDS -lt "$deadline" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "run-ab: $side component exited before READY; log tail:" >&2
            tail -5 "$OUT/$side.$RUN_ID.log" >&2 || true
            exit 1
        fi
        if [ -s "$ready_file" ]; then
            line="$(head -1 "$ready_file")"
            break
        fi
        sleep 0.1
    done
    if [ -z "${line:-}" ]; then
        echo "run-ab: $side component did not become READY in time" >&2
        exit 1
    fi
    exec 9>&-

    port="$(printf '%s' "$line" | sed -n 's/.*"port":\([0-9][0-9]*\).*/\1/p')"
    [ -n "$port" ] || { echo "run-ab: $side ready record has no port: $line" >&2; exit 1; }
    COMPONENT_PORT[$side]=$port

    # Identity contract: every component must report the bundle/worker
    # SHA-256 it actually loaded through CAPSID_BENCH_IDENTITY_OUT, written
    # before the ready record. Missing or mismatched identity is incomplete
    # evidence — an A/B run whose sides loaded different bundles is void.
    local reported_bundle="" reported_worker=""
    if [ -s "$identity_file" ]; then
        reported_bundle="$(sed -n \
            's/.*"bundle_sha256"[[:space:]]*:[[:space:]]*"\([0-9a-f]\{64\}\)".*/\1/p' \
            "$identity_file" | head -1)"
        reported_worker="$(sed -n \
            's/.*"worker_sha256"[[:space:]]*:[[:space:]]*"\([0-9a-f]\{64\}\)".*/\1/p' \
            "$identity_file" | head -1)"
    fi
    if [ -z "$reported_bundle" ] || [ -z "$reported_worker" ]; then
        echo "run-ab: $side (round $round) did not report its bundle/worker identity" >&2
        exit 2
    fi
    if [ "$reported_bundle" != "$BUNDLE_SHA" ] || [ "$reported_worker" != "$WORKER_SHA" ]; then
        echo "run-ab: $side (round $round) identity does not match the runner's bundle/worker" >&2
        exit 2
    fi
    printf '%s\n' "$line" >>"$OUT/.tmp/ready.lines"
}

stop_component() {
    local side="$1" pid="${COMPONENT_PID[$side]:-}"
    [ -n "$pid" ] || return 0
    kill -TERM "$pid" 2>/dev/null || true
    local deadline=$((SECONDS + 5))
    while [ $SECONDS -lt "$deadline" ] && kill -0 "$pid" 2>/dev/null; do
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    COMPONENT_PID[$side]=""
}

run_loadgen() {
    local side="$1" round="$2"
    local samples="$OUT/samples.$side.$round.$$.jsonl"
    local correctness="$OUT/correctness.$side.$round.$$.json"
    # The loadgen environment, identical for headline rounds and the
    # profile round. Only the profile round (round 0) is wrapped in perf
    # stat so the evidence can tell whether the LOAD SIDE saturated its
    # cores (headline rounds stay zero-probe).
    local loadgen_env=(env
        CAPSID_BENCH_TARGET="http://127.0.0.1:${COMPONENT_PORT[$side]}"
        CAPSID_BENCH_WORKLOAD="$WORKLOAD"
        CAPSID_BENCH_WARMUP_S="$WARMUP"
        CAPSID_BENCH_DURATION_S="$DURATION"
        CAPSID_BENCH_CONNECTIONS="$CONNECTIONS"
        CAPSID_BENCH_INFLIGHT="$INFLIGHT"
        CAPSID_BENCH_SIDE="$side"
        CAPSID_BENCH_ROUND="$round"
        CAPSID_BENCH_SAMPLES_OUT="$samples"
        CAPSID_BENCH_CORRECTNESS_OUT="$correctness"
        ${LOADGEN_CPUSET:+taskset -c "$LOADGEN_CPUSET"} \
        ${CPUSET:+taskset -c "$CPUSET"})
    if [ "$round" = "0" ] && [ "$NO_PROFILE" != "1" ]; then
        # perf stat forwards the child's exit status, so the loadgen's
        # own return code is preserved exactly.
        perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
            -o "$OUT/perf-stat/$side-loadgen.stat" \
            "${loadgen_env[@]}" "$LOADGEN"
    else
        "${loadgen_env[@]}" "$LOADGEN"
    fi
    local loadgen_rc=$?
    [ "$loadgen_rc" -eq 0 ] || { echo "run-ab: loadgen failed for $side round $round (rc=$loadgen_rc)" >&2; exit 1; }

    cat "$samples" >>"$OUT/samples.jsonl"
    cat "$correctness" >>"$OUT/.tmp/correctness.lines"
    rm -f "$samples" "$correctness"

    local ok errors_as_success
    ok="$(tail -1 "$OUT/.tmp/correctness.lines" | sed -n 's/.*"ok":\(true\|false\).*/\1/p')"
    [ "$ok" = "true" ] || { echo "run-ab: correctness failed for $side round $round" >&2; exit 1; }
    errors_as_success="$(tail -1 "$OUT/.tmp/correctness.lines" | sed -n 's/.*"errors_as_success":\(true\|false\).*/\1/p')"
    [ "$errors_as_success" != "true" ] || { echo "run-ab: errors counted as success for $side round $round" >&2; exit 1; }
}

# ---------------------------------------------------------------------------
# Headline runs: three interleaved rounds per side, no profiling probes.
# ---------------------------------------------------------------------------

for ((round = 1; round <= ROUNDS; round++)); do
    # Alternating order: A→B, B→A, A→B so the candidate is never
    # always the second side (eliminates a fixed order bias).
    if ((round % 2 == 0)); then
        HEADLINE_SIDES=(candidate baseline)
    else
        HEADLINE_SIDES=(baseline candidate)
    fi
    for side in "${HEADLINE_SIDES[@]}"; do
        cmd="$BASELINE"
        [ "$side" = "candidate" ] && cmd="$CANDIDATE"
        # Headline rounds are zero-probe (no CAPSID_HOST_IPC_METRICS);
        # mechanism counters come from the dedicated diagnostic round 0.
        start_component "$side" "$cmd" "$round"
        run_loadgen "$side" "$round"
        stop_component "$side"
    done
done

# ---------------------------------------------------------------------------
# Profile runs: one separate run per side with probes attached. Not part of
# the headline samples.
# ---------------------------------------------------------------------------

if [ "$NO_PROFILE" != "1" ]; then
    for side in baseline candidate; do
        cmd="$BASELINE"
        [ "$side" = "candidate" ] && cmd="$CANDIDATE"
        profile_out="$OUT/$side-gateway.pprof"
        start_component "$side" "$cmd" 0 "$profile_out"

        local_pid="${COMPONENT_PID[$side]}"
        workers="$BASELINE_WORKERS"
        [ "$side" = "candidate" ] && workers="$CANDIDATE_WORKERS"

        # Worker processes: the gateway spawns them directly, so match by
        # parent pid, skipping zombies (perf cannot attach to them). A pool
        # side must expose EXACTLY its configured worker count — one perf
        # stream per shard, and a missing shard would silently halve the
        # profile. The legacy cmdline fallback exists for script-based
        # fakes and only ever applies to a single worker.
        all_workers="$(ps -eo pid=,ppid=,stat=,cmd= | \
            awk -v gw="$local_pid" -v name="$WORKER_NAME" '
                $2 == gw && $0 ~ name && $3 !~ /^Z/ { print $1 }')"
        worker_count="$(printf '%s\n' "$all_workers" | grep -c . || true)"
        if [ "$worker_count" -lt "$workers" ] && [ "$workers" -eq 1 ]; then
            # Legacy cmdline fallback for script-based fakes; it must still
            # match EXACTLY one process (never an over-broad partial match).
            all_workers="$(ps -eo pid=,ppid=,stat=,cmd= | \
                awk -v name="$WORKER_NAME" '
                    $0 ~ name && $3 !~ /^Z/ && $0 !~ /run-ab\.sh/ && $0 !~ /awk/ {
                        print $1; exit
                    }')"
            worker_count="$(printf '%s\n' "$all_workers" | grep -c . || true)"
        fi
        if [ "$worker_count" -ne "$workers" ]; then
            echo "run-ab: expected $workers worker processes for $side profile run, found $worker_count" >&2
            exit 2
        fi
        mapfile -t worker_pids < <(printf '%s\n' "$all_workers" | head -n "$workers")

        # Resource sampling (RSS/PSS from /proc/<pid>/status, IPC syscalls
        # and bytes from /proc/<pid>/io) across the loadgen run.
        snapshot_resource() {
            local pid="$1" label="$2"
            local rss pss syscr syscw rchar wchar
            rss="$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)"
            # PSS is only available from smaps_rollup, not from status.
            pss="$(awk '/^Pss:/ {print $2}' "/proc/$pid/smaps_rollup" 2>/dev/null || echo 0)"
            syscr="$(awk '/^syscr:/ {print $2}' "/proc/$pid/io" 2>/dev/null || echo 0)"
            syscw="$(awk '/^syscw:/ {print $2}' "/proc/$pid/io" 2>/dev/null || echo 0)"
            rchar="$(awk '/^rchar:/ {print $2}' "/proc/$pid/io" 2>/dev/null || echo 0)"
            wchar="$(awk '/^wchar:/ {print $2}' "/proc/$pid/io" 2>/dev/null || echo 0)"
            # awk exits 0 even without a match; a missing field must not
            # produce an empty arithmetic operand downstream.
            [ -n "$rss" ] || rss=0
            [ -n "$pss" ] || pss=0
            [ -n "$syscr" ] || syscr=0
            [ -n "$syscw" ] || syscw=0
            [ -n "$rchar" ] || rchar=0
            [ -n "$wchar" ] || wchar=0
            printf '%s|%s|%s|%s|%s|%s|%s\n' \
                "$label" "$rss" "$pss" "$syscr" "$syscw" "$rchar" "$wchar" \
                >>"$OUT/.tmp/resource.$side.profile.$$"
        }
        : >"$OUT/.tmp/resource.$side.profile.$$"
        snapshot_resource "$local_pid" "gateway"
        worker_index=0
        for worker_pid in "${worker_pids[@]}"; do
            worker_index=$((worker_index + 1))
            if [ "$workers" -eq 1 ]; then
                snapshot_resource "$worker_pid" "worker"
            else
                snapshot_resource "$worker_pid" "worker.$worker_index"
            fi
        done

        # Candidate (C++) host: perf record. Baseline (Go) gateway: pprof via
        # CAPSID_BENCH_CPU_PROFILE; perf stat still applies to both sides.
        perf_record_pid=""
        if [ "$side" = "candidate" ]; then
            perf record --call-graph dwarf -F 199 \
                -o "$OUT/candidate-host.perf.data" -p "$local_pid" \
                >"$OUT/perf-stat/candidate-host.record.log" 2>&1 &
            perf_record_pid=$!
        elif [ "$BASELINE_HOST_PROFILE" = "1" ]; then
            perf record --call-graph dwarf -F 199 \
                -o "$OUT/baseline-gateway.perf.data" -p "$local_pid" \
                >"$OUT/perf-stat/baseline-gateway.record.log" 2>&1 &
            perf_record_pid=$!
        fi
        # One perf record + stat stream per pool shard: the single-worker
        # naming stays byte-identical (evidence regression), a pool side
        # gets $side-worker.N.* files. All streams are attached up front so
        # the loadgen window samples every shard equally.
        worker_record_pids=()
        worker_stat_pids=()
        worker_index=0
        for worker_pid in "${worker_pids[@]}"; do
            worker_index=$((worker_index + 1))
            worker_suffix=""
            [ "$workers" -eq 1 ] || worker_suffix=".$worker_index"
            perf record --call-graph dwarf -F 199 \
                -o "$OUT/$side-worker$worker_suffix.perf.data" -p "$worker_pid" \
                >"$OUT/perf-stat/$side-worker$worker_suffix.record.log" 2>&1 &
            worker_record_pids+=("$!")
            perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
                -p "$worker_pid" \
                -o "$OUT/perf-stat/$side-worker$worker_suffix.stat" \
                -- sleep "$DURATION" >/dev/null 2>&1 &
            worker_stat_pids+=("$!")
        done

        perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
            -p "$local_pid" -o "$OUT/perf-stat/$side-gateway.stat" -- sleep "$DURATION" \
            >/dev/null 2>&1 &
        gateway_stat_pid=$!

        # Diagnostic window IPC mechanism counters: the round-0 loadgen runs
        # with CAPSID_HOST_IPC_METRICS armed; count the per-pump delta
        # metrics lines already emitted (startup noise) BEFORE the loadgen
        # starts, then sum the deltas the loadgen window appends. The
        # per-side window files are merged by the mechanism step below; the
        # gate compares per-request ratios (see evidence.py), which makes
        # the sequential per-side windows volume-comparable.
        ipc_log="$OUT/.tmp/$side.round0.$RUN_ID.log"
        pre_metric_lines="$(grep -c '^{"host":' "$ipc_log" 2>/dev/null || true)"
        [ -n "$pre_metric_lines" ] || pre_metric_lines=0

        run_loadgen "$side" 0

        snapshot_resource "$local_pid" "gateway"
        worker_index=0
        for worker_pid in "${worker_pids[@]}"; do
            worker_index=$((worker_index + 1))
            if [ "$workers" -eq 1 ]; then
                snapshot_resource "$worker_pid" "worker"
            else
                snapshot_resource "$worker_pid" "worker.$worker_index"
            fi
        done

        python3 - "$ipc_log" "$pre_metric_lines" \
            "$OUT/.tmp/ipc_window.$side.0.$$.json" <<'PY'
import json, sys
log, pre, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
totals = {}
window_lines = 0
index = 0
with open(log, "r", encoding="utf-8", errors="replace") as handle:
    for line in handle:
        line = line.strip()
        if not line.startswith('{"host":'):
            continue
        index += 1
        if index <= pre:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        window_lines += 1
        for section in ("host", "client"):
            for key, value in (obj.get(section) or {}).items():
                if isinstance(value, int) and not isinstance(value, bool):
                    dotted = "{}.{}".format(section, key)
                    # High-water counters are maxima, not deltas: each line
                    # carries the sub-window's peak, so the window's peak is
                    # the max across lines, not the sum.
                    if dotted.endswith("_hw"):
                        totals[dotted] = max(
                            totals.get(dotted, 0), value)
                    else:
                        totals[dotted] = totals.get(dotted, 0) + value
with open(out, "w", encoding="utf-8") as handle:
    json.dump({"window_lines": window_lines, "counters": totals}, handle)
PY

        for p in "$gateway_stat_pid" "${worker_stat_pids[@]}"; do
            [ -n "$p" ] && wait "$p" 2>/dev/null || true
        done
        for p in "$perf_record_pid" "${worker_record_pids[@]}"; do
            [ -n "$p" ] && kill -INT "$p" 2>/dev/null || true
        done
        for p in "$perf_record_pid" "${worker_record_pids[@]}"; do
            [ -n "$p" ] && wait "$p" 2>/dev/null || true
        done
        stop_component "$side"
    done
fi

# ---------------------------------------------------------------------------
# Evidence gate
# ---------------------------------------------------------------------------

EVIDENCE_STATUS="complete"
INCOMPLETE_REASONS=""

fail_evidence() {
    EVIDENCE_STATUS="incomplete"
    INCOMPLETE_REASONS="${INCOMPLETE_REASONS}${INCOMPLETE_REASONS:+; }$1"
}

# The working tree must be clean: a benchmark bound to a dirty tree cannot
# be replayed from its manifest commit. The runner's own evidence output
# (bench/results/, gitignored by default and untracked until reviewed) is
# excluded — evidence dirs are legitimate untracked state, not source dirt.
# CAPSID_BENCH_TEST_MODE is for the fake-component RED test only; production
# runs must never set it, and a test-mode run is recorded as such in the
# manifest.
TEST_MODE="${CAPSID_BENCH_TEST_MODE:-0}"
if [ "$TEST_MODE" != "1" ] && \
    [ -n "$(git -C "$SCRIPT_DIR/.." status --porcelain -- . \
        ':(exclude)bench/results/' 2>/dev/null)" ]; then
    fail_evidence "dirty working tree"
fi

# Component logs (one file per side and round) must be free of internal
# Runtime/ABI state errors: a frame that reached the Runtime after it forgot
# the request is a Host-side race.
if grep -lE \
    "grant_response_credit failed|write_request failed|end_request failed|begin_request failed|invalid IPC frame" \
    "$OUT"/.tmp/*.log >/dev/null 2>&1; then
    fail_evidence "component log contains internal state errors"
fi

# Perf quality: the profile runs must not have lost samples, must have at
# least one user-space symbol resolved, and all perf stat counters must have
# been supported.
if [ "$NO_PROFILE" != "1" ]; then
    # Worker profile files are $side-worker.perf.data for a single worker
    # and $side-worker.N.perf.data for a pool side; the glob covers both.
    for file in "$OUT"/baseline-worker*.perf.data "$OUT"/candidate-worker*.perf.data \
        "$OUT"/candidate-host.perf.data "$OUT"/baseline-gateway.perf.data; do
        [ -e "$file" ] || continue
        if [ -s "$file" ]; then
            if perf report --stdio -i "$file" --no-children 2>/dev/null \
                | grep -q "Total Lost Samples: [1-9]"; then
                fail_evidence "lost samples in $(basename "$file")"
            fi
            # A profile with zero resolved user-space symbols is not usable
            # evidence — every symbol column would be a kernel address.
            # With --sort symbol the symbol column always begins with a
            # bracket ([.] user, [k] kernel, [unknown]), so "resolved"
            # means a [.] line whose symbol is not a raw hex address
            # (unresolved user samples render as "[.] 0x...").
            # Test-mode runs (fake components) are exempt.
            if [ "$TEST_MODE" != "1" ] && \
                ! perf report --stdio --no-children -i "$file" \
                --sort symbol 2>/dev/null | grep -qE '%\s+\[\.\]\s+[^0\[]'; then
                fail_evidence "zero user-space symbols in $(basename "$file")"
            fi
        fi
    done
    if grep -q "<not supported>" "$OUT"/perf-stat/*.stat 2>/dev/null; then
        fail_evidence "perf stat reported unsupported counters"
    fi
fi

# Samples: at least ROUNDS measured rounds per side.
for side in baseline candidate; do
    for ((round = 1; round <= ROUNDS; round++)); do
        if ! grep -q "\"side\":\"$side\",\"round\":$round,\"phase\":\"measured\"" \
            "$OUT/samples.jsonl"; then
            fail_evidence "missing measured sample for $side round $round"
        fi
    done
done

# IPC mechanism counters: merge the diagnostic-window sums into one file per
# side. With --require-ipc-counters, every side must have produced at least
# one counter line inside its round-0 loadgen window (the bodyless off/on
# A/B runs the same capsid-host binary on both sides, so both sides must
# emit counters).
for side in baseline candidate; do
    python3 - "$OUT" "$side" "$$" <<'PY'
import json, sys, glob, os
out_dir, side, pid = sys.argv[1], sys.argv[2], sys.argv[3]
files = sorted(glob.glob(os.path.join(
    out_dir, ".tmp", "ipc_window.%s.*.%s.json" % (side, pid))))
totals = {}
window_lines = 0
for path in files:
    with open(path, "r", encoding="utf-8") as handle:
        window = json.load(handle)
    window_lines += window.get("window_lines", 0)
    for key, value in (window.get("counters") or {}).items():
        totals[key] = totals.get(key, 0) + value
with open(os.path.join(out_dir, ".tmp",
                       "ipc_mechanism.%s.%s.json" % (side, pid)),
          "w", encoding="utf-8") as handle:
    json.dump({"window_lines": window_lines, "counters": totals}, handle)
PY
done

IPC_MECHANISM_BASELINE="{}"
IPC_MECHANISM_CANDIDATE="{}"
for side in baseline candidate; do
    mechanism_file="$(ls "$OUT"/.tmp/ipc_mechanism.$side.*.json 2>/dev/null | head -1 || true)"
    if [ -z "$mechanism_file" ]; then
        if [ "$REQUIRE_IPC_COUNTERS" = "1" ]; then
            fail_evidence "no IPC mechanism capture for $side diagnostic window"
        fi
        continue
    fi
    mechanism="$(cat "$mechanism_file")"
    if [ "$REQUIRE_IPC_COUNTERS" = "1" ]; then
        window_lines="$(printf '%s' "$mechanism" | python3 -c \
            'import json,sys; print(json.load(sys.stdin).get("window_lines", 0))')"
        if [ "$window_lines" = "0" ]; then
            fail_evidence "no IPC mechanism counters for $side diagnostic window"
        fi
    fi
    if [ "$side" = "baseline" ]; then
        IPC_MECHANISM_BASELINE="$mechanism"
    else
        IPC_MECHANISM_CANDIDATE="$mechanism"
    fi
done

# correctness.json and ready-records.json must be valid JSON arrays.
{
    printf '['
    paste -sd, "$OUT/.tmp/correctness.lines" 2>/dev/null
    printf ']\n'
} >"$OUT/correctness.json"
{
    printf '['
    paste -sd, "$OUT/.tmp/ready.lines" 2>/dev/null
    printf ']\n'
} >"$OUT/ready-records.json"
if ! python3 -c 'import json,sys; json.load(open(sys.argv[1]))' \
    "$OUT/correctness.json" 2>/dev/null; then
    fail_evidence "correctness.json is not valid JSON"
fi
if ! python3 -c 'import json,sys; json.load(open(sys.argv[1]))' \
    "$OUT/ready-records.json" 2>/dev/null; then
    fail_evidence "ready-records.json is not valid JSON"
fi

# Profiles. The baseline gateway is either the Go gateway (writes its own
# pprof) or, with --baseline-host-profile, the capsid-host perf-recorded by
# the runner into baseline-gateway.perf.data.
if [ "$NO_PROFILE" != "1" ]; then
    if [ ! -s "$OUT/baseline-gateway.pprof" ] && \
       [ ! -s "$OUT/baseline-gateway.perf.data" ]; then
        fail_evidence "missing or empty profile: baseline-gateway"
    fi
    # Worker profiles: at least one $side-worker[.N].perf.data must exist
    # and be non-empty; the gateway profiles stay single files.
    for pattern in baseline-worker*.perf.data candidate-worker*.perf.data; do
        if ! ls "$OUT"/$pattern >/dev/null 2>&1; then
            fail_evidence "missing or empty profile: $pattern"
        fi
        for file in "$OUT"/$pattern; do
            if [ ! -s "$file" ]; then
                fail_evidence "missing or empty profile: $(basename "$file")"
            fi
        done
    done
    for file in candidate-host.perf.data; do
        if [ ! -s "$OUT/$file" ]; then
            fail_evidence "missing or empty profile: $file"
        fi
    done
    if ! ls "$OUT"/perf-stat/*.stat >/dev/null 2>&1; then
        fail_evidence "missing perf-stat output"
    fi
fi

# Resource deltas from the profile runs: RSS/PSS deltas (kB) and IPC
# syscall/byte counts from /proc/<pid>/io, per side and process. Computed
# before the report so the report can reference them.
RESOURCE_JSON=""
{
    printf '{'
    first=1
    for f in "$OUT"/.tmp/resource.*.profile.*; do
        [ -f "$f" ] || continue
        side="$(basename "$f" | sed -E 's/resource\.([a-z]+)\..*/\1/')"
        # Labels are gateway plus worker (single) or worker.N (pool shards).
        labels="$(grep -oE '^[a-z0-9.]+' "$f" | sort -u)"
        for label in $labels; do
            before="$(grep "^$label|" "$f" | head -1 || true)"
            after="$(grep "^$label|" "$f" | tail -1 || true)"
            [ -n "$before" ] && [ -n "$after" ] || continue
            delta() {
                echo $(( $(printf '%s' "$after" | cut -d'|' -f"$1") - \
                         $(printf '%s' "$before" | cut -d'|' -f"$1") ))
            }
            [ "$first" -eq 1 ] || printf ','
            first=0
            printf '"%s_%s": {"rss_delta_kb": %s, "pss_delta_kb": %s, "read_syscalls": %s, "write_syscalls": %s, "read_bytes": %s, "write_bytes": %s}' \
                "$side" "$label" "$(delta 2)" "$(delta 3)" "$(delta 4)" \
                "$(delta 5)" "$(delta 6)" "$(delta 7)"
        done
    done
    printf '}'
} >"$OUT/.tmp/resource.json"
RESOURCE_JSON="$(cat "$OUT/.tmp/resource.json" 2>/dev/null || echo '{}')"

set -e

# Manifest fields.
[ "$COMMIT" != "unknown" ] || fail_evidence "commit not resolvable"
[ "$BUILD_ARGS" != "{}" ] || fail_evidence "build arguments not recorded"

# Collect the run metadata for the Python evidence generator (the only
# component allowed to produce manifest.json and report.md).
GENERATED_AT="$NOW"
COMMAND_ARGS="$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1:]))' \
    "${ORIGINAL_ARGV[@]}")"
HOST_BIN_SHA=""
[ -n "$HOST_BIN" ] && HOST_BIN_SHA="$(sha256sum "$HOST_BIN" | cut -d' ' -f1)"
BASELINE_HOST_BIN_SHA=""
[ -n "$BASELINE_HOST_BIN" ] && \
    BASELINE_HOST_BIN_SHA="$(sha256sum "$BASELINE_HOST_BIN" | cut -d' ' -f1)"
CANDIDATE_HOST_BIN_SHA=""
[ -n "$CANDIDATE_HOST_BIN" ] && \
    CANDIDATE_HOST_BIN_SHA="$(sha256sum "$CANDIDATE_HOST_BIN" | cut -d' ' -f1)"
{
    for key in RUN_ID GENERATED_AT COMMIT BUILD_ARGS UNAME NPROC \
        CGROUP_CPU_MAX PERF_PARANOID; do
        eval "printf '%s=%s\\n' \"$key\" \"\${$key}\""
    done
    printf 'COMMAND_ARGS=%s\n' "$COMMAND_ARGS"
    printf 'BASELINE_CMD=%s\nCANDIDATE_CMD=%s\nWORKER_CMD=%s\n' \
        "$BASELINE" "$CANDIDATE" "$WORKER"
    printf 'WORKER_SHA=%s\nBUNDLE_CMD=%s\nBUNDLE_SHA=%s\n' \
        "$WORKER_SHA" "$BUNDLE" "$BUNDLE_SHA"
    printf 'LOADGEN_CMD=%s\nLOADGEN_SHA=%s\n' \
        "$LOADGEN" "$(sha256sum "$LOADGEN" | cut -d' ' -f1)"
    printf 'HOST_BIN_CMD=%s\nHOST_BIN_SHA=%s\n' "$HOST_BIN" "$HOST_BIN_SHA"
    printf 'BASELINE_HOST_BIN_CMD=%s\nBASELINE_HOST_BIN_SHA=%s\n' \
        "$BASELINE_HOST_BIN" "$BASELINE_HOST_BIN_SHA"
    printf 'CANDIDATE_HOST_BIN_CMD=%s\nCANDIDATE_HOST_BIN_SHA=%s\n' \
        "$CANDIDATE_HOST_BIN" "$CANDIDATE_HOST_BIN_SHA"
    printf 'WORKLOAD=%s\nROUNDS=%s\nWARMUP=%s\nDURATION=%s\n' \
        "$WORKLOAD" "$ROUNDS" "$WARMUP" "$DURATION"
    printf 'CONNECTIONS=%s\nINFLIGHT=%s\nCPUSET=%s\n' \
        "$CONNECTIONS" "$INFLIGHT" "$CPUSET"
    printf 'LOADGEN_CPUSET=%s\n' "$LOADGEN_CPUSET"
    printf 'APP=%s\nAUTHORITY=%s\nTIMEOUT_MS=%s\nWINDOW=%s\n' \
        "$APP" "$AUTHORITY" "$TIMEOUT_MS" "$WINDOW"
    printf 'TCP_NODELAY=%s\nTEST_MODE=%s\n' "$TCP_NODELAY_STATE" "$TEST_MODE"
    printf 'BASELINE_ENV=%s\nCANDIDATE_ENV=%s\n' "$BASELINE_ENV" "$CANDIDATE_ENV"
    printf 'BASELINE_WORKERS=%s\nCANDIDATE_WORKERS=%s\n' \
        "$BASELINE_WORKERS" "$CANDIDATE_WORKERS"
    printf 'BASELINE_HOST_PROFILE=%s\n' "$BASELINE_HOST_PROFILE"
    printf 'STATISTIC=%s\nREQUIRE_IPC_COUNTERS=%s\n' \
        "$STATISTIC" "$REQUIRE_IPC_COUNTERS"
    printf 'IPC_MECHANISM_BASELINE=%s\n' "$IPC_MECHANISM_BASELINE"
    printf 'IPC_MECHANISM_CANDIDATE=%s\n' "$IPC_MECHANISM_CANDIDATE"
    printf 'EVIDENCE_STATUS=%s\nINCOMPLETE_REASONS=%s\n' \
        "$EVIDENCE_STATUS" "$INCOMPLETE_REASONS"
    printf 'UNSUPPORTED_COUNTERS=%s\n' \
        "$(grep -h '<not supported>' "$OUT"/perf-stat/*.stat 2>/dev/null \
            | sed 's/^ *//' | sort -u | paste -sd';' -)"
} >"$OUT/.tmp/meta.env"

python3 "$SCRIPT_DIR/evidence.py" "$OUT" \
    || { echo "run-ab: evidence generation failed" >&2; exit 1; }

# report.md must exist before the manifest's file digests are computed.
[ -s "$OUT/report.md" ] || fail_evidence "report.md missing"
[ -s "$OUT/manifest.json" ] || fail_evidence "manifest.json missing"

if [ "$EVIDENCE_STATUS" = "incomplete" ]; then
    echo "run-ab: INCOMPLETE_EVIDENCE: $INCOMPLETE_REASONS" >&2
    exit 2
fi

echo "run-ab: complete evidence for $RUN_ID under $OUT"
exit 0
