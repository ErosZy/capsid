#!/usr/bin/env bash
# R0 paired A/B (tier-3 plan §9): BC27-opt (ext emission ON) vs BC26-opt
# (emission OFF) through the SAME worker binary, interleaved ABBA/BAAB,
# PAIRS paired samples per fixture, median per arm. The two opt bundles
# are produced by two capsid-bytecode-compile builds (CAPSID_AOT_EMIT_EXT
# on/off); the worker's dual reader accepts both versions, so the only
# difference between the arms is the R0 ext template at get_array_el
# sites. Bodies are cross-checked byte-for-byte (--expect-body) so the
# A/B doubles as a correctness check.
#
# Usage: bash bench/r0-paired-ab.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-bench/results/r0-paired-$(date +%Y%m%dT%H%M%S)}
mkdir -p "$OUT"
WORKER=${WORKER:-build-release/capsid-worker}
THROUGHPUT=${THROUGHPUT:-bench/bin/exec-throughput}
COMPILE26=${COMPILE26:-build-release/capsid-bytecode-compile}
COMPILE27=${COMPILE27:-build-release-emit/capsid-bytecode-compile}
SUT_CPUSET=${SUT_CPUSET:-2-3}
PAIRS=${PAIRS:-7}
FIXTURES=${FIXTURES:-"matrix-rt arrlocal-rt sieve-rt json-rt v8-suite-mod"}
for bin in "$WORKER" "$THROUGHPUT" "$COMPILE26" "$COMPILE27"; do
    [ -x "$bin" ] || { echo "missing required binary: $bin" >&2; exit 1; }
done

median() {
    sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

echo "pairs: $PAIRS sut_cpuset: $SUT_CPUSET" | tee "$OUT/manifest.txt"
{
    echo "generated_at: $(date --iso-8601=seconds)"
    echo "commit: $(git rev-parse HEAD)"
    echo "runner_sha256: $(sha256sum bench/r0-paired-ab.sh | cut -d' ' -f1)"
    echo "command: OUT=$OUT WORKER=$WORKER THROUGHPUT=$THROUGHPUT COMPILE26=$COMPILE26 COMPILE27=$COMPILE27 SUT_CPUSET=$SUT_CPUSET PAIRS=$PAIRS FIXTURES=$FIXTURES bash bench/r0-paired-ab.sh"
    echo "uname: $(uname -a)"
    sha256sum "$WORKER" "$THROUGHPUT" "$COMPILE26" "$COMPILE27"
} >> "$OUT/manifest.txt"

for name in $FIXTURES; do
    src="bench/fixtures/$name.js"
    source_name="file:///app/$name.js"
    q26="$OUT/$name.opt26.qjsb"
    q27="$OUT/$name.opt27.qjsb"

    # Static evidence: both compiler report lines + ext census.
    "$COMPILE26" \
        --source "$src" --source-name "$source_name" \
        --application "bench" --version "bench-1" --key-id "bench-key" \
        --bytecode-out "$q26" --attestation-out "$OUT/$name.att26.json" \
        --signing-message-out "$OUT/$name.sig26.bin" \
        2>"$OUT/$name.opt26.stderr"
    "$COMPILE27" \
        --source "$src" --source-name "$source_name" \
        --application "bench" --version "bench-1" --key-id "bench-key" \
        --bytecode-out "$q27" --attestation-out "$OUT/$name.att27.json" \
        --signing-message-out "$OUT/$name.sig27.bin" \
        2>"$OUT/$name.opt27.stderr"
    printf '%-16s ver26=%d ver27=%d size26=%d size27=%d\n' "$name" \
        "$(od -An -tu1 -N1 "$q26")" "$(od -An -tu1 -N1 "$q27")" \
        "$(stat -c%s "$q26")" "$(stat -c%s "$q27")" \
        | tee -a "$OUT/cells.txt"

    # Reference body from a source run. exec-throughput prints the body
    # raw inside the JSON string (multi-line bodies break line-based
    # parsing), so join all lines first, then cut between the markers.
    # NO_BODY_CHECK_FIXTURES: fixtures whose body varies run to run by
    # design (v8-suite-mod self-times its own benchmarks, so scores
    # differ every run) skip the byte-for-byte cross-check; a structural
    # marker check stands in, and timing still comes from interleaved
    # samples.
    if [[ " ${NO_BODY_CHECK_FIXTURES:-} " == *" $name "* ]]; then
        body=""
        markers="${BODY_MARKERS:-Richards: Score (version 7):}"
    else
        body=$(taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
            --mode source --input "$src" --source-name "$source_name" \
            --rounds 1 --warmup 0 \
            | awk '{all = all $0 "\n"} END {
                       a = index(all, "\"body\":\"");
                       b = index(all, "\",\"ok\"");
                       if (a > 0 && b > a + 8)
                           print substr(all, a + 8, b - (a + 8));
                   }')
        [ -n "$body" ] || { echo "$name: no body extracted from source run" >&2;
                            continue; }
    fi

    # One warmup run per arm, then PAIRS interleaved ABBA/BAAB samples.
    expect=(); [ -n "$body" ] && expect=(--expect-body "$body")
    for arm in 26 27; do
        q="$OUT/$name.opt$arm.qjsb"
        if [ -n "$body" ]; then
            taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
                --mode opt --input "$q" --source-name "$source_name" \
                --rounds 1 --warmup 0 --expect-body "$body" >/dev/null \
                || { echo "$name: opt$arm warmup failed (body mismatch?)" >&2;
                     exit 1; }
        else
            raw=$(taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
                --mode opt --input "$q" --source-name "$source_name" \
                --rounds 1 --warmup 0)
            for m in $markers; do
                case "$raw" in *"$m"*) : ;; *)
                    echo "$name: opt$arm warmup missing marker '$m'" >&2
                    exit 1;; esac
            done
        fi
    done
    for ((i = 0; i < PAIRS; i++)); do
        if ((i % 2 == 0)); then seq="26 27 27 26"; else seq="27 26 26 27"; fi
        for arm in $seq; do
            if [ "$arm" = 26 ]; then q="$q26"; else q="$q27"; fi
            taskset -c "$SUT_CPUSET" "$THROUGHPUT" --worker "$WORKER" \
                --mode opt --input "$q" --source-name "$source_name" \
                --rounds 1 --warmup 0 "${expect[@]}" \
                >>"$OUT/$name.$arm.jsonl"
        done
    done
    t26=$(grep -o '"ms":[0-9.]*' "$OUT/$name.26.jsonl" | cut -d: -f2 | median)
    t27=$(grep -o '"ms":[0-9.]*' "$OUT/$name.27.jsonl" | cut -d: -f2 | median)
    delta=$(awk -v a="$t26" -v b="$t27" \
        'BEGIN { printf "%.3f", 100.0 * (a - b) / a }')
    printf '%-16s opt26_ms=%8.3f opt27_ms=%8.3f  R0_BC27_vs_BC26=%s%%\n' \
        "$name" "$t26" "$t27" "$delta" | tee -a "$OUT/cells.txt"
    { echo "=== $name opt26 stderr ==="; cat "$OUT/$name.opt26.stderr";
      echo "=== $name opt27 stderr ==="; cat "$OUT/$name.opt27.stderr"; } \
        >> "$OUT/static.txt"
done

find "$OUT" -maxdepth 1 -type f -print0 | sort -z | \
    xargs -0 sha256sum > "$OUT/sha256sums.txt"
echo "results in $OUT"
