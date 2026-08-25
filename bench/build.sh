#!/usr/bin/env bash
# Builds the benchmark components into bench/bin/:
#   - bench/bin/loadgen            (Go, no external dependencies)
#   - bench/bin/baseline-gateway   (Go + cgo over the capsid_worker ABI)
#   - bench/bin/bytecode-raw       (Step 8: rewriter-free bytecode generator)
#   - bench/bin/exec-throughput    (Step 8: three-state timing harness)
#   - bench/bin/analyze            (Step 8: static ceiling re-measurement)
#   - bench/bin/classic-bytecode   (classic-suite raw/rewritten compiler+runner)
# The candidate side is bench/wrappers/run-host.sh with CAPSID_BENCH_HOST_BIN
# pointing at the built capsid-host; the worker is the built capsid-worker.
# Every component's SHA-256 is recorded by the runner in manifest.json.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/bin"
BUILD_LINUX="${BUILD_LINUX:-$SCRIPT_DIR/../build-release}"

mkdir -p "$BIN"

echo "==> loadgen"
if command -v go >/dev/null 2>&1; then
    (cd "$SCRIPT_DIR/loadgen" && go build -o "$BIN/loadgen" .)
else
    echo "    (skipped: go not installed)"
fi

echo "==> baseline-gateway (cgo capsid_worker)"
if command -v go >/dev/null 2>&1; then
    (cd "$SCRIPT_DIR/baseline-gateway" \
        && CGO_CFLAGS="-I$SCRIPT_DIR/../include" \
           CGO_LDFLAGS="-L$BUILD_LINUX" \
           go build -o "$BIN/baseline-gateway" .)
else
    echo "    (skipped: go not installed)"
fi

echo "==> bytecode-raw (Step 8; links the vendored quickjs)"
g++ -O2 -std=c++20 \
    -I"$BUILD_LINUX/txiki-build/deps/quickjs" \
    -I"$SCRIPT_DIR/../vendor/txiki.js/deps/quickjs" \
    "$SCRIPT_DIR/bytecode-raw.cc" -o "$BIN/bytecode-raw" \
    -L"$BUILD_LINUX/txiki-build/deps/quickjs" -lqjs

echo "==> exec-throughput + analyze (Step 8; link capsid_bytecode_rewriter)"
g++ -O2 -std=c++20 -I"$SCRIPT_DIR/../include" -I"$SCRIPT_DIR/../src" \
    "$SCRIPT_DIR/exec-throughput.cc" -o "$BIN/exec-throughput" \
    -L"$BUILD_LINUX" -lcapsid_runtime -pthread
# analyze compiles the rewriter sources directly, so it must see the
# overlay quickjs headers (the same include the CMake target uses). The IR
# sources are linked so the analyze-only CFG, SSA and region entry points are
# available to the benchmark tool.
g++ -O2 -std=c++20 -I"$SCRIPT_DIR/../src" \
    -I"$BUILD_LINUX/vendor-overlay/txiki.js/deps/quickjs" \
    "$SCRIPT_DIR/analyze.cc" \
    "$SCRIPT_DIR/../src/bytecode_rewriter/bytecode_rewriter.cc" \
    "$SCRIPT_DIR/../src/bytecode_rewriter/ir/cfg.cc" \
    "$SCRIPT_DIR/../src/bytecode_rewriter/ir/ssa.cc" \
    "$SCRIPT_DIR/../src/bytecode_rewriter/ir/region.cc" \
    "$SCRIPT_DIR/../src/bytecode_rewriter/ir/effects.cc" \
    -o "$BIN/analyze"

echo "==> classic-bytecode (classic-suite compiler + runner)"
g++ -O2 -std=c++20 \
    -I"$SCRIPT_DIR/../src" \
    -I"$BUILD_LINUX/vendor-overlay/txiki.js/deps/quickjs" \
    "$SCRIPT_DIR/classic-bytecode.cc" -o "$BIN/classic-bytecode" \
    -L"$BUILD_LINUX" -lcapsid_bytecode_rewriter \
    -L"$BUILD_LINUX/txiki-build/deps/quickjs" -lqjs

echo "==> components"
ls -l "$BIN"
for bin in "$BIN"/*; do
    [ -f "$bin" ] && sha256sum "$bin"
done
