#!/usr/bin/env bash
# Builds the M1B benchmark components into bench/bin/:
#   - bench/bin/loadgen            (Go, no external dependencies)
#   - bench/bin/baseline-gateway   (Go + cgo over the capsid_worker ABI)
# The candidate side is bench/wrappers/run-host.sh with CAPSID_BENCH_HOST_BIN
# pointing at the built capsid-host; the worker is the built capsid-worker.
# Every component's SHA-256 is recorded by the runner in manifest.json.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/bin"
BUILD_LINUX="${BUILD_LINUX:-$SCRIPT_DIR/../build-linux}"

mkdir -p "$BIN"

echo "==> loadgen"
(cd "$SCRIPT_DIR/loadgen" && go build -o "$BIN/loadgen" .)

echo "==> baseline-gateway (cgo capsid_worker)"
(cd "$SCRIPT_DIR/baseline-gateway" \
    && CGO_CFLAGS="-I$SCRIPT_DIR/../include" \
       CGO_LDFLAGS="-L$BUILD_LINUX" \
       go build -o "$BIN/baseline-gateway" .)

echo "==> components"
ls -l "$BIN"
sha256sum "$BIN/loadgen" "$BIN/baseline-gateway"
