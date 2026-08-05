#!/usr/bin/env bash
# Benchmarks-only static-pool wrapper (NOT a managed production path). Same
# CAPSID_BENCH_* contract as run-host.sh, plus CAPSID_BENCH_WORKERS for the
# fixed pool size (1/2/4): the runner's multi-worker profile binds one perf
# stream per pool shard, so the wrapper must exec capsid-host's
# --mode static-pool with the same worker/bundle/identity parameters the
# single-worker wrapper uses.
set -euo pipefail

identity_out="${CAPSID_BENCH_IDENTITY_OUT:-}"
if [ -n "$identity_out" ]; then
    bundle_sha="$(sha256sum "${CAPSID_BENCH_BUNDLE:?}" | cut -d' ' -f1)"
    worker_sha="$(sha256sum "${CAPSID_BENCH_WORKER:?}" | cut -d' ' -f1)"
    printf '{"bundle_sha256":"%s","worker_sha256":"%s"}\n' \
        "$bundle_sha" "$worker_sha" >"$identity_out"
fi

workers="${CAPSID_BENCH_WORKERS:-1}"
case "$workers" in
1|2|4|6|8) ;;
*) echo "run-static-pool-host: CAPSID_BENCH_WORKERS must be 1, 2, 4, 6 or 8 (got $workers)" >&2; exit 2 ;;
esac

exec "${CAPSID_BENCH_HOST_BIN:?}" \
    --mode static-pool \
    --workers "$workers" \
    --worker "${CAPSID_BENCH_WORKER:?}" \
    --source-bundle "${CAPSID_BENCH_BUNDLE:?}" \
    --source-name "file://${CAPSID_BENCH_BUNDLE}" \
    --application "${CAPSID_BENCH_APPLICATION:-orders}" \
    --listen "${CAPSID_BENCH_LISTEN:-127.0.0.1:0}" \
    --routing path \
    --public-scheme http \
    --public-authority "${CAPSID_BENCH_PUBLIC_AUTHORITY:-public.example}" \
    --request-timeout-ms "${CAPSID_BENCH_TIMEOUT_MS:-10000}" \
    --initial-stream-window "${CAPSID_BENCH_WINDOW:-1024}" \
    --strict-sandbox off \
    --ready-fd "${CAPSID_BENCH_READY_FD:-3}"
