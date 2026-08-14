#!/usr/bin/env bash
# Sample PSS/RSS/CPU of the three SUT stacks while a three-stack run is in
# progress. Attaches to the running processes (does not start anything).
#
#   bench/sample-sut-memory.sh <outdir> [interval_s] [max_samples]
#
# Emits <outdir>/sut-resources.jsonl, one JSON line per stack per tick:
#   {"ts":...,"stack":"capsid|php|python","pids":N,"pss_kb":...,"rss_kb":...,
#    "cpu_pct":...}
# cpu_pct is the stack's process-tree CPU share over the previous tick
# (utime+stime delta / wall delta). PSS via /proc/PID/smaps_rollup; php runs
# in a docker container whose processes are visible in the host /proc.
set -euo pipefail
OUT=${1:?usage: sample-sut-memory.sh <outdir> [interval_s]}
INTERVAL=${2:-15}
MAX=${3:-0}
mkdir -p "$OUT"

identify() {
    # prints: stack<TAB>pid
    pgrep -f 'capsid-host --mode static-pool' | awk '{print "capsid\t" $1}'
    pgrep -f 'capsid-worker --ipc-fd' | awk '{print "capsid\t" $1}'
    pgrep -f 'gunicorn --workers' | awk '{print "python\t" $1}'
    pgrep -f 'php-fpm: (master|pool)' | awk '{print "php\t" $1}'
    pgrep -f 'nginx: (master|worker)' | awk '{print "php\t" $1}'
}

# smaps_rollup is ptrace-protected across users; statm is always readable.
pss_kb() { awk '/^Pss:/ {s += $2} END {print s + 0}' "$1" 2>/dev/null || echo 0; }
rss_kb() {
    awk '{print $2 * 4}' "$1" 2>/dev/null || echo 0
}

declare -A TICKS
tick_us() { date +%s%N; }

OUTFILE="$OUT/sut-resources.jsonl"
echo "interval_s=$INTERVAL" >> "$OUT/manifest.txt" 2>/dev/null || true
n=0
while true; do
    t0=$(tick_us)
    for stack in capsid php python; do
        pids=$(identify | awk -F'\t' -v s="$stack" '$1 == s {print $2}')
        if [ -z "$pids" ]; then continue; fi
        pss=0; rss=0; cpu=0
        for pid in $pids; do
            [ -r "/proc/$pid/statm" ] || continue
            rss=$(( rss + $(rss_kb "/proc/$pid/statm") ))
            [ -r "/proc/$pid/smaps_rollup" ] && \
                pss=$(( pss + $(pss_kb "/proc/$pid/smaps_rollup") ))
            st=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null || echo 0)
            key="$stack$pid"
            if [ -n "${TICKS[$key]:-}" ]; then
                cpu=$(( cpu + st - ${TICKS[$key]} ))
            fi
            TICKS[$key]=$st
        done
        nproc=$(echo "$pids" | wc -l)
        printf '{"ts":%s,"stack":"%s","pids":%s,"pss_kb":%s,"rss_kb":%s,"cpu_ticks_delta":%s}\n' \
            "$t0" "$stack" "$nproc" "$pss" "$rss" "$cpu" >> "$OUTFILE"
    done
    n=$(( n + 1 ))
    [ "$MAX" -gt 0 ] && [ "$n" -ge "$MAX" ] && break
    sleep "$INTERVAL"
done
echo "sampler done: $n ticks in $OUTFILE"
