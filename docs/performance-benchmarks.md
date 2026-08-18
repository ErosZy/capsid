# Performance: Evidence Rules and Current State

This document is the single maintained document for performance topics; it keeps the evidence rules and the current (2026-08-18) conclusion-level samples. Historical optimization process and earlier checkpoints (M1P, E1-E14, Host optimization loop, the 2026-08-14 4C runs, and the 2026-08-18 Intel 6C runs) live in git history and the raw artifacts in `bench/results/`, and are not maintained here.

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
| CPU | AMD Ryzen 3 3300X (4C/8T) |
| OS | Ubuntu 24.04 on WSL2, kernel 6.6.87.2-microsoft-standard-WSL2 |
| Memory | 7.9 GiB visible to WSL |
| Process protocol | SUT taskset 0-3 / loadgen 4-7; two-process model |
| Load protocol | conns=64, 12 workloads × 3 rounds (warmup 3s + measured 8s), correctness checked each round; workload order rotates the starting stack to cancel drift |
| capsid commit | `c943e35` |

The stacks under test (versions recorded in each manifest; `bench/results/four-qps-final-20260818T131300/manifest.txt`, `cold-start-20260818T134435/manifest.txt`):

| Stack | Component and version |
|---|---|
| capsid + hono | capsid `c943e35` (`build-m1d` host `1e4add5…`, worker `fe7f848…`) + Hono 4.12.32 (self-contained bundle); static-pool 2 workers, `initial-stream-window 16384` |
| PHP + Slim | PHP 8.5.9 + Slim 4.15.2 + nginx 1.30.4 + php-fpm `pm=static max_children=2` (docker container `capsid-php-bench`, port 8080) |
| Python + Flask | Python 3.14.5 + Flask 3.1.3 + Gunicorn 26.0.0 (2 sync workers) |
| Python + FastAPI | Python 3.14.5 + FastAPI 0.141.1 + Uvicorn 0.52.3 (2 workers, uvloop + httptools) |
| Cold-start extras | Node v24.18.0, Deno 2.9.3 |

Workload matrix: json / bytes / stream at 1k, 4k, 16k, 32k (4k cells use the loadgen `matrix-<kind>-<label>` names; the legacy 1k/16k/32k names are `json`/`json16k`/`json32k` etc.). Payloads are byte-aligned and the loadgen verifies exact content every round: `matrix-bytes` is exact 0x62 bytes, `matrix-stream` 's' bytes, legacy bytes 0x61, legacy stream b/c/d thirds, legacy JSON compact or spaced marker.

## 3. Four-Stack Matrix (2026-08-18, c64, 3 rounds)

12 workloads × 4 stacks × 3 rounds, warmup 3s + measured 8s, correctness checked every round — **all 144 rounds OK**. This run meets the conclusion threshold: raw samples, correctness verdicts, per-process resource samples, and capsid host/worker perf profiles are all saved. Raw data: `bench/results/four-qps-final-20260818T131300/` (merged: `four-qps-20260818T124403` capsid/php/flask + `four-qps-20260818T130505` 4k matrix + `four-qps-20260818T131156` fastapi full rerun), profiles: `bench/results/four-qps-profile-20260818T134336/`.

Median QPS over 3 rounds (winner per row in bold):

| workload | capsid + hono | PHP 8 + Slim | Flask + Gunicorn | FastAPI + Uvicorn |
|---|---:|---:|---:|---:|
| json 1k | **6881** | 1788 | 5007 | 6214 |
| matrix-json-4k | **5563** | 1691 | 4620 | 5538 |
| json 16k | 5054 | 1637 | 4626 | **5557** |
| json 32k | 4383 | 1577 | 4020 | **4825** |
| bytes 1k | 5209 | 1784 | 5050 | **6047** |
| matrix-bytes-4k | 4694 | 1700 | 4827 | **5870** |
| bytes 16k | 4431 | 1648 | 4628 | **5581** |
| bytes 32k | 3459 | 1585 | 4127 | **4868** |
| stream 1k | **4709** | 1730 | 4691 | 2261 |
| matrix-stream-4k | 4792 | 1697 | **4824** | 3077 |
| stream 16k | **3932** | 1631 | 3713 | 2169 |
| stream 32k | 3294 | 1531 | **4130** | 2107 |

p50/p95/p99 latency (ms, median over rounds, from the same samples):

| workload | capsid | PHP | Flask | FastAPI |
|---|---:|---:|---:|---:|
| json 1k | 9.1 / 12.0 / 13.4 | 35.7 / 37.2 / 38.3 | 12.7 / 13.5 / 14.3 | 10.1 / 12.5 / 14.4 |
| json 16k | 12.5 / 15.5 / 18.5 | 39.0 / 40.5 / 41.2 | 13.7 / 14.7 / 17.0 | 11.3 / 13.6 / 15.3 |
| json 32k | 14.5 / 17.2 / 19.3 | 40.5 / 42.3 / 43.9 | 15.6 / 16.7 / 21.4 | 13.1 / 15.8 / 17.6 |
| bytes 1k | 12.0 / 14.8 / 16.8 | 35.8 / 37.4 / 38.6 | 12.7 / 13.4 / 14.0 | 10.4 / 12.7 / 14.5 |
| bytes 32k | 18.7 / 22.0 / 23.7 | 40.3 / 41.8 / 42.5 | 15.4 / 16.3 / 17.2 | 13.0 / 16.0 / 18.0 |
| stream 1k | 13.8 / 16.9 / 19.9 | 36.9 / 38.4 / 39.1 | 13.6 / 14.4 / 14.8 | 28.0 / 33.1 / 36.9 |
| stream 16k | 16.4 / 19.8 / 21.8 | 39.2 / 40.9 / 41.9 | 17.2 / 18.0 / 19.1 | 29.2 / 34.3 / 38.4 |
| stream 32k | 19.3 / 22.6 / 25.4 | 41.5 / 43.3 / 44.3 | 15.4 / 16.3 / 17.9 | 30.0 / 35.2 / 38.9 |

Per-process resources (median over the 5s sampling window; PSS from `smaps_rollup`, RSS from `statm`; PSS is `n/a` where the sampler (non-root) cannot read another user's `smaps_rollup` — nginx/php-fpm run as root in the container. CPU is the max window-average % of one core observed for any process of the role):

| role | PSS (MB) | RSS (MB) | CPU max observed |
|---|---:|---:|---:|
| capsid host (static-pool) | 5.7 | 7.4 | 103% |
| capsid worker (each of 2) | 6.0 | 9.3 | 100% |
| gunicorn worker (each of 2) | 22.2 | 32.4 | 94% |
| nginx (container, root) | n/a | 6.3 | 20% |
| php-fpm child (each of 2, root) | n/a | 14.8 | 95% |
| php-fpm master (root) | n/a | 22.2 | — |

capsid's full serving path (host + 2 workers) stays at ≈5.7 MB PSS for the host plus ≈6.0 MB per worker; a gunicorn sync worker alone uses 22.2 MB PSS / 32.4 MB RSS.

### Profiles (capsid host and worker, json16k, 30s, `perf record -F 99`)

`bench/results/four-qps-profile-20260818T134336/` — perf data + full `perf report` text + correctness (158,348 responses checked, 0 mismatches during the profile session). Only capsid is profiled: it is the stack under test; the PHP/Flask/FastAPI stacks serve as comparison references and are not profiled.

capsid **host** (790 samples): 6.1% `memcpy`, 5.1% `__libc_malloc_impl`, 3.5% `alloc_slot` (all in `ld-musl`) — the host's share is dominated by request/response buffer copying and allocation on the scheduling path.

capsid **worker** (3,807 samples): 21.8% `JS_CallInternal` (QuickJS entry into the Hono handler), 9.0% `malloc_usable_size`, 6.1% `lre_exec_backtrack` (regexp execution) — the worker's time is dominated by JS handler execution itself, i.e. the application code the framework runs, not by the framework's dispatch.

### Reading the matrix

- **Small JSON: capsid wins** (json 1k 6,881 vs fastapi 6,214 vs flask 5,007 vs php 1,788; 4k json also capsid). PHP is the consistent laggard: ~1.6-1.8k QPS, p95 ≈37-44 ms — 3.5-4× the p95 of the other three.
- **Static medium payloads: FastAPI (uvloop + httptools + precomputed bodies) edges ahead** on json16k/32k and all bytes cells (+10-20% over capsid, which still beats Flask on every json/bytes cell).
- **Streaming: async Python collapses.** FastAPI's `StreamingResponse` costs ~2× over its own static responses (stream 1k 2,261 vs bytes 1k 6,047; p95 33 vs 13 ms) and trails capsid by 2× on stream 1k/16k. Flask's plain generator + WSGI holds up better (stream 32k 4,130, best cell). capsid streams at ~90% of its static bytes throughput.
- **Payload size is the shared scaling factor**: every stack loses ~35-45% from 1k to 32k (capsid json 6,881→4,383; fastapi 6,214→4,825; php 1,788→1,577).
- **Density**: capsid's per-process memory is 3-5× smaller than gunicorn workers and php-fpm children (5.7-6.0 MB PSS vs 22.2 MB / 14.8 MB RSS), which is what makes multi-worker deployment cheap for the host.

## 4. Cold-Start Comparison (2026-08-18, median ms, 5 rounds)

Measurement class 4 (process creation, handshake, validation, loading, READY, and first response). Fixture is real-shaped JS source (three template rotations: loop + object-literal function, class, arrow/map/filter/sort chain), at 10k/100k/1M sizes; each side loads the same function body byte-aligned, differing only in entry point. capsid uses C ABI spawn→load (source/trusted bytecode)→READY→first response (bodyless IPC request); Node/Deno use process start→stdout READY→curl first request. Each cell drops 1 warmup round and takes the median of 5 rounds. capsid side from commit `c943e35` (`bench/results/cold-start-20260818T134435/`).

| Size | capsid source | capsid trusted bytecode | Node 24.18 source | Deno 2.9.3 source |
|---:|---:|---:|---:|---:|
| 10k | **9.1** | **8.2** | 110 | 38 |
| 100k | **20.1** | **10.4** | 110 | 40 |
| 1M | 142.0 | **39.2** | 152 | 53 |

READY times (same samples): capsid source 8.8/19.6/141.6, bytecode 7.9/9.9/38.8, Node 96/97/140, Deno 31/31/45.

- **Startup baseline dominates small sizes**: capsid 10k source 9.1ms is about 1/12 of Node (110ms) and 1/4 of Deno (38ms), and it remains fastest at 100k (20.1 vs 110/40).
- **Trusted bytecode pays off as compile cost grows**: at 1M, bytecode 39.2ms is 3.6× faster than capsid source (142ms), 3.9× faster than Node (152ms), and still 26% faster than Deno (53ms). Node's ~110ms floor is process+V8 bootstrap; Deno's ~38ms floor is smaller but capsid bytecode beats even that at 100k (10.4ms).
- **At 1M source capsid sits between Node and Deno** (142 vs 152 vs 53): QuickJS full compile vs V8 parse/lazy compile; Deno keeps the V8 parser advantage on AST-dense source. Bytecode erases that gap entirely.
- Semantic note: capsid first response goes through in-process IPC, while Node/Deno use local HTTP curl; "first request completes after ready" is aligned, but the request path implementation differs, so this is not an isomorphic comparison.

## 5. Retired Checkpoints

The 2026-08-18 Intel i5-12400F 6C/12T conclusion-adjacent tables (three-stack 18-workload matrix, cold-start, and resource profile, commit `b39acee`/`build-win`) and the 2026-08-14 4C tables were retired from this page on 2026-08-18 and are not maintained here. They remain available in git history and in the raw artifacts under `bench/results/` referenced by the older revisions of this document.
