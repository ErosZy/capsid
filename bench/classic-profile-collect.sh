#!/usr/bin/env bash
# Full-suite opcode profile collection (task #74, corrected run).
# Profiles the DEPLOYED pipeline: the compiler arm is the production
# build (bench/bin/classic-bytecode) with the deployed kPassAll mask
# (0x7f, BC26 output), so the histogram measures where the shipped
# bytecode spends its time — the input to candidate ranking. Execution
# uses a separate CONFIG_OPCODE_PROFILE runner (bench/bin/
# classic-bytecode-profile) so instrumented timing is never a
# performance result.
#
# The earlier collection (bench/results/classic-profile-20260825)
# failed two ways: --profile-tool was the non-profile binary (every
# program: "--opcode-profile requires a CONFIG_OPCODE_PROFILE build"),
# and --passes 0xffffffff profiled the candidate (BC27 output, which
# the 45-patch profile qjs cannot read) instead of the deployed 0x7f.
#
# Usage: bash bench/classic-profile-collect.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/classic-profile-$(date +%Y%m%dT%H%M%S)}
CORPUS=${CORPUS:-/tmp/capsid-suite-corpus-v1}
COMPILER=${COMPILER:-bench/bin/classic-bytecode}
PROFILE_TOOL=${PROFILE_TOOL:-bench/bin/classic-bytecode-profile}
CPUSET=${CPUSET:-2-3}
TIMEOUT=${TIMEOUT:-600}
mkdir -p "$OUT"

for bin in "$COMPILER" "$PROFILE_TOOL"; do
    [ -x "$bin" ] || { echo "missing required binary: $bin" >&2; exit 1; }
done

# Fail fast if the profile tool is not a CONFIG_OPCODE_PROFILE build
# (the earlier collection's exact mistake): a real one-shot profile
# must succeed. The probe uses the pinned cpuset, matching the run.
probe=/tmp/capsid-profile-probe.qjsb
probe_out="$OUT/.probe.profile.jsonl"
# The probe must be a classic script (the production compiler rejects
# module syntax): the -rt.js fixtures are modules, so use a small
# corpus file instead.
"$COMPILER" compile --input "$CORPUS/sunspider-math-partial-sums.js" \
    --source-name file:///probe.js --output "$probe" \
    --optimize --passes 0x7f 2>"$OUT/.probe.compile.err"
if ! taskset -c "$CPUSET" "$PROFILE_TOOL" run --input "$probe" \
        --warmup 0 --rounds 1 --opcode-profile "$probe_out" \
        >"$OUT/.probe.run.out" 2>"$OUT/.probe.run.err"; then
    echo "profile tool failed the probe (not a CONFIG_OPCODE_PROFILE" >&2
    echo "build?): $PROFILE_TOOL" >&2
    cat "$OUT/.probe.run.err" >&2
    exit 1
fi
[ -s "$probe_out" ] || { echo "probe produced an empty profile" >&2; exit 1; }
rm -f "$probe" "$probe_out" "$OUT/.probe.compile.err" \
      "$OUT/.probe.run.out" "$OUT/.probe.run.err"

echo "cpuset: $CPUSET timeout: $TIMEOUT" | tee "$OUT/run-args.txt"
python3 bench/classic-suite-profile.py \
    --corpus "$CORPUS" --out "$OUT" \
    --compiler "$COMPILER" --profile-tool "$PROFILE_TOOL" \
    --passes 0x7f --cpuset "$CPUSET" --timeout "$TIMEOUT"
