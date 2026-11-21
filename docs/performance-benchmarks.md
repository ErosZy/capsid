# Performance: Current Measurements and Methodology

Reviewed 2026-11-20. The latest accepted local measurement is a **development
snapshot** (`0.3.0-beta.4`, `82eaab9`), not a published `v0.3.0-beta.5` or
stable `v0.3.0` benchmark. The [current status](current-status.md) and
[generated data](current-status.json) separate release identity, measured
source, qualification and historical data.

## Latest Accepted Record (`capsid-perf-lane-v3`)

| Field | Recorded value |
| --- | --- |
| Measured source | `82eaab999cfd628dbe1529eb553f63c0ee52a9ea` |
| Measured tree | `5e61f517d5325704c8dc38e35b0c63193fb91ec1` |
| Session | `local-wsl-v030-beta4-v3-20261120T052109Z-82eaab99` |
| Contract / matrix | `capsid-perf-lane-v3` (pure-JS seven-stack across three reference tiers + `dynamic-api`) |
| Completed measurement date | 2026-11-20 UTC |
| Machine/lane | i5-12400F; 12 logical CPUs; measured set 0–3; WSL2 Linux |
| Runtime posture | Clean Release/LTO (`CAPSID_USE_MIMALLOC=OFF`); strict C-ABI cold workers; managed/strict serving and density |
| Record status | All three lanes (`4533` sealed files) and whole-session independent review accepted |
| Published `v0.3.0-beta.5` | Source `c7a80b9`; **not measured by this session** |

The archive label is `v0.3.0-beta.4`, but its measured commit was untagged and
its `sourceTag` is empty. Source-tree equality does not change a binary's
embedded commit or make a later binary performance-equivalent. This record does
not qualify its measured snapshot for release or close all historical runtime
issues.

## Serving and Memory (Three-Tier Pure-JS Matrix)

These are medians of five 15-second measured rounds, after five seconds of
warm-up, using 64 keep-alive connections and 64 in-flight requests. Each round
verifies exact frozen bytes (`exact-bytes-v2`, zero errors/timeouts/mismatches).
`dynamic-api` executes per-request `new URL` query parsing, header reads, an
`async` microtask boundary, dynamic 8-item JSON order construction, FNV-1a
checksum calculation (`x-bench-checksum`), and `JSON.stringify`.

| Stack | Reference tier / isolation | JSON 1 KiB | JSON 16 KiB | Stream 1 KiB | Dynamic API | Trickle PSS MiB | Loaded PSS MiB | 1K QPS/MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Capsid | T1 · Strict Linux OS sandbox (`NS+Landlock+Seccomp`) | 21,770.8 | 13,952.2 | 10,756.9 | 9,974.9 | 14.9 | 18.6 | 1,170.5 |
| workerd | T1 · V8 Isolate (`workerd 2026-10-10`) | 27,649.5 | 20,523.0 | 16,211.3 | 20,501.0 | 46.6 | 609.7 | 45.3 |
| Deno | T1 · Permission sandbox (`2.9.8 --allow-net=127.0.0.1`) | 142,309.0 | 45,383.6 | 85,061.8 | 55,614.0 | 41.4 | 64.2 | 2,216.7 |
| Node `--jitless` | T2 · Single process (no sandbox, V8 no-JIT) | 9,462.8 | 9,336.6 | 8,386.0 | 7,201.7 | 56.4 | 113.9 | 83.1 |
| Bun `--jitless` | T2 · Single process (no sandbox, `BUN_JSC_useJIT=0`) | 137,504.3 | 43,909.4 | 122,787.8 | 43,180.4 | 28.7 | 31.6 | 4,351.4 |
| Node.js | T3 · Single process (no sandbox, V8 JIT `24.14.0`) | 67,447.5 | 38,454.2 | 44,000.9 | 33,853.1 | 58.4 | 75.4 | 894.5 |
| Bun | T3 · Single process (no sandbox, JSC JIT `1.4.5`) | 139,067.3 | 41,295.4 | 129,431.5 | 76,186.5 | 30.6 | 40.9 | 3,400.2 |

Trickle is 15 seconds at 10 req/s on a fresh forest, **before** loaded traffic.
PSS includes the complete owned service forest; rounded roles need not sum to
the independently rounded total. Latency percentiles in the raw summaries are
from the last measured round, not medians across five rounds.

## Cold Start and Density

Cold start ends at the first validated response. Each cell has one discarded
run plus five measured rounds; 50 warm requests and stable worker-tree PSS are
recorded separately (under `v3`, cold and QPS PSS settle checks allow an
explicit `32 KiB` allocator page-flap tolerance while density remains exact
`0 KiB`).

| Runtime | 10 KiB total ms / PSS | 100 KiB total ms / PSS | 1 MiB total ms / PSS | Warm request ms |
| --- | ---: | ---: | ---: | ---: |
| Capsid · bytecode | 5.51 ms / 6.6 MiB | 6.94 ms / 7.5 MiB | 18.47 ms / 15.2 MiB | 0.074–0.078 |
| Capsid · source | 6.42 ms / 6.6 MiB | 10.78 ms / 7.7 MiB | 68.83 ms / 18.7 MiB | 0.077–0.079 |
| workerd | 14.91 ms / 49.8 MiB | 16.44 ms / 50.8 MiB | 35.82 ms / 64.9 MiB | 0.176–0.185 |
| Deno | 30.11 ms / 39.4 MiB | 35.75 ms / 40.7 MiB | 108.04 ms / 48.1 MiB | 0.117–0.132 |
| Bun `--jitless` | 9.00 ms / 27.5 MiB | 13.99 ms / 28.9 MiB | 61.63 ms / 38.4 MiB | 0.109–0.113 |
| Bun | 8.80 ms / 29.3 MiB | 14.24 ms / 30.9 MiB | 60.85 ms / 39.9 MiB | 0.113–0.115 |
| Node.js | 43.27 ms / 57.6 MiB | 48.92 ms / 57.7 MiB | 70.21 ms / 72.3 MiB | 0.211–0.227 |
| Node `--jitless` | 74.30 ms / 80.1 MiB | 74.92 ms / 80.6 MiB | 95.28 ms / 95.5 MiB | 0.185–0.194 |

The C-ABI probe itself is excluded from cold worker PSS. Other stacks use the
recorded HTTP client/transport rather than the IPC entry, so transport overhead
must not be presented as an engine-only comparison. All sizes/stacks and the
matching binary/compiler identities are in the site data and archives.

Density uses a 2048 MiB budget and original coverage/startup/request deadlines.
Every worker/cell must complete one cold or 100 warm requests; counts below are
budget-limited, not arbitrary cap-limited observations.

| Stack | Phase | 10 KiB | 100 KiB | 1 MiB |
| --- | --- | ---: | ---: | ---: |
| Capsid workers | Cold | 735 | 503 | 128 |
| Capsid workers | Warm | 731 | 503 | 128 |
| workerd cells | Cold | 1041 | 901 | 247 |
| workerd cells | Warm | 604 | 553 | 219 |

These sampled windows do not prove long-duration memory retention or universal
maximum density. Heap diagnostics, service-tree PSS, individual worker cost and
pool counts are different metrics and must not be substituted for each other.

## Evidence and Replay

The [accepted independent review](data/performance-review.json) binds the
original exits, quiet windows, twelve density boundaries, nine executables and
measured source closure (`5467` files in
[performance-source-manifest-v030-beta4.json](data/performance-source-manifest-v030-beta4.json)).
Physical Windows-host exclusivity is **not** claimed.

Both the new `v3` session (`4533` sealed files) and the archived `v1`
cross-language session (`485b29be6cc8d0dcdb653ee6226956ddece02948`,
`local-wsl-v030-headers-ready-20261115T235007Z-7368b4ec`, `5123` sealed files;
source manifest [performance-source-manifest.json](data/performance-source-manifest.json))
are retained together (`9656` total sealed files across 6 compressed transports)
under `perf-site/archives/`. The [transport index](../perf-site/archive-index.json)
records every part digest, file count and uncompressed length. Site preparation:

1. verifies part and complete transport digests;
2. restores only bounded regular portable members into a new scratch directory;
3. runs the unchanged complete lane/point validation and replays raw metrics for both `v3` and `v1`;
4. verifies every original checksum and the exact bytes published for download.

Compression changes transport, not the accepted source, samples, counts, budgets
or verdict. It is not permission to repair a failed run. The
[site instructions](../perf-site/README.md) describe replay/downloads.

## Historical Comparisons and Limits

The archived `v1` point (`485b29b`, 2026-11-16, cross-language Capsid / workerd /
FastAPI / Flask / PHP Slim: Capsid `9341.6 / 8090.2 / 6491.7 QPS`, `14.0 / 18.0 MiB`
PSS, `738 / 509 / 128` warm workers) and the historical unverified `v0.2.2` point
(`9489.7 / 7109.1 / 6523.2 QPS`, `882 / 568 / 132` warm workers) are preserved
unchanged. Do not infer a formal paired gain or single-variable causal attribution
across different matrix contracts (`v1` vs `v3`) or unverified historical points.

No formal paired v0.2.2-versus-current gain, physical-host equivalence or
engine-only causal attribution is claimed. Diagnostic ABBA percentages cannot
be added or promoted into this whole-session result. No new measurement was
run merely to refresh the documentation.

The normative protocol is [bench/SPEC.md](../bench/SPEC.md); the
[archive contract](../bench/perf-evidence.md) defines failure and provenance
rules. Source, fixture, controller, configuration and environment changes need
their own matching evidence. Required CI acceptance alone is not a benchmark.

## Historical source and preserved anchors

The original dated checkpoints and their numbers are preserved byte-for-byte in the
[captured document](history/archives/2026-11-17/performance-benchmarks.md.txt). They are not current release instructions.

<details>
<summary>Legacy section links</summary>

<a id="performance-evidence-rules-and-current-state"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L1) — performance-evidence-rules-and-current-state

<a id="capsid-review-phase-93"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L11) — capsid-review-phase-93

<a id="第-93-轮2026-10-20五栈-qps-控制器与归档已接通"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L13) — 第-93-轮2026-10-20五栈-qps-控制器与归档已接通

<a id="capsid-review-phase-92"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L24) — capsid-review-phase-92

<a id="第-92-轮状态2026-10-20qps-负载正确性原始证据修复完整生产器继续"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L26) — 第-92-轮状态2026-10-20qps-负载正确性原始证据修复完整生产器继续

<a id="capsid-review-phase-91"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L35) — capsid-review-phase-91

<a id="第-91-轮状态2026-10-20编译器发布兼容修复完整目标继续"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L36) — 第-91-轮状态2026-10-20编译器发布兼容修复完整目标继续

<a id="capsid-review-phase-90"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L45) — capsid-review-phase-90

<a id="第-90-轮状态2026-10-20cold-控制器归档接通完整目标继续"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L46) — 第-90-轮状态2026-10-20cold-控制器归档接通完整目标继续

<a id="capsid-review-phase-89"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L59) — capsid-review-phase-89

<a id="第-89-轮状态2026-10-20cold-探针已修整车道与强兼容目标未完成"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L60) — 第-89-轮状态2026-10-20cold-探针已修整车道与强兼容目标未完成

<a id="1-evidence-rules"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L96) — 1-evidence-rules

<a id="conclusion-threshold"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L98) — conclusion-threshold

<a id="measurements-that-must-not-be-mixed"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L112) — measurements-that-must-not-be-mixed

<a id="result-storage-format"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L116) — result-storage-format

<a id="current-optimization-principles"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L120) — current-optimization-principles

<a id="2-test-environment-2026-10-10"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L130) — 2-test-environment-2026-10-10

<a id="3-five-stack-serving-matrix-2026-10-10-c64-5-rounds"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L156) — 3-five-stack-serving-matrix-2026-10-10-c64-5-rounds

<a id="reading-the-matrix"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L186) — reading-the-matrix

<a id="measured-density-fill-a-pool-to-2-gb-2026-10-10"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L194) — measured-density-fill-a-pool-to-2-gb-2026-10-10

<a id="4-cold-start-comparison-2026-10-10-median-ms-5-rounds"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L206) — 4-cold-start-comparison-2026-10-10-median-ms-5-rounds

<a id="5-retired-checkpoints"></a>
[Historical section](history/archives/2026-11-17/performance-benchmarks.md.txt#L222) — 5-retired-checkpoints

</details>
