#!/usr/bin/env bash
# Direct compiled-OFF tax probe for ext34. Both tools compile and execute the
# deployed 0x7f pass mask. The runner additionally requires every control and
# candidate bytecode blob to be identical, so any measured difference is in
# the two runtime binaries rather than the serialized program.
#
# CONTROL_TOOL must be built from the patchless/pre-0045 tree. CANDIDATE_TOOL
# must be built from the ext34-capable tree with CONFIG_EXT_FUSION34 compiled
# in, while the pass mask remains 0x7f for both arms.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/ext34-off-tax-ab-$(date +%Y%m%dT%H%M%S)}
CORPUS=${CORPUS:-/tmp/capsid-suite-corpus-v1}
CONTROL_TOOL=${CONTROL_TOOL:-bench/bin/classic-bytecode-pre45}
CANDIDATE_TOOL=${CANDIDATE_TOOL:-bench/bin/classic-bytecode-ext34}
CPUSET=${CPUSET:-2-3}
PAIRS=${PAIRS:-7}
PROGRAMS=${PROGRAMS:-"audio-beat-detection audio-fft audio-oscillator \
imaging-darkroom gameboy navier-stokes richards box2d"}

for tool in "$CONTROL_TOOL" "$CANDIDATE_TOOL"; do
    [ -x "$tool" ] || { echo "missing required binary: $tool" >&2; exit 1; }
done
mkdir -p "$OUT"

args=(--corpus "$CORPUS" --out "$OUT"
      --control-tool "$CONTROL_TOOL" --candidate-tool "$CANDIDATE_TOOL"
      --control-passes 0x7f --candidate-passes 0x7f
      --require-bytecode-identical
      --pairs "$PAIRS" --cpuset "$CPUSET")
for program in $PROGRAMS; do
    args+=(--program "$program")
done
python3 bench/classic-suite-ab.py "${args[@]}"
