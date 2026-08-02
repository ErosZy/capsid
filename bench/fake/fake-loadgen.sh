#!/usr/bin/env bash
# Fake loadgen for the benchmark runner RED test. It writes one deterministic
# raw sample line and the correctness verdict without contacting the gateway.
# Behavior switches (RED scenarios):
#   CAPSID_BENCH_FAKE_FEWER_ROUNDS=1   -> emits no measured sample (runner must
#                                        reject fewer than three rounds).
#   CAPSID_BENCH_FAKE_CORRECTNESS_FAIL=1 -> correctness verdict fails (runner
#                                        must reject).
#   CAPSID_BENCH_FAKE_ERRORS_AS_SUCCESS=1 -> errors are counted as success
#                                        (runner must reject).
set -euo pipefail

side="${CAPSID_BENCH_SIDE:?}"
round="${CAPSID_BENCH_ROUND:?}"
samples_out="${CAPSID_BENCH_SAMPLES_OUT:?}"
correctness_out="${CAPSID_BENCH_CORRECTNESS_OUT:?}"

printf '{"side":"%s","round":%s,"phase":"warmup","qps":0}\n' "$side" "$round" \
    >"$samples_out"

if [ "${CAPSID_BENCH_FAKE_FEWER_ROUNDS:-0}" != "1" ]; then
    printf '{"side":"%s","round":%s,"phase":"measured","qps":12345.6,"p50_ms":0.9,"p95_ms":1.3,"p99_ms":2.1,"dispatch_wait_ms":0.02,"completed":50000,"errors":0,"timeouts":0}\n' \
        "$side" "$round" >>"$samples_out"
fi

if [ "${CAPSID_BENCH_FAKE_CORRECTNESS_FAIL:-0}" = "1" ]; then
    printf '{"ok":false,"error":"fake content mismatch","responses_checked":50000,"mismatches":1,"errors_as_success":false}\n' \
        >"$correctness_out"
elif [ "${CAPSID_BENCH_FAKE_ERRORS_AS_SUCCESS:-0}" = "1" ]; then
    printf '{"ok":true,"responses_checked":50000,"mismatches":0,"errors_as_success":true}\n' \
        >"$correctness_out"
else
    printf '{"ok":true,"responses_checked":50000,"mismatches":0,"errors_as_success":false}\n' \
        >"$correctness_out"
fi

exit 0
