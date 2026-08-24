#!/usr/bin/env bash
# Direct A/B: BC27 ext34 fusion (ext ids 2/3) vs deployed BC26, through
# the classic-script corpus (task #75e). Both arms compile from the SAME
# source with the SAME classic-bytecode binary (46-patch overlay: the
# dual reader + ext 2/3 handlers); the arms differ only in the pass
# mask: control = 0x7f (deployed kPassAll), candidate = 0x1ff (kPassAll
# | kPassExtFuse34 | kPassExtFuse4). Paired ABBA/BAAB on the pinned
# cpuset via bench/classic-suite-ab.py; the runner records raw samples
# and a paired-log-ratio summary but never decides the gate.
#
# Programs: the task #75e list — kraken audio-beat-detection/audio-fft/
# audio-oscillator/imaging-darkroom, octane gameboy/navier-stokes/
# richards/box2d.
#
# Usage: bash bench/ext34-classic-ab.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/ext34-classic-ab-$(date +%Y%m%dT%H%M%S)}
CORPUS=${CORPUS:-/tmp/capsid-suite-corpus-v1}
TOOL=${TOOL:-bench/bin/classic-bytecode}
CPUSET=${CPUSET:-2-3}
PAIRS=${PAIRS:-7}
PROGRAMS=${PROGRAMS:-"audio-beat-detection audio-fft audio-oscillator \
imaging-darkroom gameboy navier-stokes richards box2d"}

[ -x "$TOOL" ] || { echo "missing required binary: $TOOL" >&2; exit 1; }
mkdir -p "$OUT"

echo "cpuset: $CPUSET pairs: $PAIRS" | tee "$OUT/run-args.txt"
{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "worktree_dirty: $(test -n "$(git status --porcelain)" && echo true || echo false)"
    echo "tool_sha256: $(sha256sum "$TOOL" | cut -d' ' -f1)"
    echo "corpus_manifest_sha256: $(sha256sum "$CORPUS/manifest.json" | cut -d' ' -f1)"
    echo "control_passes: 0x7f (deployed kPassAll, BC26)"
    echo "candidate_passes: 0x1ff (kPassAll | kPassExtFuse34 | kPassExtFuse4, BC27)"
    echo "command: OUT=$OUT CORPUS=$CORPUS TOOL=$TOOL CPUSET=$CPUSET PAIRS=$PAIRS bash $0"
} > "$OUT/manifest.txt"

args=(--corpus "$CORPUS" --out "$OUT" --control-tool "$TOOL"
      --control-passes 0x7f --candidate-passes 0x1ff
      --pairs "$PAIRS" --cpuset "$CPUSET")
for p in $PROGRAMS; do
    args+=(--program "$p")
done
python3 bench/classic-suite-ab.py "${args[@]}"
