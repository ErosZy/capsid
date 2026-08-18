# Performance: Evidence Rules and Current State

This document is the single maintained document for performance topics; it keeps the evidence rules and the current (2026-08-18) observed samples. Historical optimization process and earlier checkpoints (M1P, E1-E14, Host optimization loop, and the 2026-08-14 4C runs) live in git history and the raw artifacts in `bench/results/`, and are not maintained here.

## 1. Evidence Rules

### Conclusion Threshold

To write a performance conclusion into product documentation, all of the following must be present at once:

- same hardware, OS, build type, Runtime, worker, bundle, and resource limits;
- same load generator, connection count, inflight, response content, and validation logic;
- warm-up and measured phases separated, with at least three interleaved A/B raw samples;
- QPS, p50/p95/p99, errors, timeouts, cancellation, drain, CPU, and memory saved together;
- profiles collected separately for gateway and worker to explain which layer time is spent in;
- record commit, dependency identity, build flags, commands, environment, and result file SHA-256;
- positive control proves returned content is correct; negative control proves error responses are not counted as success.

When raw A/B samples or either side's profile are missing, you may report "observed samples", but must not write "optimization works", "improved N%", or adjust default capacity based on it.

### Measurements That Must Not Be Mixed

The following data answer different questions, and reports must keep them separate: full HTTP stack total cost; Host A/B; single-worker execution and memory; cold start (process creation, handshake, validation, loading, READY, and first response); density/stability. Full-container RSS/PSS cannot be compared directly with single-worker PSS; source and trusted-bytecode cold start also cannot substitute for warm request throughput testing.

### Result Storage Format

Each run must save at least the manifest (commit, identity, environment, commands, and file digests), raw samples (not just aggregates), correctness results, both-side profiles, and a report generated only from those files. Automated audit should reject orphaned reports, "improvement" conclusions missing profiles, results not bound to a commit, and changes that submit only summary numbers without raw samples.

### Current Optimization Principles

- find hotspots with profiles first, then write the optimization and matching RED benchmark;
- do not introduce io_uring, shared-memory IPC, a custom HTTP parser, or complex scheduling for speculative gains;
- correctness, isolation, and fail-closed contracts cannot be bypassed for QPS;
- default worker/inflight/queue numbers can only be frozen after a representative workload scan;
- every performance change runs correctness, sanitizer, fault-injection, and same-condition regression.

The early M1 baseline is only used to freeze the magnitude of the minimal common data plane: the first round does not wait for request body, streaming, cancel, or timeout to be implemented, and cannot be extrapolated into conclusions about the full data plane. Once those contracts land, they must be benchmarked on the same runner, and the new checkpoint must be recorded while keeping rather than overwriting the first-round samples.

## 2. Test Environment (2026-08-18)

The current runs share the following environment:

| Item | Value |
|---|---|
| CPU | Intel Core i5-12400F (6C/12T) |
| OS | Ubuntu 24.04 on WSL2, kernel 6.18.33.2-microsoft-standard-WSL2 |
| Memory | 11 GiB visible to WSL |
| Process protocol | SUT taskset 0-5 / loadgen 6-11; two-process model |
| Load protocol | conns=64, 18 workloads × 2 rounds (warmup 3s + measured 8s), correctness checked each round |

The stack under test (versions recorded in each manifest):

| Stack | Component and version |
|---|---|
| capsid + hono | capsid commit `b39acee` + Hono 4.12.32 (self-contained bundle); static-pool 2 workers, `initial-stream-window 65536` |
| PHP 8 + Slim | PHP 8.3.6 + Slim 4.15.2 + nginx 1.24.0 + php-fpm (pm.max_children=2); app/vendor on native filesystem, OPcache enabled, `fastcgi_buffering off` |
| Python 3 + Flask | Python 3.12.3 + Flask 3.1.3 + Gunicorn 26.0.0 (2 sync workers) |
| Cold-start extras | Node v24.3.0, Deno 2.9.3 |

## 3. Three-Stack Full Matrix (2026-08-18, c64, 2 rounds)

6 sizes × 3 kinds = 18 workloads, 2 rounds per cell, byte-aligned payloads, correctness checked every round. This is an **observed samples** run — profiles were not collected — so it does not meet the conclusion threshold above and no default capacities are frozen from it. All three stacks were pinned to CPUs 0-5, loadgen to 6-11. Raw samples and manifests: `build-win/bench-obs/results-20260818T102517/` and the 8k supplement `build-win/bench-obs/results-20260818T104504/` (local, git-ignored).

| workload | capsid + hono | PHP 8 + Slim | Python 3 + Flask |
|---|---:|---:|---:|
| json 1k | **13838** | 4037 | 7030 |
| json 4k | **13471** | 4015 | 7172 |
| json 8k | **10117** | 3986 | 6012 |
| json 16k | **9615** | 4118 | 5536 |
| json 32k | **7210** | 3789 | 5015 |
| json 64k | **5069** | 3278 | 4073 |
| bytes 1k | **11895** | 4361 | 7546 |
| bytes 4k | **11064** | 4211 | 7404 |
| bytes 8k | **9977** | 3979 | 6442 |
| bytes 16k | **9602** | 4154 | 6294 |
| bytes 32k | **7841** | 3894 | 5354 |
| bytes 64k | **5942** | 3540 | 4677 |
| stream 1k | **11559** | 4349 | 7558 |
| stream 4k | **11224** | 4184 | 7315 |
| stream 8k | **9526** | 4076 | 6625 |
| stream 16k | **7996** | 4133 | 6402 |
| stream 32k | **6123** | 3937 | 5337 |
| stream 64k | **4121** | 3356 | 4274 |

p95 latency (ms) for the same cells:

| workload | capsid | PHP 8 | Python 3 |
|---|---:|---:|---:|
| json 1k | 6.9 | 17.5 | 10.2 |
| json 8k | 9.9 | 17.8 | 12.1 |
| json 64k | 24.9 | 20.7 | 16.8 |
| bytes 8k | 10.1 | 18.1 | 11.3 |
| bytes 64k | 20.1 | 19.1 | 14.4 |
| stream 8k | 10.0 | 17.7 | 11.1 |
| stream 64k | 21.2 | 20.7 | 16.3 |

Resource medians over the whole run (PSS/RSS via `smaps_rollup`/`statm`; CPU is the median of non-zero 5s process-tree ticks and is an observed slice, not a stable CPU share):

| stack | PSS (MB) | RSS (MB) | CPU non-zero tick median | CPU max observed |
|---|---:|---:|---:|---:|
| capsid + hono | 34.1 | 47.0 | 8.0% | 308% |
| PHP 8 + Slim | 23.7 | 55.2 | 190.4% | 257% |
| Python 3 + Flask | 54.4 | 86.6 | 137.2% | 203% |

A prior attempt on the same day had PHP at 17 QPS / p95 3.6 s because the Slim tree lived on the `/mnt/e` 9p filesystem and paid per-request `stat`/autoload cost; moving the PHP app to the native filesystem and enabling OPcache restored parity (~4k QPS, p95 ~17 ms). That failure mode is recorded here because deployment location is part of the stack's operating conditions.

## 4. Cold-Start Comparison (2026-08-18, median ms, 5 rounds)

Measurement class 4 (process creation, handshake, validation, loading, READY, and first response). Fixture is real-shaped JS source (three template rotations: loop + object-literal function, class, arrow/map/filter/sort chain), at 10k/100k/1M sizes (36/355/3547 top-level units); each side loads the same function body byte-aligned, differing only in entry point. capsid uses C ABI spawn→load (source/trusted bytecode)→READY→first response (bodyless IPC request); Node/Deno use process start→stdout READY→curl first request. Each cell drops 1 warmup round and takes the median of 5 rounds. capsid side built from commit `b39acee`; Node v24.3.0, Deno 2.9.3; SUT taskset 0-5. Raw samples: `build-win/bench-obs/cold-start-20260818T105257/` (local, git-ignored).

| Size | capsid source | capsid trusted bytecode | Node 24 source | Deno 2.9 source |
|---:|---:|---:|---:|---:|
| 10k | **5.1** | **4.9** | 47 | 32 |
| 100k | **9.2** | **5.6** | 45 | 33 |
| 1M | 58.2 | **15.1** | 71 | 33 |

READY times (same samples): capsid source 4.9/8.9/57.9, bytecode 4.6/5.4/14.9, Node 39/38/63, Deno 26/27/27.

- **Startup baseline dominates small sizes**: capsid 10k source 5.1ms is about 1/9 of Node and 1/6 of Deno, and it remains fastest at 100k (9.2 vs 45/33).
- **Trusted bytecode pays off as compile cost grows**: at 1M, bytecode 15.1ms is ~3.9× faster than capsid source (58.2ms) and is the fastest cell overall.
- **At 1M source capsid sits between Node and Deno** (58.2 vs 71 vs 33): QuickJS full compile vs V8 parse/lazy compile; Deno keeps the V8 parser advantage on AST-dense source.
- Semantic note: capsid first response goes through in-process IPC, while Node/Deno use local HTTP curl; "first request completes after ready" is aligned, but the request path implementation differs, so this is not an isomorphic comparison.

## 5. Retired Checkpoints

The 2026-08-14 4C conclusion-level tables (three-stack matrix, cold-start comparison, and resource profile) were retired from this page on 2026-08-18 and are not maintained here. They remain available in git history and in the raw artifacts under `bench/results/` referenced by the older revisions of this document.
