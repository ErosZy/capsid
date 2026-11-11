#!/usr/bin/env bash
# Fake worker for the benchmark runner RED test. It is spawned by the fake
# gateway (which plays the role of the real gateway/host spawning its worker)
# and simply stays alive long enough for the runner to attach perf. Note:
# no `exec` — the runner finds the worker by matching its command line, and
# `exec sleep` would replace the script name with "sleep 600".
set -euo pipefail

sleep_pid=""
cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$sleep_pid" ]]; then
        kill "$sleep_pid" 2>/dev/null || true
        wait "$sleep_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM
sleep 600 &
sleep_pid=$!
wait "$sleep_pid" 2>/dev/null || true
