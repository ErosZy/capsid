# A/B benchmark arena-mimalloc-hono-20260826-v2

- commit: 6409604ba10e94c54c2cd2a2ef38e0473348a9aa
- workload: fixed-1k, rounds: 7, warmup: 5s, measured: 10s
- connections: 16, inflight: 64, cpuset: 2-3, tcp_nodelay: on
- baseline env: (none)
- candidate env: (none)
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 4209.58 | 15.306 | 17.116 | 20.777 | 0.239 | 42153 | 0 | 0 |
| candidate | 1 | 4155.50 | 15.573 | 17.350 | 20.354 | 0.243 | 41618 | 0 | 0 |
| candidate | 2 | 4091.92 | 15.783 | 17.713 | 20.939 | 0.247 | 40975 | 0 | 0 |
| baseline | 2 | 4143.88 | 15.558 | 17.566 | 21.409 | 0.243 | 41493 | 0 | 0 |
| baseline | 3 | 4160.14 | 15.559 | 17.491 | 20.955 | 0.243 | 41647 | 0 | 0 |
| candidate | 3 | 4107.98 | 15.755 | 17.555 | 20.374 | 0.246 | 41148 | 0 | 0 |
| candidate | 4 | 4117.59 | 15.776 | 17.699 | 20.442 | 0.245 | 41241 | 0 | 0 |
| baseline | 4 | 4138.10 | 15.545 | 17.345 | 21.783 | 0.244 | 41442 | 0 | 0 |
| baseline | 5 | 4184.95 | 15.405 | 17.375 | 21.422 | 0.241 | 41909 | 0 | 0 |
| candidate | 5 | 4151.76 | 15.599 | 17.572 | 20.698 | 0.243 | 41570 | 0 | 0 |
| candidate | 6 | 4108.67 | 15.729 | 17.527 | 20.445 | 0.246 | 41138 | 0 | 0 |
| baseline | 6 | 4116.19 | 15.665 | 17.588 | 21.505 | 0.245 | 41217 | 0 | 0 |
| baseline | 7 | 4191.54 | 15.425 | 17.321 | 20.700 | 0.241 | 41974 | 0 | 0 |
| candidate | 7 | 4148.99 | 15.639 | 17.431 | 20.546 | 0.243 | 41547 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 7 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 4163.48
- candidate QPS: 4126.06
- delta QPS: -0.90%; delta p50: +1.28%
- M1B acceptance gate: QPS ≥ +5% or p50 ≤ -10%; verdict: FAIL

## Profiling

Profiling was explicitly disabled for this run (`--no-profile`).
