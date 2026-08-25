# Performance: Evidence Rules and Current State

This document is the single maintained document for performance topics; it keeps the evidence rules, the current (2026-08-20) release-level samples, the 2026-08-25 QuickJS specialization review, and the 2026-08-25 clean retained-optimizer validation. The `v0.2.0` stable release adopts the final-RC runs as its published performance baseline. Measurements retain their `v0.2.0-rc.07` labels because that is the exact binary identity used to collect them; the stable release changes only documentation and release metadata. Historical optimization process and earlier checkpoints (M1P, E1-E14, Host optimization loop, the 2026-08-14 4C runs, and the superseded 2026-08-18 runs) live in git history and the raw artifacts in `bench/results/`, and are not maintained here.

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

### Retained-set source-service checkpoint (2026-08-25)

After rebooting WSL into an otherwise idle environment, commit `cfeae0b` was
built Release + LTO with opcode profiling, field IC, and ext34 all compiled
OFF. The same four-stack protocol then completed 144/144 correctness checks
with zero errors or timeouts. The clean main run is
`bench/results/four-qps-current-clean-20260825T161800/` (349 checksum entries;
checksum-list SHA-256 `67ffb510…`), the four-cell repeat is
`bench/results/four-qps-current-clean-focused-20260825T164700/`
(`94272dee…`), and the profile is
`bench/results/four-qps-current-clean-profile-20260825T164600/`
(`c134cda4…`). The negative control returned 1,402 HTTP 404 responses and the
verifier rejected all 1,402, recording zero successful QPS.

The main-run median QPS values were:

| workload | capsid + hono | PHP + Slim | Flask + Gunicorn | FastAPI + Uvicorn |
|---|---:|---:|---:|---:|
| json 1k | **7042** | 1872 | 5068 | 6260 |
| matrix-json-4k | **5601** | 1746 | 4663 | 5504 |
| json 16k | 5070 | 1731 | 4730 | **5520** |
| json 32k | 4077 | 1573 | 4011 | **4617** |
| bytes 1k | 5168 | 1839 | 4985 | **5954** |
| matrix-bytes-4k | 4714 | 1705 | 4704 | **5665** |
| bytes 16k | 4214 | 1670 | 4596 | **5401** |
| bytes 32k | 3273 | 1579 | 4071 | **4667** |
| stream 1k | **4753** | 1749 | 4608 | 2160 |
| matrix-stream-4k | 4728 | 1748 | **4861** | 3042 |
| stream 16k | **3830** | 1676 | 3743 | 2131 |
| stream 32k | 3218 | 1574 | **4180** | 2081 |

This is a cross-time checkpoint, not a same-session old/new A/B. The three
external stacks quantify common machine drift; the last column below divides
the capsid ratio by the geometric mean of those three ratios:

| workload | capsid vs 2026-08-20 | external-stack drift | drift-adjusted capsid |
|---|---:|---:|---:|
| json 1k | -3.01% | -1.22% | -1.81% |
| matrix-json-4k | +2.23% | -0.22% | +2.45% |
| json 16k | -1.39% | +0.41% | -1.80% |
| json 32k | -4.86% | -3.25% | -1.67% |
| bytes 1k | -3.27% | -3.26% | -0.01% |
| matrix-bytes-4k | -4.89% | -3.48% | -1.46% |
| bytes 16k | -4.91% | -3.23% | -1.74% |
| bytes 32k | -7.02% | -3.17% | -3.98% |
| stream 1k | -1.90% | -2.80% | +0.92% |
| matrix-stream-4k | -2.13% | -0.37% | -1.77% |
| stream 16k | -3.30% | +0.01% | -3.31% |
| stream 32k | -1.52% | +0.16% | -1.67% |
| 12-cell geometric mean | **-3.02%** | **-1.71%** | **-1.33%** |

The per-stack geometric-mean changes were capsid -3.02%, PHP -1.64%, Flask
-1.27%, and FastAPI -2.22%. A focused repeat did not reproduce a
capsid-specific large-payload loss: when JSON/bytes 32k slowed again, all four
stacks slowed by 10-18%; stream 4k returned to 4,697 QPS with 1.1% CV, and
stream 16k returned to 3,905 QPS with 1.3% CV. Therefore the cross-time center
does **not** establish either an HTTP throughput win or a product regression.
It is a passed safety screen with a small unresolved cross-time center.

This source-bundle matrix also does not execute BC26's trusted-bytecode AOT
pipeline, so it cannot attribute its QPS to that optimizer. Its value here is
the product-level source/runtime, resource, and correctness gate. The direct
raw-bytecode versus optimized-bytecode evidence remains the AOT attribution.

Resources stayed flat: capsid host PSS was 6.2 MB and each worker 6.4 MB
(about 19.0 MB total), versus 5.9/6.4 MB (18.7 MB total) in the release run.
There is no accumulating IC sidecar state. The current profile captured 831
host and 3,694 worker samples with zero lost samples and 163,203 validated
responses. Host leaders were `memcpy` 7.22%, allocator 6.02%, and `get_meta`
3.01%. Worker leaders remained `JS_CallInternal` 20.36%,
`malloc_usable_size` 10.42%, malloc 6.74%, regexp backtracking 5.44%, and
`JS_GetPropertyInternal` 3.68%. The profile explains why dispatch-focused VM
wins do not pass through proportionally to source-Hono request QPS.

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

### Retained-set cold-start checkpoint (2026-08-25)

The same clean `cfeae0b` build repeated the five-round protocol. Raw evidence
is in `bench/results/cold-start-current-clean-20260825T164500/` (20 checksum
entries; checksum-list SHA-256 `1de6b30c…`).

| Size | capsid source | capsid bytecode | Node source | Deno source |
|---:|---:|---:|---:|---:|
| 10k | 8.43 ms (+1.08%) | 7.45 ms (+2.19%) | 110 ms (+1.85%) | 39 ms (0%) |
| 100k | 18.35 ms (+0.60%) | 9.58 ms (-1.03%) | 109 ms (+0.93%) | 39 ms (0%) |
| 1M | 133.63 ms (-0.34%) | 36.23 ms (+0.95%) | 137 ms (+1.48%) | 52 ms (0%) |

Parentheses are latency change versus the 2026-08-20 release baseline, so
positive is slower. All capsid cells stayed within +/-2.2%; there is no
observed cold-start regression. At 1M, trusted bytecode remains 3.69x faster
than capsid source and 1.44x faster than Deno source.

## 5. Bytecode AOT Optimizer (final BC26 configuration)

This section keeps only the final deployed result. Intermediate v1, tier-2, and
tier-2b tables remain in git history; implementation and soundness are maintained
in [Bytecode AOT Optimizer](bytecode-aot-optimizer.md).

The execution harness compares source, unoptimized bytecode, and optimized
bytecode on pinned CPUs, discards one warmup, and records five measured samples.
Optimization attribution always uses optimized versus unoptimized bytecode;
source additionally measures parsing/compilation and is not the optimization
baseline. Raw manifests, samples, reports and hashes are under
`bench/results/exec-throughput-20260823T152705/`,
`bench/results/exec-throughput-20260823T153323/`,
`bench/results/exec-throughput-20260823T153344/`, and
`bench/results/p16-evidence/`.

The deployed pipeline is P11/P14/P16/P2/P3.1 plus compaction, pc2line remapping,
checksum rebuild and verification.

### Breadth and pass decisions

| Evidence set | Result | Interpretation |
| --- | ---: | --- |
| 12,645-instruction tier-2 corpus | SSI suite added 2 unique removals (0.016%) | P9-P15' deleted; direct P11/P14 retained |
| 12,233-instruction bundle corpus | P16 removed 88 instructions (0.72%) | Best current breadth estimate for P16 |
| 1,076-instruction compute fixture corpus | P16 removed 120 instructions (11.15%) | Deliberately marker-dense mechanism corpus, not production prevalence |
| `arith-rt` optimized loop | 76 → 26 instructions | Entry marker/producer/store scaffolding was the dominant synthetic cost |

The large fixture effects are real mechanism evidence but not broad product
claims. The final three recorded runs produced:

| fixture | final instructions | optimized vs raw |
| --- | ---: | ---: |
| arith-rt | 145 → 35 | +83.80%..+84.70% |
| cascade-rt | 100 → 59 | +56.17%..+57.22% |
| prop-hoist-rt | 53 → 43 | +39.86%..+42.77% |
| copy-chain-rt | 48 → 39 | +21.07%..+26.60% |
| prop-loop-rt | 53 → 43 | +17.26%..+18.67% |

Those runs shared a machine with concurrent sessions; the five large effects
were stable, while millisecond-scale fixtures changed sign and are intentionally
not used for a performance conclusion. No result here supports a general
20%+ request-throughput claim. The 0.72% bundle-corpus attribution is the
relevant warning against extrapolating the marker-dense fixtures.

Correctness for the final configuration: 10/10 targeted ctest groups, optimized
round-trip and source-worker differential coverage, 40,000 ASan/UBSan fuzz
cases, and byte-for-byte determinism. P14 also demonstrated why instruction
count is not a cost model: removing a small number of expensive `get_field`
operations produced more wall-clock benefit than removing the same number of
cheap dispatches.

### Clean retained-set execution validation (2026-08-25)

The clean post-reboot run used the shipped BC26 `kPassAll` configuration and
compared optimized bytecode directly with raw bytecode. One warmup was dropped
and five measured executions were retained per mode. Evidence is in
`bench/results/exec-throughput-current-clean-20260825T164400/` (172 artifact
checksums after excluding the checksum file itself; checksum-list SHA-256
`4b0c21b8…`).

| fixture | raw median | optimized median | optimized vs raw |
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
speedup is +25.89%, and summing fixture medians gives +41.23%; both aggregate
numbers are synthetic-suite scores dominated by arith/cascade and must not be
reported as request-throughput improvement. The two negatives are 12 us and
147 us absolute differences on short fixtures; the pre-reboot string result
was -6.61% and fell to -0.60% after reboot, confirming substantial environment
noise at this scale.

The more representative cumulative gate enabled **all retained items**: BC26
`kPassAll` plus the mixed-number `mul` fast path, compared directly with the
patchless/add-loc control. Eight Kraken/Octane programs used seven balanced
pairs each. Evidence is in
`bench/results/all-effective-cumulative-20260825/` (67 artifact checksums;
checksum-list SHA-256 `81764e13…`; summary SHA-256 `35d1840c…`). Positive means
the complete retained candidate is faster:

| program | gain, paired 95% CI | positive pairs |
|---|---:|---:|
| Kraken Beat Detection | +1.23% [-2.98%, +5.62%] | 4/7 |
| Kraken DFT | -0.63% [-8.16%, +7.53%] | 3/7 |
| Kraken FFT | **+2.61% [+0.40%, +4.87%]** | 5/7 |
| Kraken Oscillator | **+4.17% [+2.03%, +6.36%]** | 7/7 |
| Kraken Darkroom | **+6.78% [+5.63%, +7.93%]** | 7/7 |
| Octane Box2D | **+1.33% [+0.59%, +2.08%]** | 7/7 |
| Octane Navier-Stokes | **+5.89% [+5.37%, +6.42%]** | 7/7 |
| Octane Richards | +2.10% [-6.06%, +10.96%] | 4/7 |
| **equal-weight geometric mean** | **+2.91% [+0.84%, +5.02%]** | 7/8 positive centers |

This is the answer to whether the retained optimizations work together: **yes,
the combined classic-suite gate is significantly positive and has no
significant per-program regression.** Kraken's suite center is +2.80% and
Octane's +3.09%, although their program-dispersion intervals cross zero due to
the small program count. Keep BC26 plus mixed-number `mul`; keep IC, BC27,
ext34, and store/reload fusion disabled or removed as recorded below.

## 6. Runtime Specialization Status (2026-08-24)

R0's single-op array ext is a measured negative and is no longer emitted. Its
target fixture regressed by 12.69% because the generic property helper already
contains the same dense-array fast path and `OP_ext` added another indirect
dispatch. The ext id remains reserved so archived experimental bytecode cannot
be reinterpreted.

The branch now contains two non-production mechanisms:

- source-attributed exact-site opcode profile v3, used only to identify actual
  helper paths and hot application PCs without folding bootstrap execution into
  AOT candidates; and
- a compile-gated, runtime-only monomorphic own-data `get_field` IC with
  bounded sidecars, generic fallback, dequickening, and canonical
  serialization.

The IC correctness and rollback tests pass. An uninstrumented O3 + `NDEBUG` +
LTO no-go screen was then run on the AMD Ryzen 3 3300X. The initial screen used
seven balanced ABBA/BAAB pairs per arm. OFF and ADAPTIVE used the same worker
binary; the end-to-end screen also used the same host binary, two workers, 64
connections, and CPUs 0-3 for the SUT / 4-7 for the load generator. Every
worker response body and every load-generator correctness verdict matched.

Single-worker latency uses a paired log-ratio mean and Student-t 95% interval;
positive means ADAPTIVE is faster:

| case | OFF median | ADAPTIVE median | gain, paired 95% CI | positive pairs | quickened / dequickened |
| --- | ---: | ---: | ---: | ---: | ---: |
| long-lived monomorphic receiver | 14.065 ms | 13.680 ms | **+2.82% [+2.16%, +3.49%]** | 7/7 | 3 / 0 |
| fresh receiver per request | 13.505 ms | 15.806 ms | **-18.98% [-29.11%, -7.39%]** | 0/7 | 3 / 3 |
| sequential Hono JSON | 0.252 ms | 0.253 ms | -0.63% [-2.05%, +0.82%] | 2/7 | 32 / 9 |

The matching host + two-worker Hono screen did not show a broad win:

| workload | QPS change, paired 95% CI | positive pairs | p95 change, paired 95% CI |
| --- | ---: | ---: | ---: |
| JSON | +2.56% [-1.05%, +6.30%] | 6/7 | -8.19% [-14.69%, -1.20%] |
| static bytes 4k | +0.80% [-3.98%, +5.83%] | 4/7 | -2.08% [-11.49%, +8.32%] |
| stream 4k | -3.86% [-9.95%, +2.65%] | 2/7 | +3.45% [-10.99%, +20.24%] |

The geometric mean of the three QPS centers is -0.20%. Both adaptive workers
allocated 47,956 bytes of IC state, quickened 153 sites, and dequickened
111/112 sites. The shared stderr makes their complete JSON reports interleave,
but these individual numeric records are intact. This short screen does not
replace the full section-1 profile/resource conclusion gate; it is sufficient
to reject enabling a mechanism that already has a large directed regression
and no broad throughput win.

A same-source PATCHLESS versus feature-compiled-but-OFF test also found a
**+1.92% [+0.72%, +3.14%] latency tax** on the property-dense monomorphic
fixture (6/7 pairs); fresh and Hono intervals crossed zero. Thus the ideal
+2.82% ADAPTIVE-vs-OFF result contains only about one percentage point of
headroom over this measured compile/layout tax. Independent sessions are not
subtracted to manufacture an exact net result.

The largest regression had a concrete implementation explanation. After eight
misses the direct opcode restored ordinary `get_field` and marked the site
denied, but every later generic access still called the no-inline observer and
updated the terminal site's miss counters. The fresh-receiver case therefore
kept paying observation work after the site had already decided it could not
serve.

That defect was fixed and regression-tested before making the final decision.
A terminal site now keeps the runtime opcode only as an observation-free route
into the shared generic `get_field` handler; it neither calls `js_ic_observe`
nor updates policy counters. A directed test executes another 100,000 accesses
after parking and proves that observations, misses, dequickens, and
megamorphic transitions stay unchanged. Serialization, teardown, mode-change,
ext round-trip, optimizer differential, overlay audit/key, and all directed IC
tests pass.

The repaired same-binary screen was increased to 21 balanced pairs. Positive
gain still means ADAPTIVE is faster:

| case | OFF median | ADAPTIVE median | gain, paired 95% CI | positive pairs | quickened / dequickened |
| --- | ---: | ---: | ---: | ---: | ---: |
| long-lived monomorphic receiver | 14.941 ms | 14.492 ms | +1.40% [-1.83%, +4.73%] | 12/21 | 3 / 0 |
| fresh receiver per request | 14.192 ms | 14.752 ms | -2.77% [-6.96%, +1.62%] | 6/21 | 3 / 3 |
| sequential Hono JSON | 0.255 ms | 0.256 ms | -0.60% [-2.36%, +1.18%] | 9/21 | 32 / 9 |

The repair recovered most of the old -18.98% fresh-receiver loss, proving the
diagnosis, but none of the three repaired same-binary intervals excludes zero.
More importantly, a direct 21-pair PATCHLESS-to-ADAPTIVE comparison measured
feature-worker latency relative to PATCHLESS (positive is a regression):

| case | ADAPTIVE latency change, paired 95% CI | regression pairs |
| --- | ---: | ---: |
| fresh receiver per request | **+7.31% [+5.15%, +9.51%]** | 20/21 |
| sequential Hono JSON | -0.94% [-2.84%, +0.99%] | 7/21 |
| long-lived monomorphic receiver | -0.25% [-4.70%, +4.40%] | 13/21 |

The matching 21-pair PATCHLESS-to-feature-built-OFF attribution produced
fresh +3.48% [-0.16%, +7.25%], Hono -0.52% [-2.88%, +1.89%], and mono +2.38%
[-1.47%, +6.38%]. These sessions are not arithmetically subtracted: the direct
PATCHLESS-to-ADAPTIVE comparison is the product verdict. The OFF centers also
remain far outside the pre-registered +/-0.5% zero-tax gate and are consistent
with the earlier significant +1.92% mono tax.

**Final verdict: stop this field-IC implementation and keep it compile-gated,
default OFF.** The terminal fix is correct and valuable diagnostic evidence,
but it does not turn the mechanism into a product win: the ideal mono case is
neutral against PATCHLESS, Hono is neutral, and fresh receivers regress
significantly. Do not spend on a second terminal opcode, POLY2, prototype/put
caches, or the full host/resource matrix for this design. Any later IC attempt
must first eliminate compiled-OFF observer/layout tax and prove a stable-site
win directly against PATCHLESS; same-binary OFF is not an adequate baseline.

Reproduction scripts are `bench/field-ic-ab.sh`,
`bench/field-ic-host-ab.sh`, and `bench/field-ic-off-tax.sh`. Raw data,
manifests, summaries, and SHA-256 lists are under
`bench/results/field-ic-ab-20260824T161100/`,
`bench/results/field-ic-host-ab-20260824T161900/`, and
`bench/results/field-ic-off-tax-20260824T162800/` for the first screen. The
terminal-fix evidence is under
`bench/results/field-ic-terminal-ab-20260824T165700-p21/`,
`bench/results/field-ic-terminal-net-20260824T170000-p21/`, and
`bench/results/field-ic-terminal-off-tax-20260824T170100-p21/`.

The CFG+SSA region path is decision-only. A future `OP_ext` proposal must bind
v3 observations to stable bundle/function/PC identities, contain at least two
instructions, have complete evidence for every member, and show a benefit the
generic handler does not already provide before implementation and A/B.

The follow-up source-attributed census used Hono, H3, itty-router, and Elysia,
then independently checked Kraken, Octane 2, and SunSpider. The common
`get_arg0 > get_field` pair was present across all four frameworks and in five
legacy-suite programs, so it was not selected from one synthetic microbenchmark.
Its framework dispatch-only ceiling was nevertheless just 1.026%: the fusion
still had to perform the complete generic property lookup.

An experimental six-byte BC27 implementation was correctness-clean and reduced
all four framework bundles, but failed an 11-pair balanced real-framework gate.
Candidate gain was Hono -0.95% [-2.00%, +0.20%], H3 -1.13%
[-2.11%, -0.08%], itty-router +0.89% [-0.06%, +2.31%], and Elysia -3.98%
[-4.88%, -3.13%]. The equal-weight combined result was **-1.28%
[-1.77%, -0.77%]**. The implementation and patch were removed; the census and
negative result remain as the decision record. Traditional suites are breadth
evidence only: 34/41 classic programs completed and their exact-site table had
24,832,010 overflowed insertions, while Capsid module conversion executed only
9/37 compilable cases due to harness/strictness incompatibilities.

This result narrows the next gate: do not add `get_locN + get_field` variants or
another ext whose only saving is one cheap dispatch. A fusion must also collapse
material helper, coercion, stack, or reference-count work. Any renewed IC must
first make the feature-compiled OFF path patchless-equivalent and encode a
per-bytecode-site slot directly; only then is a PATCHLESS-to-enabled A/B worth
running.

### ext34 post-review result (2026-08-25)

The first ext34 keep record (+0.66%/+0.86%) is invalid. Review found a
three-byte matcher buffer that could be overwritten when a two-slot
`get_loc0_loc1` was decoded with only one byte remaining. It could miscompile
pair+pair windows in optimized builds. The review also added missing BC27
argument/local operand bounds and restored the original `get_array_el`
exception-PC convention. The old timings are retained only as history.

After those fixes, a same-feature-binary `0x7f` versus `0x1ff` run used seven
balanced ABBA/BAAB pairs over eight pinned Kraken/Octane programs (224 timed
samples). The fusion itself was strongly positive where its windows were hot:

| program | candidate gain, paired 95% CI |
| --- | ---: |
| Kraken Beat Detection | **+9.72% [+8.80%, +10.66%]** |
| Kraken FFT | **+9.66% [+8.70%, +10.63%]** |
| Octane Navier-Stokes | **+3.22% [+2.39%, +4.07%]** |
| other five programs | intervals cross zero |
| equal-weight geomean | +2.96%, program CI [-0.46%, +6.49%] |

This is valid evidence that a three/four-instruction fusion can retain the
generic property slow path and still win by removing enough dispatches. It
retracts the earlier blanket claim that any fused handler containing a generic
slow path must lose.

It is not a production keep. A direct patchless-runtime versus
feature-capable-runtime comparison ran `0x7f` on both sides and verified all
eight BC26 blobs byte-for-byte. The feature-capable binary nevertheless
regressed by **-1.44%** equal-weight, program CI **[-2.50%, -0.37%]**. This is
runtime handler/layout tax; byte-identical serialization never measured it.

A separate direct PATCHLESS `0x7f` versus corrected enabled `0x1ff` run is the
net product comparison: Beat Detection +8.30% [+6.63%, +10.00%], FFT +8.26%
[+6.40%, +10.15%], Box2D **-2.21% [-2.90%, -1.51%]**, Richards **-3.22%
[-4.42%, -2.02%]**, and the other four intervals crossed zero except the
bytecode-identical Darkroom control at +1.65% [+0.91%, +2.39%]. Equal-weight
gain was +1.51% with program CI [-2.07%, +5.22%]. Thus the current enabled
binary has strong workload-specific wins but no broad keep and two significant
regressions. Darkroom's movement despite identical bytecode is direct evidence
that handler layout is workload-dependent.

Production therefore uses a real compile gate, default OFF. With
`CAPSID_ENABLE_EXT_FUSION34=OFF`, the ext formats, ids, reader checks and
handlers are absent, and standalone `quickjs.c.o`, `libqjs.a`, and `qjs` are
byte-identical to the pre-0045 build under the same flags. ON/OFF also enters
the bytecode compatibility record. ext34 remains a measurement backend, not a
shipped optimization. Do not add more default handlers until their final
enabled binary beats PATCHLESS and the default binary remains exactly zero
tax.

Correctness evidence includes default-OFF and feature-ON optimizer/CFG/ext
tests, invalid slot rejection in both readers, getter/coercion/null/exception
differentials, exact backtrace comparison, and ASan/UBSan. The classic corpus
proves performance and successful execution; its appended completion marker is
not a general semantic-output oracle and must not be described as one.

## 7. Retired Checkpoints

The previous 2026-08-18 AMD Ryzen 3 3300X checkpoint (`c943e35`, `four-qps-final-20260818T131300`, `four-qps-profile-20260818T132600`, and `cold-start-20260818T134435`) was superseded by the clean rc.07 run above. The 2026-08-18 Intel i5-12400F 6C/12T conclusion-adjacent tables (commit `b39acee`/`build-win`) and the 2026-08-14 4C tables are also retired. They remain available in git history and in the raw artifacts under `bench/results/` referenced by the older revisions of this document.
