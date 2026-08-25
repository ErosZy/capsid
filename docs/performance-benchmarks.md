# Performance Evidence and Current Results

This document contains only the latest maintained performance results. QuickJS
experiment decisions are maintained separately in
[QuickJS Optimization](quickjs-optimization.md).

## 1. Evidence Rules

A product performance conclusion requires all of the following:

- the same hardware, OS, build type, runtime, worker, bundle, and resource limits;
- the same load generator, connection count, inflight limit, response content,
  and validation logic;
- separate warmup and measured phases with at least three interleaved samples;
- QPS, p50/p95/p99, errors, timeouts, CPU, and memory saved together;
- separate host and worker profiles when the runtime binary changes;
- commit, dependency identity, build flags, command, environment, and artifact
  SHA-256 values;
- a positive control for exact response content and a negative control proving
  that error responses are not counted as success.

Full HTTP throughput, single-worker execution, cold start, and memory density
answer different questions and must not be combined into one percentage.
Optimized-bytecode attribution always compares optimized with raw bytecode;
source mode also includes parsing and compilation.

Raw samples, correctness verdicts, profiles, manifests, and checksum lists are
stored together under `bench/results/`. A large microbenchmark gain is a
mechanism result, not a general request-throughput claim.

## 2. Current Test Environment

All current results were collected on 2026-08-25 after rebooting WSL into an
idle environment.

| Item | Current value |
|---|---|
| CPU | AMD Ryzen 3 3300X, 4C/8T |
| OS | Ubuntu 24.04 on WSL2, kernel 6.6.87.2-microsoft-standard-WSL2 |
| Memory | 7.9 GiB visible to WSL |
| Measured build | commit `cfeae0b`, version 0.2.1, Release + LTO, clean tree |
| QuickJS defaults | opcode profile OFF, field IC OFF, ext34 OFF |
| CPU partition | SUT CPUs 0-3; load generator CPUs 4-7 |
| Service protocol | two workers, c64, 3s warmup + 8s measured, 3 rotated rounds |
| Hono bundle | Hono 4.12.32, SHA-256 `83ebc6c2…` |
| Load generator | SHA-256 `58011e51…` |

Comparison stacks:

| Stack | Version and mode |
|---|---|
| capsid + Hono | static pool, 2 workers, initial stream window 16,384 |
| PHP + Slim | PHP 8.5.9, Slim 4.15.2, nginx 1.30.4, php-fpm static 2 children |
| Flask + Gunicorn | Python 3.14.5, Flask 3.1.3, Gunicorn 26.0.0, 2 sync workers |
| FastAPI + Uvicorn | Python 3.14.5, FastAPI 0.141.1, Uvicorn 0.52.3, uvloop + httptools, 2 workers |
| Cold-start references | Node 24.18.0 and Deno 2.9.3 |

## 3. Current Four-Stack Matrix

The current run completed 12 workloads × 4 stacks × 3 rounds: **144/144
correctness checks passed**, with zero errors or timeouts. The negative control
returned 1,402 HTTP 404 responses; all 1,402 were rejected and successful QPS
was zero.

Median QPS over three rotated rounds:

| workload | capsid + Hono | PHP + Slim | Flask + Gunicorn | FastAPI + Uvicorn |
|---|---:|---:|---:|---:|
| JSON 1k | **7042** | 1872 | 5068 | 6260 |
| JSON 4k | **5601** | 1746 | 4663 | 5504 |
| JSON 16k | 5070 | 1731 | 4730 | **5520** |
| JSON 32k | 4077 | 1573 | 4011 | **4617** |
| bytes 1k | 5168 | 1839 | 4985 | **5954** |
| bytes 4k | 4714 | 1705 | 4704 | **5665** |
| bytes 16k | 4214 | 1670 | 4596 | **5401** |
| bytes 32k | 3273 | 1579 | 4071 | **4667** |
| stream 1k | **4753** | 1749 | 4608 | 2160 |
| stream 4k | 4728 | 1748 | **4861** | 3042 |
| stream 16k | **3830** | 1676 | 3743 | 2131 |
| stream 32k | 3218 | 1574 | **4180** | 2081 |

Capsid latency from the same samples:

| workload | p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|
| JSON 1k | 8.82 | 11.38 | 13.30 |
| JSON 4k | 11.01 | 14.02 | 16.20 |
| JSON 16k | 12.71 | 16.31 | 17.90 |
| JSON 32k | 15.53 | 19.56 | 22.00 |
| bytes 1k | 12.43 | 14.99 | 17.17 |
| bytes 4k | 13.38 | 16.32 | 18.23 |
| bytes 16k | 15.03 | 18.40 | 20.27 |
| bytes 32k | 19.45 | 22.87 | 25.87 |
| stream 1k | 13.38 | 16.84 | 18.82 |
| stream 4k | 13.40 | 16.28 | 18.12 |
| stream 16k | 18.05 | 22.56 | 24.87 |
| stream 32k | 20.57 | 26.65 | 28.92 |

Current per-process resources:

| role | PSS MB | RSS MB | maximum observed CPU |
|---|---:|---:|---:|
| capsid host | 6.2 | 7.7 | 104% |
| capsid worker, each of 2 | 6.4 | 9.7 | 95% |
| Gunicorn worker, each of 2 | 23.7 | 32.7 | 94% |
| Uvicorn worker, each of 2 | 42.1 | 51.8 | 110% |
| nginx | unavailable | 6.2 | 25% |
| php-fpm child, each of 2 | unavailable | 14.6 | 95% |

The full Capsid serving path is about 19.0 MB PSS: 6.2 MB for the host plus
6.4 MB per worker. No growing IC sidecar state is present because the IC is
compiled OFF.

### Current profile

A 30-second JSON 16k profile validated 163,203 responses with no mismatch,
error, timeout, or lost perf sample.

| process | leading symbols |
|---|---|
| host, 831 samples | `memcpy` 7.22%, allocator 6.02%, `get_meta` 3.01% |
| workers, 3,694 samples | `JS_CallInternal` 20.36%, `malloc_usable_size` 10.42%, malloc 6.74%, regexp backtracking 5.44%, `JS_GetPropertyInternal` 3.68% |

The source-Hono path remains dominated by JS calls, allocation, and framework
regexp work. This matrix loads source and therefore does not measure BC26 AOT
attribution.

## 4. Current Cold Start

Each cell drops one warmup and reports the median of five runs from process
creation through READY and the first validated response.

| source size | capsid source | capsid trusted bytecode | Node source | Deno source |
|---:|---:|---:|---:|---:|
| 10k | **8.43 ms** | **7.45 ms** | 110 ms | 39 ms |
| 100k | 18.35 ms | **9.58 ms** | 109 ms | 39 ms |
| 1M | 133.63 ms | **36.23 ms** | 137 ms | 52 ms |

At 1M, trusted bytecode is 3.69× faster than Capsid source and 1.44× faster
than Deno source. Capsid's request uses its in-process worker protocol; Node and
Deno use local HTTP, so this is aligned lifecycle evidence rather than an
isomorphic request-path comparison.

## 5. Current Bytecode Optimizer Results

The implementation and soundness contract are in
[Bytecode AOT Optimizer](bytecode-aot-optimizer.md). The current clean run
compares optimized BC26 directly with raw BC26, using one discarded warmup and
five measured executions per mode.

| fixture | raw median | optimized median | gain |
|---|---:|---:|---:|
| arith-rt | 172.659 ms | 13.659 ms | **+92.09%** |
| cascade-rt | 68.192 ms | 37.246 ms | **+45.38%** |
| matrix-rt | 7.639 ms | 7.396 ms | +3.18% |
| sieve-rt | 49.625 ms | 48.650 ms | +1.96% |
| string-rt | 2.016 ms | 2.028 ms | -0.60% |
| fib-rt | 50.246 ms | 50.192 ms | +0.11% |
| json-rt | 4.118 ms | 4.028 ms | +2.19% |
| prop-loop-rt | 94.985 ms | 79.122 ms | **+16.70%** |
| prop-hoist-rt | 13.012 ms | 9.965 ms | **+23.42%** |
| copy-chain-rt | 14.857 ms | 11.952 ms | **+19.55%** |
| branch-const-rt | 13.491 ms | 13.429 ms | +0.46% |
| cse-loop-rt | 19.149 ms | 18.571 ms | +3.02% |
| licm-rt | 8.157 ms | 8.304 ms | -1.80% |

Eleven of thirteen centers are positive. The equal-fixture geometric-mean
speedup is +25.89%, but the aggregate is dominated by synthetic arith/cascade
fixtures and is not an HTTP throughput claim.

The final portfolio gate enables all retained work—BC26 `kPassAll` plus the
mixed-number `mul` fast path—against the patchless/add-loc control. Eight
Kraken/Octane programs use seven balanced pairs each:

| program | gain, paired 95% CI |
|---|---:|
| Kraken Beat Detection | +1.23% [-2.98%, +5.62%] |
| Kraken DFT | -0.63% [-8.16%, +7.53%] |
| Kraken FFT | **+2.61% [+0.40%, +4.87%]** |
| Kraken Oscillator | **+4.17% [+2.03%, +6.36%]** |
| Kraken Darkroom | **+6.78% [+5.63%, +7.93%]** |
| Octane Box2D | **+1.33% [+0.59%, +2.08%]** |
| Octane Navier-Stokes | **+5.89% [+5.37%, +6.42%]** |
| Octane Richards | +2.10% [-6.06%, +10.96%] |
| **equal-weight geometric mean** | **+2.91% [+0.84%, +5.02%]** |

Seven of eight centers are positive and no program regresses significantly.
This is the production keep result. IC, BC27, ext34, and store/reload fusion
remain disabled or removed as recorded in
[QuickJS Optimization](quickjs-optimization.md).

## 6. Evidence Identities

| current evidence | directory | checksum-list SHA-256 |
|---|---|---|
| four-stack matrix | `bench/results/four-qps-current-clean-20260825T161800/` | `67ffb510…` |
| focused stability repeat | `bench/results/four-qps-current-clean-focused-20260825T164700/` | `94272dee…` |
| host/worker profile | `bench/results/four-qps-current-clean-profile-20260825T164600/` | `c134cda4…` |
| raw/optimized execution | `bench/results/exec-throughput-current-clean-20260825T164400/` | `4b0c21b8…` |
| cold start | `bench/results/cold-start-current-clean-20260825T164500/` | `1de6b30c…` |
| retained portfolio | `bench/results/all-effective-cumulative-20260825/` | `81764e13…` |

The retained-portfolio summary SHA-256 is `35d1840c…`. Every listed checksum
file was verified after collection.
