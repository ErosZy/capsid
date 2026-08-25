#!/usr/bin/env bash
# Balanced A/B over statically bundled V8 Web Tooling Benchmark workloads.
# Generate CORPUS first with bench/prepare-web-tooling.py. Each process runs
# one upstream workload call; classic-suite-ab.py supplies fresh-runtime
# repetitions, correctness smoke checks, bytecode identities, and CIs.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/web-tooling-ab-$(date +%Y%m%dT%H%M%S)}
CORPUS=${CORPUS:-/tmp/capsid-web-tooling-corpus-v1}
CONTROL_TOOL=${CONTROL_TOOL:-bench/bin/classic-bytecode}
CANDIDATE_TOOL=${CANDIDATE_TOOL:-$CONTROL_TOOL}
CONTROL_PASSES=${CONTROL_PASSES:-0}
CANDIDATE_PASSES=${CANDIDATE_PASSES:-0x7f}
CPUSET=${CPUSET:-2}
PAIRS=${PAIRS:-3}
TIMEOUT=${TIMEOUT:-900}
PROGRAMS=${PROGRAMS:-}
SMOKE_ONLY=${SMOKE_ONLY:-0}

[ -x "$CONTROL_TOOL" ] || {
    echo "missing control binary: $CONTROL_TOOL" >&2
    exit 1
}
[ -x "$CANDIDATE_TOOL" ] || {
    echo "missing candidate binary: $CANDIDATE_TOOL" >&2
    exit 1
}
[ -f "$CORPUS/manifest.json" ] || {
    echo "missing corpus manifest: $CORPUS/manifest.json" >&2
    exit 1
}

args=(--corpus "$CORPUS" --out "$OUT"
      --control-tool "$CONTROL_TOOL" --candidate-tool "$CANDIDATE_TOOL"
      --control-passes "$CONTROL_PASSES"
      --candidate-passes "$CANDIDATE_PASSES"
      --pairs "$PAIRS" --cpuset "$CPUSET" --timeout "$TIMEOUT")
if [ "$SMOKE_ONLY" = 1 ]; then
    args+=(--smoke-only)
fi
for program in $PROGRAMS; do
    args+=(--program "$program")
done

python3 bench/classic-suite-ab.py "${args[@]}"
