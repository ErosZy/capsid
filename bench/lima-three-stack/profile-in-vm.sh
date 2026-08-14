#!/usr/bin/env bash
set -euo pipefail

# Run inside the Lima/Colima VM. Captures host + worker userspace stacks for
# one matrix cell while the ordinary correctness-checking load generator runs.
IMAGE_TAG=${IMAGE_TAG:-baseline}
WORKLOAD=${WORKLOAD:-matrix-json-1k}
CONCURRENCY=${CONCURRENCY:-64}
WARMUP_S=${WARMUP_S:-2}
DURATION_S=${DURATION_S:-12}
PROFILE_S=${PROFILE_S:-6}
SERVICE_CPUS=${SERVICE_CPUS:-0-1}
LOADGEN_CPUS=${LOADGEN_CPUS:-2-3}
OUT=${OUT:?OUT is required}
NETWORK=capsid-bench-net
SERVICE=capsid-profile-service
LOADGEN=capsid-profile-loadgen

mkdir -p "$OUT"
chmod 0777 "$OUT"
sudo docker network inspect "$NETWORK" >/dev/null 2>&1 ||
    sudo docker network create "$NETWORK" >/dev/null

cleanup() {
    sudo docker rm -f "$LOADGEN" "$SERVICE" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
cleanup

sudo docker run -d --name "$SERVICE" --network "$NETWORK" --network-alias service \
    --cpuset-cpus "$SERVICE_CPUS" --memory 2g --memory-swap 2g \
    --pids-limit 512 --ulimit nofile=1048576:1048576 \
    -e CAPSID_WORKERS=4 -e CAPSID_STREAM_WINDOW=65536 \
    "capsid-bench/capsid:$IMAGE_TAG" >/dev/null

service_ip=$(sudo docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$SERVICE")
for _ in $(seq 1 200); do
    if curl -fsS --max-time 1 \
        "http://$service_ip:8080/@capsid/orders/bench/matrix-json-1k" \
        -o /dev/null 2>/dev/null; then
        break
    fi
    sleep 0.05
done

sudo docker run --rm --name "$LOADGEN" --network "$NETWORK" \
    --cpuset-cpus "$LOADGEN_CPUS" --memory 512m --memory-swap 512m \
    --pids-limit 512 --ulimit nofile=1048576:1048576 \
    -v "$OUT:/results" \
    -e CAPSID_BENCH_TARGET=http://service:8080 \
    -e CAPSID_BENCH_WORKLOAD="$WORKLOAD" \
    -e CAPSID_BENCH_SIDE="$IMAGE_TAG" -e CAPSID_BENCH_ROUND=1 \
    -e CAPSID_BENCH_WARMUP_S="$WARMUP_S" \
    -e CAPSID_BENCH_DURATION_S="$DURATION_S" \
    -e CAPSID_BENCH_CONNECTIONS="$CONCURRENCY" \
    -e CAPSID_BENCH_INFLIGHT="$CONCURRENCY" \
    -e CAPSID_BENCH_SAMPLES_OUT=/results/samples.jsonl \
    -e CAPSID_BENCH_CORRECTNESS_OUT=/results/correctness.jsonl \
    -e CAPSID_BENCH_PHASE_OUT=/results/measured.phase \
    capsid-bench/loadgen:local >"$OUT/loadgen.log" 2>&1 &
loadgen_pid=$!

while [[ ! -f "$OUT/measured.phase" ]] && kill -0 "$loadgen_pid" 2>/dev/null; do
    sleep 0.01
done
[[ -f "$OUT/measured.phase" ]]

mapfile -t service_pids < <(sudo docker top "$SERVICE" -eo pid | awk 'NR > 1 {print $1}')
pid_csv=$(IFS=,; echo "${service_pids[*]}")
printf '%s\n' "${service_pids[@]}" >"$OUT/pids.txt"

sudo perf record -q -e cpu-clock -F 299 -g --call-graph dwarf,8192 \
    -p "$pid_csv" -o "$OUT/perf.data" -- sleep "$PROFILE_S" \
    2>"$OUT/perf-record.log"
sudo perf report --stdio --no-children --no-inline --no-demangle \
    --call-graph none --sort comm,dso,symbol --percent-limit 0.25 \
    -i "$OUT/perf.data" >"$OUT/perf-report-flat.txt" \
    2>"$OUT/perf-report-flat.log"
sudo perf report --stdio --no-children --no-inline --no-demangle \
    --percent-limit 0.5 -i "$OUT/perf.data" \
    >"$OUT/perf-report-callgraph.txt" 2>"$OUT/perf-report-callgraph.log"

wait "$loadgen_pid"
sudo docker logs "$SERVICE" >"$OUT/service.log" 2>&1 || true
sudo docker inspect "$SERVICE" >"$OUT/service-inspect.json"

echo "profile: $OUT"
