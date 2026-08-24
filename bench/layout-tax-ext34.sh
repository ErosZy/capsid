#!/usr/bin/env bash
# Layout-tax probe for the BC27 ext34 fusion foundation (task #75d).
# Arms: the pre-ext34 pipeline (a00145a build: 45-patch overlay, no
# ext34 matcher) vs the post-ext34 pipeline (current HEAD: 46-patch
# overlay with ext ids 2/3 handlers, run-based matcher). Both compile
# every bench fixture with the deployed kPassAll (== 0x7f: all v1/v2
# passes, ext bits 7/8 excluded), so both arms must emit byte-identical
# BC26 bundles. A single differing byte means the ext34 infrastructure
# (overlay patch 0045 + matcher code) perturbs the BC26 pipeline
# (layout/observer tax) and must be fixed before any direct A/B.
#
# Usage: bash bench/layout-tax-ext34.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/layout-tax-ext34-$(date +%Y%m%dT%H%M%S)}
ARM_A=${ARM_A:-build-release/capsid-bytecode-compile}  # pre-ext34 (a00145a)
ARM_B=${ARM_B:-build-dev/capsid-bytecode-compile}      # post-ext34 (HEAD)
FIXTURES=${FIXTURES:-$(printf '%s\n' bench/fixtures/*.js | sort)}
mkdir -p "$OUT"
for bin in "$ARM_A" "$ARM_B"; do
    [ -x "$bin" ] || { echo "missing required binary: $bin" >&2; exit 1; }
done

sha256sum "$ARM_A" "$ARM_B" > "$OUT/arm-sha256.txt"
{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "worktree_dirty: $(test -n "$(git status --porcelain)" && echo true || echo false)"
    echo "runner_sha256: $(sha256sum "$0" | cut -d' ' -f1)"
    echo "command: OUT=$OUT ARM_A=$ARM_A ARM_B=$ARM_B bash $0"
    echo "uname: $(uname -a)"
    echo "arm_a_commit_hint: a00145a (pre-ext34: 45-patch overlay, no matcher)"
    echo "arm_b_commit_hint: aa450f1 (post-ext34: 46-patch overlay, run-based matcher)"
} > "$OUT/manifest.txt"
cat "$OUT/arm-sha256.txt" >> "$OUT/manifest.txt"

pass=0
fail=0
for src in $FIXTURES; do
    name=$(basename "$src" .js)
    source_name="file:///app/$name.js"
    common=(--source "$src" --source-name "$source_name"
            --application "bench" --version "bench-1" --key-id "bench-key")
    "$ARM_A" "${common[@]}" \
        --bytecode-out "$OUT/$name.a.qjsb" --attestation-out "$OUT/$name.a.att.json" \
        --signing-message-out "$OUT/$name.a.sig.bin" 2>"$OUT/$name.a.err"
    "$ARM_B" "${common[@]}" \
        --bytecode-out "$OUT/$name.b.qjsb" --attestation-out "$OUT/$name.b.att.json" \
        --signing-message-out "$OUT/$name.b.sig.bin" 2>"$OUT/$name.b.err"
    if cmp -s "$OUT/$name.a.qjsb" "$OUT/$name.b.qjsb"; then
        printf '%-24s identical (size %d)\n' "$name" "$(stat -c%s "$OUT/$name.a.qjsb")" \
            | tee -a "$OUT/result.txt"
        pass=$((pass + 1))
    else
        printf '%-24s DIFF (a=%d b=%d)\n' "$name" \
            "$(stat -c%s "$OUT/$name.a.qjsb")" "$(stat -c%s "$OUT/$name.b.qjsb")" \
            | tee -a "$OUT/result.txt"
        sha256sum "$OUT/$name.a.qjsb" "$OUT/$name.b.qjsb" >> "$OUT/result.txt"
        fail=$((fail + 1))
    fi
done

printf 'pass=%d fail=%d\n' "$pass" "$fail" | tee -a "$OUT/result.txt"
if [ "$fail" -gt 0 ]; then
    echo "LAYOUT TAX DETECTED: $fail fixture(s) differ across ext34 arms" >&2
    exit 1
fi
echo "layout tax: zero across $pass fixtures (byte-identical BC26)" | tee -a "$OUT/result.txt"
