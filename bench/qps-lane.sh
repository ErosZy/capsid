#!/usr/bin/env bash
# Owned QPS controller; restricted observations cannot qualify publication.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${CAPSID_BENCH_PYTHON:-python3}" -B "$SCRIPT_DIR/qps-lane.py" "$@"
