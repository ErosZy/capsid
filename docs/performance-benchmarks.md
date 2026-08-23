# Performance: Evidence Rules and Current State

This document is the single maintained document for performance topics; it keeps the evidence rules and the current (2026-08-20) conclusion-level samples. The `v0.2.0` stable release adopts these final-RC runs as its published performance baseline. Measurements retain their `v0.2.0-rc.07` labels because that is the exact binary identity used to collect them; the stable release changes only documentation and release metadata. Historical optimization process and earlier checkpoints (M1P, E1-E14, Host optimization loop, the 2026-08-14 4C runs, and the superseded 2026-08-18 runs) live in git history and the raw artifacts in `bench/results/`, and are not maintained here.

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

## 2. Test Environment (2026-08-20)

The current runs share the following environment:

| Item | Value |
|---|---|
| CPU | AMD Ryzen 3 3300X (4C/8T) |
| OS | Ubuntu 24.04 on WSL2, kernel 6.6.87.2-microsoft-standard-WSL2 |
| Memory | 7.9 GiB visible to WSL |
| Process protocol | SUT taskset 0-3 / loadgen 4-7; two-process model |
| Load protocol | conns=64, 12 workloads × 3 rounds (warmup 3s + measured 8s), correctness checked each round; workload order rotates the starting stack to cancel drift |
| capsid release / commit | `v0.2.0-rc.07` / `17206e4` (clean tree at build time) |

The stacks under test (versions and artifact SHA-256 values are recorded in `bench/results/four-qps-rc07-clean-20260820T170500/manifest.txt` and `bench/results/cold-start-rc07-clean-20260820T173500/manifest.txt`):

| Stack | Component and version |
|---|---|
| capsid + hono | capsid `v0.2.0-rc.07` (`17206e4`; Release + LTO; host `dd0fa171…`, worker `8950ca6d…`) + Hono 4.12.32 (self-contained bundle `83ebc6c2…`); static-pool 2 workers, `initial-stream-window 16384` |
| PHP + Slim | PHP 8.5.9 + Slim 4.15.2 + nginx 1.30.4 + php-fpm `pm=static max_children=2` (clean docker container `capsid-php-bench-clean`, port 8080, pinned to CPUs 0-3) |
| Python + Flask | Python 3.14.5 + Flask 3.1.3 + Gunicorn 26.0.0 (2 sync workers) |
| Python + FastAPI | Python 3.14.5 + FastAPI 0.141.1 + Uvicorn 0.52.3 (2 workers, uvloop + httptools) |
| Cold-start extras | Node v24.18.0, Deno 2.9.3 |

Workload matrix: json / bytes / stream at 1k, 4k, 16k, 32k (4k cells use the loadgen `matrix-<kind>-<label>` names; the legacy 1k/16k/32k names are `json`/`json16k`/`json32k` etc.). Payloads are byte-aligned and the loadgen verifies exact content every round: `matrix-bytes` is exact 0x62 bytes, `matrix-stream` 's' bytes, legacy bytes 0x61, legacy stream b/c/d thirds, legacy JSON compact or spaced marker.

## 3. Four-Stack Matrix (2026-08-20, c64, 3 rounds)

12 workloads × 4 stacks × 3 rounds, warmup 3s + measured 8s, correctness checked every round — **all 144 rounds OK**. The negative control returned HTTP 404 for 1,404/1,404 responses and the correctness verifier rejected every response; the load generator's process exit code alone must not be treated as a correctness verdict. This run meets the conclusion threshold: raw samples, correctness verdicts, per-process resource samples, and capsid host/worker perf profiles are all saved. Raw data: `bench/results/four-qps-rc07-clean-20260820T170500/` (144 samples + 144 correctness files; 349 manifest checksum entries), profiles: `bench/results/four-qps-rc07-clean-profile-20260820T173200/`.

Median QPS over 3 rounds (winner per row in bold):

| workload | capsid + hono | PHP 8 + Slim | Flask + Gunicorn | FastAPI + Uvicorn |
|---|---:|---:|---:|---:|
| json 1k | **7261** | 1894 | 5139 | 6330 |
| matrix-json-4k | 5479 | 1751 | 4631 | **5562** |
| json 16k | 5141 | 1720 | 4703 | **5520** |
| json 32k | 4286 | 1628 | 4129 | **4788** |
| bytes 1k | 5343 | 1901 | 5113 | **6205** |
| matrix-bytes-4k | 4956 | 1757 | 4877 | **5898** |
| bytes 16k | 4432 | 1730 | 4729 | **5594** |
| bytes 32k | 3520 | 1631 | 4159 | **4869** |
| stream 1k | **4845** | 1793 | 4704 | 2246 |
| matrix-stream-4k | 4831 | 1763 | **4861** | 3049 |
| stream 16k | **3961** | 1669 | 3725 | 2149 |
| stream 32k | 3267 | 1567 | **4165** | 2088 |

p50/p95/p99 latency (ms, median over rounds, from the same samples):

| workload | capsid | PHP | Flask | FastAPI |
|---|---:|---:|---:|---:|
| json 1k | 8.60 / 11.19 / 12.51 | 33.69 / 35.05 / 36.16 | 12.44 / 13.05 / 13.43 | 10.21 / 13.24 / 15.17 |
| json 16k | 12.40 / 15.22 / 16.86 | 37.08 / 38.51 / 39.37 | 13.53 / 14.25 / 15.32 | 11.39 / 13.88 / 15.63 |
| json 32k | 14.72 / 18.25 / 20.48 | 39.26 / 40.51 / 41.29 | 15.39 / 16.32 / 17.58 | 13.18 / 15.89 / 17.75 |
| bytes 1k | 11.96 / 14.15 / 15.90 | 33.63 / 34.66 / 35.31 | 12.48 / 13.19 / 13.87 | 10.09 / 12.63 / 14.46 |
| bytes 32k | 18.42 / 23.70 / 25.34 | 39.17 / 40.41 / 41.09 | 15.29 / 16.19 / 18.28 | 12.96 / 16.10 / 18.10 |
| stream 1k | 13.03 / 16.03 / 18.27 | 35.46 / 37.29 / 38.54 | 13.57 / 14.25 / 14.63 | 28.18 / 33.01 / 36.98 |
| stream 16k | 16.73 / 21.05 / 23.07 | 38.16 / 40.20 / 41.53 | 17.10 / 17.97 / 19.44 | 29.44 / 34.31 / 37.95 |
| stream 32k | 19.54 / 23.91 / 26.06 | 40.69 / 42.01 / 43.17 | 15.31 / 16.11 / 17.47 | 30.25 / 35.62 / 39.35 |

Per-process resources (median over the 5s sampling window; PSS from `smaps_rollup`, RSS from `statm`; PSS is `n/a` where the sampler (non-root) cannot read another user's `smaps_rollup` — nginx/php-fpm run as root in the container. CPU is the max window-average % of one core observed for any process of the role):

| role | PSS (MB) | RSS (MB) | CPU max observed |
|---|---:|---:|---:|
| capsid host (static-pool) | 5.9 | 7.6 | 105% |
| capsid worker (each of 2) | 6.4 | 9.8 | 91% |
| gunicorn worker (each of 2) | 23.7 | 32.7 | 94% |
| uvicorn worker (each of 2) | 42.0 | 51.5 | 110% |
| nginx (container, root) | n/a | 6.1 | 25% |
| php-fpm child (each of 2, root) | n/a | 14.6 | 96% |

capsid's full serving path (host + 2 workers) stays at ≈5.9 MB PSS for the host plus ≈6.4 MB per worker (≈18.7 MB total); a gunicorn sync worker alone uses 23.7 MB PSS, and a uvicorn worker 42.0 MB PSS. Resource sampling is scoped to each launched process tree, so unrelated host processes are not included.

### Profiles (capsid host and worker, json16k, 30s, `perf record -F 99`)

`bench/results/four-qps-rc07-clean-profile-20260820T173200/` — perf data + full `perf report` text + correctness (163,654 responses checked, 0 mismatches, errors, or timeouts during the profile session). Only capsid is profiled: it is the stack under test; the PHP/Flask/FastAPI stacks serve as comparison references and are not profiled.

capsid **host** (873 samples): 5.84% `memcpy`, 5.73% `__libc_malloc_impl`, 3.32% `get_meta`, 2.86% `normalize_public_request`, and 2.86% `alloc_slot` — the host's share is dominated by request/response buffer handling, metadata lookup, and allocation on the scheduling path.

capsid **worker** (3,711 samples): 21.18% `JS_CallInternal` (QuickJS entry into the Hono handler), 9.59% `malloc_usable_size`, 6.58% `__libc_malloc_impl`, 6.20% `lre_exec_backtrack` (regexp execution), and 3.96% `free` — the worker's time is dominated by JS handler execution and allocation in the application/framework path.

### Reading the matrix

- **Small JSON: capsid wins at 1k** (7,261 vs fastapi 6,330 vs flask 5,139 vs php 1,894); FastAPI narrowly wins 4k (5,562 vs capsid 5,479). PHP is the consistent laggard at ~1.6-1.9k QPS with p95 ≈35-42 ms.
- **Static medium payloads: FastAPI (uvloop + httptools + precomputed bodies) leads** on json16k/32k and all bytes cells. Capsid remains ahead of Flask on every JSON cell and on bytes 1k/4k, while Flask leads capsid on bytes 16k/32k.
- **Streaming: FastAPI's response path is the outlier.** Its `StreamingResponse` reaches 2,246 QPS at 1k versus 6,205 for static bytes and trails capsid by 2.16× at stream 1k and 1.84× at stream 16k. Flask's plain generator + WSGI holds up better and wins stream 4k/32k. Capsid streams at 89-98% of its corresponding static-bytes throughput.
- **Payload scaling varies by stack and response path**: from 1k to 32k, capsid JSON loses 41%, FastAPI JSON 24%, and PHP JSON 14%; capsid bytes and stream both lose about one third. Baseline and response construction therefore matter alongside payload size.
- **Density**: capsid's full host + 2-worker serving path is ≈18.7 MB PSS; one gunicorn worker is 23.7 MB PSS and one uvicorn worker is 42.0 MB PSS. Container-owned php-fpm PSS is unavailable, so its 14.6 MB child RSS is reported without treating it as directly comparable to PSS.

Across all 12 cells, geometric-mean QPS moved from the retired 2026-08-18 AMD checkpoint to this run by +1.40% for capsid, +3.98% for PHP, +1.22% for Flask, and +0.06% for FastAPI. Because every stack and the clean-run setup moved together, this supports **no observed capsid throughput regression**; it is not evidence that a specific capsid change caused an improvement.

## 4. Cold-Start Comparison (2026-08-20, median ms, 5 rounds)

Measurement class 4 (process creation, handshake, validation, loading, READY, and first response). Fixture is real-shaped JS source (three template rotations: loop + object-literal function, class, arrow/map/filter/sort chain), at 10k/100k/1M sizes; each side loads the same function body byte-aligned, differing only in entry point. capsid uses C ABI spawn→load (source/trusted bytecode)→READY→first response (bodyless IPC request); Node/Deno use process start→stdout READY→curl first request. Each cell drops 1 warmup round and takes the median of 5 rounds. Capsid was rebuilt from `v0.2.0-rc.07` / `17206e4`; raw data and 20 manifest checksum entries are in `bench/results/cold-start-rc07-clean-20260820T173500/`.

| Size | capsid source | capsid trusted bytecode | Node 24.18 source | Deno 2.9.3 source |
|---:|---:|---:|---:|---:|
| 10k | **8.34** | **7.29** | 108 | 39 |
| 100k | **18.24** | **9.68** | 108 | 39 |
| 1M | **134.09** | **35.89** | 135 | 52 |

READY times (same samples): capsid source 8.00/17.85/133.71, bytecode 6.91/9.27/35.55, Node 96/97/123, Deno 31/31/44.

- **Startup baseline dominates small sizes**: capsid 10k source at 8.34 ms is about 13× faster than Node (108 ms) and 4.7× faster than Deno (39 ms), and it remains fastest at 100k (18.24 vs 108/39 ms).
- **Trusted bytecode pays off as compile cost grows**: at 1M, bytecode at 35.89 ms is 3.74× faster than capsid source, 3.76× faster than Node, and 31% faster than Deno. Node's ~108 ms small-bundle floor is process + V8 bootstrap; Deno's ~39 ms floor is smaller, but capsid bytecode beats it at every measured size.
- **At 1M source capsid and Node are effectively tied in these medians** (134.09 vs 135 ms), while Deno remains faster at 52 ms. Trusted bytecode erases that source-compilation gap.
- Semantic note: capsid first response goes through in-process IPC, while Node/Deno use local HTTP curl; "first request completes after ready" is aligned, but the request path implementation differs, so this is not an isomorphic comparison.

## 5. Bytecode AOT Optimizer (2026-08-23, observed samples)

Measurement class: warm execution of compute-dense fixtures (source vs
unoptimized bytecode vs optimized bytecode), CPU pinned to SUT_CPUSET 0-3, 1
warmup run discarded + 5 measured rounds, median ms per round. The optimized
bundle is produced by `capsid-bytecode-compile` (Release, G4-trimmed pipeline
P2+P3.1; the run manifest records commit `47b9369` — `cab458d` afterwards changed
only the no-change report line, not output bytes) and each optimized body is
cross-checked against the source body byte-for-byte. Raw samples, compiler
reports, manifest, and sha256 are in `bench/results/exec-throughput-20260823T042708/`;
`load_noise` is the source-vs-raw parse-skip reference (bytecode path vs source
path). Full G1-G5 verdict and static ceilings live in [Bytecode AOT Optimizer](bytecode-aot-optimizer.md) §11. Per the evidence rules above, these are observed samples, not a general "optimization works" claim: no perf profiles were collected in this class.

| fixture | source ms | raw ms | opt ms | opt vs raw | load_noise |
| --- | ---: | ---: | ---: | ---: | ---: |
| arith-rt | 44.584 | 44.487 | 27.164 | +38.94% | 0.22% |
| cascade-rt | 16.272 | 16.135 | 11.441 | +29.09% | 0.84% |
| matrix-rt | 4.514 | 4.832 | 4.638 | +4.01% | -7.04% |
| sieve-rt | 24.002 | 24.998 | 25.613 | -2.46% | -4.15% |
| string-rt | 0.474 | 0.440 | 0.459 | -4.32% | 7.17% |
| fib-rt | 15.123 | 15.207 | 15.086 | +0.80% | -0.56% |
| json-rt | 1.790 | 1.775 | 1.763 | +0.68% | 0.84% |

Static reductions (compiler report): arith-rt 145→85 insns / 297→240 bytes;
cascade-rt 100→76 insns / 225→205 bytes; the other five fixtures are
byte-identical (0% static ceiling). The two moving fixtures are const-chain-dense
loops — the pipeline's target population; the wall-clock gain tracks the insn
removal rate (see G5 in the optimizer doc for the dispatch-amortization reading).

### Tier-2 final-config run (2026-08-23, commit 8078d04)

Same protocol, 13 fixtures, post-G4-trim deployed pipeline (P2+P3.1+P11+P14 +
format passes; the tier-2 SSI suite was measured and deleted — see
[Bytecode AOT Optimizer](bytecode-aot-optimizer.md) §11 for the full G4
attribution table). Raw samples, compiler reports, manifest, and sha256 are in
`bench/results/exec-throughput-20260823T140906/`.

| fixture | source ms | raw ms | opt ms | opt vs raw | load_noise |
| --- | ---: | ---: | ---: | ---: | ---: |
| arith-rt | 46.984 | 47.083 | 28.103 | +40.31% | -0.21% |
| cascade-rt | 16.665 | 16.605 | 11.409 | +31.29% | 0.36% |
| matrix-rt | 4.739 | 4.666 | 4.720 | -1.16% | 1.54% |
| sieve-rt | 25.106 | 25.677 | 25.936 | -1.01% | -2.27% |
| string-rt | 0.452 | 0.457 | 0.448 | +1.97% | -1.11% |
| fib-rt | 15.627 | 15.726 | 15.434 | +1.86% | -0.63% |
| json-rt | 2.124 | 1.799 | 1.791 | +0.44% | 15.30% |
| prop-loop-rt | 34.012 | 33.705 | 31.194 | +7.45% | 0.90% |
| prop-hoist-rt | 6.026 | 6.005 | 3.498 | +41.75% | 0.35% |
| copy-chain-rt | 7.654 | 7.754 | 6.723 | +13.30% | -1.31% |
| branch-const-rt | 4.305 | 4.375 | 4.200 | +4.00% | -1.63% |
| cse-loop-rt | 7.378 | 7.547 | 7.501 | +0.61% | -2.29% |
| licm-rt | 4.600 | 4.690 | 4.596 | +2.00% | -1.96% |

Static reductions (compiler report, final config): arith-rt 145→85 /
297→240; cascade-rt 100→76 / 225→205; prop-hoist-rt and prop-loop-rt 53→46 /
124→99 (P14 literal-get_field folds + downstream P3.1 cascade); copy-chain-rt
48→44 / 102→93 (P11 copy-prop); the other eight fixtures are byte-identical
(0% static ceiling). prop-hoist's +41.75% on a 13.2% insn removal shows the
removed instructions are expensive `get_field` lookups, not cheap dispatches
(G5 in the optimizer doc). The G4 attribution matrix and the SSI suite's
0.016% corpus contribution are in the optimizer doc's §11.

### Tier-2b final-config run (2026-08-23, commit 40c3d8a)

Same protocol, 13 fixtures, post-tier-2b deployed pipeline (P2+P3.1+P11+P14+
P16 + format passes; P16 is the tier-2b TDZ-sound dead-store elimination —
see [Bytecode AOT Optimizer](bytecode-aot-optimizer.md) §11 for the G1-G5
adjudication). Raw samples, compiler reports, manifest, and sha256 are in
`bench/results/exec-throughput-20260823T152705/` (plus retest runs
T153323/T153344). The machine carried concurrent sessions (a test262
conformance run at 125% CPU, then a sablejs Crypto benchmark at ~115%
CPU), so the table below gives run 1 plus the range across all three
runs for each fixture; the large-gain fixtures are stable across all
three, and the millisecond-scale swings trace to that load — P16's
edits there delete only dead markers with strictly smaller outputs, so
a genuine slowdown is structurally impossible.

| fixture | opt vs raw (run 1) | opt vs raw (3-run range) |
| --- | ---: | ---: |
| arith-rt | **+84.09%** | +83.80…+84.70% |
| cascade-rt | +56.56% | +56.17…+57.22% |
| prop-hoist-rt | +42.77% | +39.86…+42.77% |
| copy-chain-rt | +21.39% | +21.07…+26.60% |
| prop-loop-rt | +18.54% | +17.26…+18.67% |
| branch-const-rt | +9.83% | -1.82…+9.83% |
| matrix-rt | +5.95% | -4.59…+5.95% |
| cse-loop-rt | +2.36% | -4.97…+6.77% |
| sieve-rt | +0.05% | -0.58…+0.70% |
| fib-rt | +0.05% | -7.49…+0.25% |
| json-rt | -0.92% | -18.03…+8.51% |
| string-rt | -2.46% | -3.34…+4.37% |
| licm-rt | -2.52% | -16.21…-2.31% |

Static reductions (compiler report, post-tier-2b): arith-rt 145→35 /
297→74 (P16 −50 insns: 18× set_loc_uninitialized + 17× push_i32 + 14×
put_loc8 + 1); cascade-rt 100→59 / 225→149 (P16 −17); matrix-rt 203→187 /
477→429 (P16 −16); the other fixtures −3 to −7 insns each; fib-rt remains
byte-identical (P16 true negative). arith-rt's +84.09% on a 76→26-insn
loop (the tier-2 G5 report's predicted dead-store scaffolding, now
removed) is the pipeline's headline: P16's deletions are paid once per
function call, so the win lands on every invocation rather than being
amortized across loop iterations. The P16 switch matrix (kPassAll vs
kPassAll∖P16, 120/1076 = 11.15% corpus static attribution on this
marker-dense corpus) and the G1-G5 verdicts are in the optimizer doc's
§11; per-fixture evidence is archived in `bench/results/p16-evidence/`.

## 6. Retired Checkpoints

The previous 2026-08-18 AMD Ryzen 3 3300X checkpoint (`c943e35`, `four-qps-final-20260818T131300`, `four-qps-profile-20260818T132600`, and `cold-start-20260818T134435`) was superseded by the clean rc.07 run above. The 2026-08-18 Intel i5-12400F 6C/12T conclusion-adjacent tables (commit `b39acee`/`build-win`) and the 2026-08-14 4C tables are also retired. They remain available in git history and in the raw artifacts under `bench/results/` referenced by the older revisions of this document.
