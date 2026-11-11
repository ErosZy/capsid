#!/usr/bin/env bash
# The Python control layer owns port leases, complete process trees and probe
# results directly. Formal runs require CAPSID_BENCH_METADATA/--metadata;
# restricted smoke runs must explicitly select --observation.
set -euo pipefail
cd "$(dirname "$0")/.."
exec "${CAPSID_BENCH_PYTHON:-python3}" -B bench/density-lane.py "$@"
