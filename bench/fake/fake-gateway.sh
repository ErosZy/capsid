#!/usr/bin/env bash
# Fake gateway for the benchmark runner RED test (host_single_worker_ab_emits_complete_evidence).
#
# It honors the same component contract as the real gateways (bench/wrappers/run-host.sh and
# bench/baseline-gateway): the runner sets CAPSID_BENCH_* environment variables, the gateway
# writes one canonical ready record to the ready fd, and in profile runs it writes a CPU
# profile file at $CAPSID_BENCH_CPU_PROFILE. It never serves traffic — the fake loadgen writes
# its own samples — but it does spawn the fake worker so the runner's worker-profile lookup
# finds a process to attach to.
#
# Behavior switches (RED scenarios):
#   CAPSID_BENCH_FAKE_BAD_IDENTITY=1  -> ready record reports a bundle SHA-256 that does not
#                                       match the bundle the runner handed out (runner must
#                                       reject the A/B identity mismatch).
#   CAPSID_BENCH_FAKE_NO_PROFILE=1   -> profile runs write nothing (runner must reject the
#                                       missing profile).
set -euo pipefail

ready_fd="${CAPSID_BENCH_READY_FD:-3}"
port="${CAPSID_BENCH_FAKE_PORT:-41234}"
side="${CAPSID_BENCH_SIDE:-baseline}"

# The real gateways spawn the worker themselves; the fake simulates that so the
# runner's worker-profile lookup (pgrep by name) has a process to attach to.
"${CAPSID_BENCH_WORKER:?}" &

# The runner computes the bundle/worker SHA-256 from the files it hands out and
# rejects a ready record that reports anything else.
bundle_sha="$(sha256sum "${CAPSID_BENCH_BUNDLE:?}" | cut -d' ' -f1)"
worker_sha="$(sha256sum "${CAPSID_BENCH_WORKER:?}" | cut -d' ' -f1)"
if [ "${CAPSID_BENCH_FAKE_BAD_IDENTITY:-0}" = "1" ]; then
    bundle_sha="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
fi

# Identity contract: written before READY (CAPSID_BENCH_IDENTITY_OUT),
# unless the scenario asks for a missing report.
if [ -n "${CAPSID_BENCH_IDENTITY_OUT:-}" ] && \
    [ "${CAPSID_BENCH_FAKE_NO_IDENTITY:-0}" != "1" ]; then
    printf '{"bundle_sha256":"%s","worker_sha256":"%s"}\n' \
        "$bundle_sha" "$worker_sha" >"${CAPSID_BENCH_IDENTITY_OUT}"
fi

printf '{"schema":"bench-ready-v1","address":"127.0.0.1","port":%s,"bundle_sha256":"%s","worker_sha256":"%s"}\n' \
    "$port" "$bundle_sha" "$worker_sha" >&"$ready_fd"

if [ -n "${CAPSID_BENCH_CPU_PROFILE:-}" ] && [ "${CAPSID_BENCH_FAKE_NO_PROFILE:-0}" != "1" ]; then
    printf 'fake gateway pprof placeholder (%s)\n' "$side" >"${CAPSID_BENCH_CPU_PROFILE}"
fi

# CAPSID_BENCH_FAKE_METRICS=1: emit IPC mechanism counter lines (same
# schema as the real capsid-host under CAPSID_HOST_IPC_METRICS=1) so the
# runner's measured-window counter capture has something to sum. Written to
# stderr, matching the real host.
metrics_pid=""
if [ "${CAPSID_BENCH_FAKE_METRICS:-0}" = "1" ]; then
    emit_metrics() {
        while :; do
            printf '%s\n' '{"host":{"commands_submitted":2,"command_batches":1,"commands_executed":2,"flush_calls":1,"flush_eagain":0,"events_queued":1,"asio_posts":1,"response_heads":1,"response_body_frames":1,"response_ends":1,"grant_commands":1,"credit_bytes_granted":1024,"credit_stall_count":0,"command_queue_hw":1,"event_queue_hw":1},"client":{"queued_frames":2,"queued_wire_bytes":128,"queue_would_block":0,"flush_calls":1,"socket_write_calls":1,"socket_write_bytes":128,"socket_write_eagain":0,"next_event_calls":1,"parsed_frames":2,"parser_payload_copied_bytes":64,"socket_read_calls":1,"socket_read_bytes":128,"socket_read_eagain":0,"queued_bytes_hw":128}}' >&2
            sleep 0.1
        done
    }
    emit_metrics &
    metrics_pid=$!
fi

trap 'kill "${metrics_pid:-}" 2>/dev/null || true' EXIT
sleep 600
