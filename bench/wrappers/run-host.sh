#!/usr/bin/env bash
# Wraps the real capsid-host binary for the benchmark runner. The runner talks
# to every component through CAPSID_BENCH_* environment variables; this script
# maps them onto the frozen M1A single-worker CLI and reports the identity
# contract (the bundle/worker SHA-256 it is about to load) before exec.
set -euo pipefail

identity_out="${CAPSID_BENCH_IDENTITY_OUT:-}"
if [ -n "$identity_out" ]; then
    bundle_sha="$(sha256sum "${CAPSID_BENCH_BUNDLE:?}" | cut -d' ' -f1)"
    worker_sha="$(sha256sum "${CAPSID_BENCH_WORKER:?}" | cut -d' ' -f1)"
    printf '{"bundle_sha256":"%s","worker_sha256":"%s"}\n' \
        "$bundle_sha" "$worker_sha" >"$identity_out"
fi

exec "${CAPSID_BENCH_HOST_BIN:?}" \
    --mode single-worker \
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
