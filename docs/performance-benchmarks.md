# Performance: Evidence Rules and Current State

This document is the single maintained document for performance topics; it keeps evidence rules, the latest conclusion-level checkpoints (2026-08-14), and clearly labeled observed-sample reruns. Historical optimization process (M1P, E1-E14, Host optimization loop) lives in git history and the raw artifacts in `bench/results/`, and is not maintained here.

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

## 2. Test Environment (2026-08-14, conclusion-level)

All conclusion-level checkpoints share the following environment:

| Item | Value |
|---|---|
| CPU | AMD Ryzen 3 3300X (4C/8T) |
| OS | Alpine Linux v3.24 (WSL2, kernel 6.6.87.2-microsoft-standard-WSL2) |
| Memory | 8 GB |
| Process protocol | SUT taskset 0-3 / loadgen 4-7; two-process model |
| Load protocol | conns=64, 12 workloads × 3 rounds (warmup 3s + measured 8s), correctness checked each round |

The stack under test (versions recorded in each manifest):

| Stack | Component and version |
|---|---|
| capsid + hono | capsid commit 9bde135 (build-m1d) + hono bundle (sha256 in manifest); static-pool 2 workers |
| PHP 8 + Slim | PHP 8.5.8 + Slim 4.15.2 + nginx 1.26.3 + php-fpm (pm.max_children=2) |
| Python 3 + Flask | Python 3.14.5 + Flask 3.1.3 + Gunicorn 26.0.0 (2 workers) |
| Cold-start extras | Node v24.18.0, Deno 2.9.3 |

## 2b. Observed Rerun Environment (2026-08-18, 6C/12T)

The observed-sample reruns in §3b/§4b share the following environment:

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

## 3. Three-Stack Full Matrix (4C, 2026-08-14, c64, 64K window)

Payloads are byte-aligned, 0 errors/0 timeouts, **33/36 cells at conclusion level (CV ≤ 7%)**; the php bytes16k, capsid stream32k, and python stream32k cells exceed CV and are recorded as observed samples (not marked in the table; raw samples available). Implementation language and server model differ—**not a leaderboard**, only for confirming magnitude. The capsid side uses the product default `--initial-stream-window 65536`. Raw samples: `bench/results/three-stack-20260814T172510/`.

| workload | capsid + hono | PHP 8 + Slim | Python 3 + Flask |
|---|---:|---:|---:|
| json 1k | **6820** | 1826 | 4625 |
| json 8k | **5213** | 1727 | 4683 |
| json 16k | **5304** | 1679 | 4495 |
| json 32k | **4558** | 1592 | 3865 |
| bytes 1k | **4591** | 1727 | 4510 |
| bytes 8k | **4405** | 1641 | 4375 |
| bytes 16k | 3971 | 1557 | **4252** |
| bytes 32k | 3414 | 1572 | **3908** |
| stream 1k | **4593** | 1745 | 4442 |
| stream 8k | **3952** | 1708 | 3570 |
| stream 16k | **3501** | 1652 | 3377 |
| stream 32k | 2886 | 1592 | **3756** |

**Shape**: regular JSON wins all cells, with the largest advantage at small payloads (json 1k is 1.47× the Python 3 stack and 3.74× the PHP 8 stack); for large byte/stream payloads (bytes ≥16k, stream 32k) the Python 3 stack overtakes, and the stream 32k gap (2886 vs 3756) cause is still under investigation. The PHP 8 stack is last across the full matrix (about 0.26-0.40× of capsid) with the best CV. The QuickJS interpreter (no JIT) remains the dominant single-worker latency factor; JIT is a vendor-level change and a separate evaluation project.

## 3b. Observed WSL Rerun (6C/12T, 2026-08-18, c64, 2 rounds)

The rerun keeps the 12 conclusion-level cells and adds the missing 4k/64k cells for a 6-size × 3-kind matrix (18 workloads), 2 rounds per cell. This is an **observed samples** run — profiles were not collected — so it does not replace the conclusion-level table above and no default capacities are frozen from it. All three stacks were pinned to CPUs 0-5, loadgen to 6-11. Raw samples and manifests: `build-win/bench-obs/results-20260818T102517/` and the 8k supplement `build-win/bench-obs/results-20260818T104504/` (local, git-ignored).

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

## 4. Cold-Start Comparison (4C, 2026-08-14, median ms)

Measurement class 4 (process creation, handshake, validation, loading, READY, and first response). Fixture is real-shaped JS source (three template rotations: loop + object-literal function, class, arrow/map/filter/sort chain), at 10k/100k/1M sizes (36/355/3547 top-level units); each side loads the same function body byte-aligned, differing only in entry point. capsid uses C ABI spawn→load (source/trusted bytecode)→READY→first response (bodyless IPC request); Node/Deno use process start→stdout READY→curl first request. Each cell drops 1 warmup round and takes the median of 5 rounds. Raw samples: `bench/results/cold-start-20260814T171047/`.

| Size | capsid source | capsid trusted bytecode | Node 24 source | Deno 2.9 source |
|---:|---:|---:|---:|---:|
| 10k | **9.5** | **8.2** | 110 | 39 |
| 100k | **19.6** | **10.6** | 110 | 40 |
| 1M | 141 | **42** | 149 | 53 |

READY times (same samples): capsid source 9.1/19.2/141.0, bytecode 7.8/10.1/41.4, Node 97/97/137, Deno 31/32/45.

1M phase breakdown (median): capsid source spawn 0.2 + transfer 7.6 + **compile 133** = 141; capsid bytecode spawn 0.2 + transfer 21.3 + **deserialize 20** = 41.4 (qjsb 2.46MB, QuickJS bytecode is uncompressed and 2.5× the source size); Node 97ms startup baseline + 40ms parse; Deno 31ms startup baseline + 14ms parse.

- **Size is significantly sensitive in real shapes**: capsid source 10k→1M total +132ms; compile cost is proportional to AST node count (3547 top-level units ≈133ms).
- **Trusted-bytecode benefit grows with compile cost**: 1M real source 141 → 41.7ms (−70%, 3.4×), bytecode path is first overall (21% faster than Deno, 3.6× faster than Node); at small sizes the benefit converges (10k only 1.3ms faster). But bytecode is not free: 2.5× size makes transfer 21.3ms the second-largest cost on that path, and deserialization still rebuilds the AST (≈20ms).
- **Source path is same magnitude as Node, slower than Deno**: 1M ready capsid 141 ≈ Node 137 (QuickJS full compile vs V8 parse + lazy compile), Deno 45ms is fastest on the source path—V8 parser advantage shows on AST-dense source.
- **Startup baseline dominates at small sizes**: at 10k capsid source 9.1ms ready is only 1/11 of Node and 1/3 of Deno; Node/Deno 97/31ms startup baselines cannot be amortized on small bundles.
- Semantic note: capsid first response goes through in-process IPC, while Node/Deno use local HTTP curl; "first request completes after ready" is aligned, but the request path implementation differs (ready→total delta: capsid ≈0.4ms, Deno ≈8ms, Node ≈13ms), so this is not an isomorphic comparison.

## 4b. Observed Cold-Start Rerun (6C/12T, 2026-08-18, median ms, 5 rounds)

Same fixture generator and measurement protocol as §4, on the 2026-08-18 WSL environment (SUT taskset 0-5). capsid side built from commit `b39acee`; Node v24.3.0, Deno 2.9.3. Raw samples: `build-win/bench-obs/cold-start-20260818T105257/` (local, git-ignored).

| Size | capsid source | capsid trusted bytecode | Node 24 source | Deno 2.9 source |
|---:|---:|---:|---:|---:|
| 10k | **5.1** | **4.9** | 47 | 32 |
| 100k | **9.2** | **5.6** | 45 | 33 |
| 1M | 58.2 | **15.1** | 71 | 33 |

READY times (same samples): capsid source 4.9/8.9/57.9, bytecode 4.6/5.4/14.9, Node 39/38/63, Deno 26/27/27.

The rerun confirms the 2026-08-14 shape on newer hardware: capsid starts 6-9× faster than Node and 1.2-6.5× faster than Deno at 10k/100k/1M source, and trusted bytecode is fastest overall at 1M (15.1ms). These numbers are observed samples, not a replacement for the conclusion-level cells above.

## 5. Resource Profile (4C, 2026-08-14, process count/PSS/RSS)

Three-stack resident (two-process protocol, idle) is sampled by `bench/sample-sut-memory.sh` every 15s for process-tree PSS (`smaps_rollup`) and RSS, median of 8 idle ticks; load rounds (json c64) are observed with 2 ticks only. Raw samples: `bench/results/sut-memory-20260814T173000/`.

| Stack | Processes | Idle PSS | Idle RSS | json load PSS |
|---|---:|---:|---:|---:|
| capsid + hono | 3 (host + 2 workers) | **12.3 MB** | 21.8 MB | 13.5 MB |
| PHP 8 + Slim | 12 (php-fpm + nginx) | —* | 124.1 MB | —* |
| Python 3 + Flask | 3 (gunicorn + 2 workers) | 62.6 MB | 89.4 MB | 62.6 MB |

*The php container processes span users; non-root cannot read `smaps_rollup`, so PSS is unavailable; its RSS includes nginx master+workers, which is a different measurement scope from the "application process tree" of the other two stacks.

- Idle PSS capsid is **1/5** of the Python 3 stack (12.3 vs 62.6 MB); RSS is 1/5.7 of the PHP 8 stack (21.8 vs 124.1 MB, the latter scope is inflated as noted above).
- Load delta: capsid PSS +1.2 MB at json c64 (QuickJS heap rises and falls with requests); Python/PHP show no visible delta (2-tick observation, small sample).