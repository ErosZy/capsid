# Performance: Evidence Rules and Current State

This document is the single maintained document for performance topics; it keeps the evidence rules and the current (2026-10-10) conclusion-level samples, which the `v0.2.2` release adopts as its published performance point (commit `aef6020`, clean `v0.2.2` release build). Historical optimization process and earlier checkpoints (M1P, E1-E14, Host optimization loop, the 2026-08-14 4C runs, the 2026-08-18 runs, and the superseded 2026-08-20 `v0.2.0-rc.07` checkpoint) live in git history and the raw artifacts in `bench/results/`, and are not maintained here.

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

## 2. Test Environment (2026-10-10)

The current runs share the following environment:

| Item | Value |
|---|---|
| CPU | Intel i5-12400F (6C/12T) |
| OS | WSL2, kernel 6.18.40.1-microsoft-standard-WSL2 |
| Memory | 11.6 GiB visible to WSL |
| Process protocol | SUT taskset 0-3; **one serving process per stack** (capsid single JS worker, workerd single process, uvicorn/gunicorn/php-fpm × 1) |
| Load protocol | conns=64, inflight=64, 3 workloads × 5 rounds (warmup 5 s + measured 15 s), exact-body correctness checked every round (verdict-gated: a wrong body fails the lane) |
| capsid release / commit | `v0.2.2` / `aef6020` (Release build, host `2136f37c…`, worker `a627bbc1…`) |

The stacks under test (versions and artifact SHA-256 values are recorded in `bench/results/five-qps-v022-20261010/manifest.txt` and `bench/results/cold-start-v022-20261010/manifest.txt`):

| Stack | Component and version |
|---|---|
| capsid | `v0.2.2` (`aef6020`; Release; host `2136f37c…`, worker `a627bbc1…`), `--mode single-worker`, precomputed-response JS fixture |
| Cloudflare workerd | workerd 2026-10-10 (single process, minimal capnp config, same fixture logic) |
| Python + FastAPI | Python 3.13 + FastAPI 0.143.0 + Uvicorn 0.54.0 (1 worker, uvloop + httptools, `--no-access-log`; framework-optimal fixture: precomputed matrix bodies, async endpoints, async stream generators) |
| Python + Flask | Python 3.13 + Flask 3.1.3 + Gunicorn 26.2.0 (1 sync worker; sync measured faster than gthread — GIL contention outweighs the keep-alive it adds) |
| PHP + Slim | PHP 8.3.6 + Slim 4 + nginx 1.24.0 + php-fpm `pm=static max_children=1`, opcache `validate_timestamps=0` + JIT tracing, nginx `fastcgi_keep_conn on` |
| Cold-start extras | Node v24.14.0, Deno 2.9.7 |

Workload set: `matrix-json-1k`, `matrix-json-16k`, `stream` (1 KiB b/c/d thirds). Payloads are byte-aligned and the loadgen verifies exact content every round. The 12-workload matrix of the rc.07 checkpoint is retired with it; these three lanes are the maintained history lanes and back the published page.

## 3. Five-Stack Serving Matrix (2026-10-10, c64, 5 rounds)

One quiet session, all five stacks back-to-back, warmup 5 s + 5 measured rounds of 15 s per workload, verdict-gated correctness every round. Raw samples, correctness verdicts, and manifests: `bench/results/five-qps-v022-20261010/`. The interactive history page built from this point: <https://eroszy.github.io/capsid/>.

Median QPS over 5 rounds (winner per row in bold):

| workload | capsid | workerd | FastAPI | Flask | PHP Slim |
|---|---:|---:|---:|---:|---:|
| json 1k | 9,264 | **26,624** | 12,948 | 3,568 | 2,095 |
| json 16k | 4,341 | **20,189** | 12,599 | 2,878 | 2,008 |
| stream 1k | 7,020 | **14,192** | 6,671 | 3,987 | 2,032 |

p50/p95 latency (ms, median over rounds, from the same samples):

| workload | capsid | workerd | FastAPI | Flask | PHP Slim |
|---|---:|---:|---:|---:|---:|
| json 1k | 6.9 / 10.5 | 1.7 / 3.9 | 5.0 / 7.6 | 18.6 / 20.6 | 30.3 / 36.3 |
| json 16k | 14.7 / 16.8 | 2.4 / 5.5 | 4.9 / 9.1 | 22.2 / 24.1 | 32.7 / 38.2 |
| stream 1k | 8.9 / 9.9 | 3.6 / 9.3 | 9.3 / 12.7 | 16.1 / 17.1 | 31.5 / 32.5 |

Serving-path memory (process-tree PSS after the last round; trickle = fresh process after 15 s of 10 req/s):

| stack | serving PSS (MB) | breakdown (MB) | trickle PSS (MB) |
|---|---:|---|---:|
| capsid | 12.2 | host 5.3 + worker 6.9 | 9.5 |
| Flask | 17.0 | gunicorn master 0.4 + worker 16.5 | 16.3 |
| PHP Slim | 28.1 | nginx 0.7 + nginx-worker 3.6 + fpm-master 14.0 + fpm-worker 9.8 | 27.5 |
| FastAPI | 46.1 | uvicorn master 0.4 + worker 45.7 | 42.2 |
| workerd | 581.0 | single process | 44.2 |

### Reading the matrix

- **Raw throughput: workerd leads every cell** (2.0-4.7× capsid) — but it is an untuned-V8 upper bound, not a tuned production limit.
- **workerd memory is the outlier**: ≈581 MB PSS under the 64-connection load versus a ≈44 MB trickle — V8 heap + JIT growth that is never returned. It is 12× past the second-largest stack and dominates the memory axis.
- **FastAPI is the Python leader once framework-optimal**: precomputed bodies, async endpoints, and async stream generators (plus `--no-access-log`, matching gunicorn's default) put it ahead of Flask on JSON (12.9k / 12.6k vs 3.6k / 2.9k); its streaming path still trails its own static-bytes throughput. Earlier checkpoints that showed FastAPI at ~3.2k were fixture artifacts (per-request `json.dumps`, sync-def threadpool hops, and an access log redirected onto a drvfs mount), not framework behavior.
- **capsid holds the density point**: ≈12.2 MB PSS for the whole serving path (host 5.3 + worker 6.9) with mid-pack QPS — about 1/4 of FastAPI's memory and 1/48 of workerd's under the same load.
- **PHP Slim is the consistent laggard** at ~2.0-2.1k QPS (p95 ≈32-38 ms) even with JIT tracing and FastCGI keep-alive enabled.

### Measured density: fill a pool to 2 GB (2026-10-10)

Real capacity runs, not footprint arithmetic: grow capsid's `static-pool` (or workerd's cell count in one process) until the process-tree PSS reaches 2048 MB, binary-searching the boundary (the recorded count must verify under budget, with a jitter guard). Each cell is reported twice: **initialized** (one real request per worker/cell) and **warmed** (100 requests through each, settling before a stability-checked sample). Source bundles; probes and manifests in `bench/results/density-v022-20261010/`.

| fixture size | capsid workers · init → warmed | workerd cells · init → warmed |
|---:|---:|---:|
| 10 KiB | **904 → 896** (−0.9%) | 1,061 → 638 (**−40%**) |
| 100 KiB | **576 → 575** (−0.2%) | 917 → 580 (**−37%**) |
| 1 MiB | **133 → 132** (−0.8%) | 244 → 222 (−9%) |

Reading the cold/warm gap: initialization alone understates a JIT runtime. workerd's V8 compiles hot paths and grows its heap as soon as cells serve real traffic, and neither is returned — 40% of its 10k/100k capacity disappears between the first request and the hundredth (and the throughput lane shows a single isolate growing 44 → 581 MB under sustained 64-connection load, so these warmed counts are still an optimistic middle for workerd). capsid's interpreter has no tier-up: cold and warmed read the same, which is why its numbers move less than 1%. workerd still packs more 1 MiB cells per 2 GB (compile-structure economics: V8 compiles lazily while QuickJS compiles every function eagerly); every capsid tenant remains a separate, sandboxable process, so one tenant's growth cannot shrink a neighbour's budget. Sustained per-tenant traffic density remains future work.

## 4. Cold-Start Comparison (2026-10-10, median ms, 5 rounds)

Measurement class 4: **process start → first validated response**. The shell-measured stacks (Node/Deno/workerd) previously read ~400 ms regardless of fixture size because `total_ms` was captured after a 50-request curl warm loop and the PSS walk; the endpoint now sits at the first exact-body-verified response, matching capsid's in-process runner. Fixture is real-shaped JS source (three template rotations), at 10k/100k/1M sizes; each side loads the same function body byte-aligned, differing only in entry point. capsid uses C ABI spawn→load (source/trusted bytecode)→READY→first response; Node/Deno use process start→stdout READY→curl first request; workerd uses first TCP connect→curl first request (its isolate initializes lazily on the first request, so ready→first-response carries that cost). Each cell drops 1 warmup round and takes the median of 5 rounds. Raw data: `bench/results/cold-start-v022-20261010/` (cells + manifests; the Deno pass ran separately and is stored alongside as `cells.deno.txt`).

| Size | capsid source | capsid trusted bytecode | Node 24.14 | Deno 2.9.7 | workerd |
|---:|---:|---:|---:|---:|---:|
| 10k | 18.4 | **17.4** | 57 | 20 | 20 |
| 100k | 22.8 | **18.3** | 57 | 31 | 22 |
| 1M | 76.1 | **28.3** | 81 | 55 | 38 |

Cold PSS (MB, same samples): capsid source 0.8/1.2/4.6, bytecode 0.9/1.7/10.2, Node 59.6/60.2/76.3, Deno 34.9/36.4/41.4, workerd 49.8/51.7/64.4.

- **capsid trusted bytecode is flat across sizes**: 17.4 → 18.3 → 28.3 ms, because deserialization replaces parsing; at 1M it is 2.7× faster than capsid source, 2.9× faster than Node, and ~1.9× faster than Deno.
- **Deno and workerd have the strongest small-bundle floors** (~20 ms); Node's ~57 ms floor is process + V8 bootstrap. Source-mode parsing shows up at 1M for every source-loading engine (capsid source 76.1, Node 81, Deno 55).
- **Cold memory stays small for capsid** (≤10.2 MB even at 1M bytecode) versus 35-76 MB for the comparison engines.

## 5. Retired Checkpoints

The 2026-08-20 `v0.2.0-rc.07` checkpoint (`four-qps-rc07-clean-20260820T170500`, `four-qps-rc07-clean-profile-20260820T173200`, `cold-start-rc07-clean-20260820T173500`) is superseded by the five-stack v0.2.2 point above: it measured a two-worker serving model, a 12-workload matrix, and a cold-start `total_ms` whose shell-measured side was inflated by the warm loop. The 2026-08-18 AMD Ryzen checkpoints, the 2026-08-18 Intel i5-12400F 6C/12T conclusion-adjacent tables (`b39acee`/`build-win`), and the 2026-08-14 4C tables were retired earlier. They remain available in git history and in the raw artifacts under `bench/results/` referenced by the older revisions of this document.
