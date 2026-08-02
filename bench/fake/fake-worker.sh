#!/usr/bin/env bash
# Fake worker for the benchmark runner RED test. It is spawned by the fake
# gateway (which plays the role of the real gateway/host spawning its worker)
# and simply stays alive long enough for the runner to attach perf. Note:
# no `exec` — the runner finds the worker by matching its command line, and
# `exec sleep` would replace the script name with "sleep 600".
set -euo pipefail

sleep 600
