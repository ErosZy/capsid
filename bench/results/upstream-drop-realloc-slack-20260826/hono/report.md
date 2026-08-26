# A/B benchmark upstream-drop-realloc-slack-20260826

- commit: 990f3a67a1d43cec0b0d3aa3ebd4a6e52be04880
- workload: fixed-1k, rounds: 7, warmup: 5s, measured: 10s
- connections: 16, inflight: 64, cpuset: 2-3, tcp_nodelay: on
- baseline env: (none)
- candidate env: (none)
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 3815.47 | 17.116 | 18.642 | 19.479 | 0.264 | 38205 | 0 | 0 |
| candidate | 1 | 3752.33 | 17.365 | 19.123 | 20.164 | 0.269 | 37580 | 0 | 0 |
| candidate | 2 | 3685.16 | 17.630 | 19.446 | 20.679 | 0.274 | 36908 | 0 | 0 |
| baseline | 2 | 3754.89 | 17.315 | 19.087 | 20.404 | 0.268 | 37614 | 0 | 0 |
| baseline | 3 | 3793.61 | 17.154 | 18.733 | 19.624 | 0.265 | 38006 | 0 | 0 |
| candidate | 3 | 3842.74 | 16.951 | 18.583 | 19.761 | 0.262 | 38489 | 0 | 0 |
| candidate | 4 | 3843.03 | 17.029 | 18.546 | 19.476 | 0.262 | 38486 | 0 | 0 |
| baseline | 4 | 3658.97 | 17.381 | 20.526 | 24.062 | 0.276 | 36643 | 0 | 0 |
| baseline | 5 | 3771.27 | 17.268 | 18.845 | 19.802 | 0.267 | 37766 | 0 | 0 |
| candidate | 5 | 3742.58 | 17.481 | 19.043 | 20.321 | 0.269 | 37477 | 0 | 0 |
| candidate | 6 | 3753.70 | 17.327 | 19.130 | 20.555 | 0.268 | 37594 | 0 | 0 |
| baseline | 6 | 3816.92 | 17.101 | 18.655 | 19.502 | 0.264 | 38234 | 0 | 0 |
| baseline | 7 | 3758.45 | 17.219 | 18.775 | 20.213 | 0.268 | 37638 | 0 | 0 |
| candidate | 7 | 3787.24 | 17.154 | 18.763 | 19.763 | 0.266 | 37929 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 7 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 3767.08
- candidate QPS: 3772.40
- delta QPS: +0.14%; delta p50: +0.32%
- M1B acceptance gate: QPS ≥ +5% or p50 ≤ -10%; verdict: FAIL

## Profiling

Profiling was explicitly disabled for this run (`--no-profile`).
