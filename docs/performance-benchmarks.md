# Performance: Current Measurements and Methodology

Reviewed 2026-11-21. The latest accepted measurement uses a **local WSL2
Release/LTO rebuild** from the exact `v0.3.0-beta.5` tag. It measures that
build, **not the distributed release packages**. The [current status](current-status.md)
and [generated data](current-status.json) distinguish the measured build from
release qualification and older records.

## Latest Accepted Record (`capsid-perf-lane-v3`)

| Field | Recorded value |
| --- | --- |
| Measured source | `c7a80b9bdafe36d415a440be7ed3384f7830d4f8` (`v0.3.0-beta.5`) |
| Measured tree | `876e29c2792899b654310fea72aac979bd8bb48f` |
| Session | `local-wsl-beta5-v3-20261121T064717Z-c7a80b9` |
| Contract / matrix | `capsid-perf-lane-v3` (pure-JS seven-stack across three reference tiers + `dynamic-api`) |
| Completed measurement date | 2026-11-21 UTC |
| Machine/lane | i5-12400F; 12 logical CPUs; measured set 0–3; WSL2 Linux |
| Runtime posture | Clean local Release/LTO (`CAPSID_USE_MIMALLOC=OFF`); strict C-ABI cold workers; managed/strict serving and density |
| Reference binaries | workerd `v1.20261121.1`, Node.js `v26.13.1`, Bun `1.4.5`, Deno `2.9.8` |
| Record status | All three lanes and whole-session replay passed (`4546` evidence files) |
| Distributed packages | Not measured; their release checks are separate |

The earlier `v0.3.0-beta.4` archive label still denotes untagged development
snapshot `82eaab9`. It remains a separate point; it is not a measurement of
either release tag's commit.

## Serving and Memory (Three-Tier Pure-JS Matrix)

These are medians of five 15-second measured rounds, after five seconds of
warm-up, using 64 keep-alive connections and 64 in-flight requests. Each round
verifies exact frozen bytes (`exact-bytes-v2`, zero errors/timeouts/mismatches).
`dynamic-api` executes per-request `new URL` query parsing, header reads, an
`async` microtask boundary, dynamic 8-item JSON order construction, FNV-1a
checksum calculation (`x-bench-checksum`), and `JSON.stringify`.

| Stack | Reference tier / isolation | JSON 1 KiB | JSON 16 KiB | Stream 1 KiB | Dynamic API | Trickle PSS MiB | Loaded PSS MiB | 1K QPS/MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Capsid | T1 · Strict Linux OS sandbox (NS+Landlock+Seccomp) | 21746.6 | 14194.6 | 10649.7 | 9808.1 | 15 | 18.5 | 1175.4918918918918 |
| workerd | T1 · V8 Isolate | 25500.5 | 19805 | 18957.8 | 18738.9 | 56.4 | 547.9 | 46.54225223580946 |
| Deno | T1 · Permission sandbox (`--allow-net=127.0.0.1`) | 139834.9 | 43977.2 | 84404.3 | 54278.4 | 40.8 | 62.9 | 2223.1303656597775 |
| Node `--jitless` | T2 · Single process (no sandbox, V8 no-JIT) | 11700 | 11855.7 | 10238.2 | 8440.3 | 56.3 | 87.7 | 133.40935005701255 |
| Bun `--jitless` | T2 · Single process (no sandbox, `BUN_JSC_useJIT=0`) | 136225.2 | 43348.8 | 121854.8 | 42833 | 33.2 | 37 | 3681.7621621621624 |
| Node.js | T3 · Single process (no sandbox, V8 JIT) | 108021.8 | 41565.9 | 88282.2 | 49259.5 | 60.5 | 135.8 | 795.4477172312223 |
| Bun | T3 · Single process (no sandbox, JSC JIT) | 134435.4 | 41518.9 | 127294.2 | 76649.9 | 35.5 | 46.2 | 2909.8571428571427 |

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
| Capsid · bytecode | 6.94 ms / 7.2 MiB | 8.17 ms / 8.5 MiB | 18.64 ms / 16.0 MiB | 0.078–0.082 |
| Capsid · source | 7.51 ms / 7.4 MiB | 12.13 ms / 8.7 MiB | 69.31 ms / 19.5 MiB | 0.078–0.082 |
| workerd | 19.26 ms / 58.4 MiB | 21.01 ms / 59.9 MiB | 40.13 ms / 73.0 MiB | 0.211–0.216 |
| Deno | 35.22 ms / 38.1 MiB | 41.09 ms / 39.0 MiB | 112.88 ms / 45.8 MiB | 0.125–0.144 |
| Bun `--jitless` | 14.23 ms / 33.3 MiB | 19.60 ms / 33.6 MiB | 66.59 ms / 43.1 MiB | 0.112–0.113 |
| Bun | 14.14 ms / 34.2 MiB | 19.33 ms / 35.8 MiB | 66.11 ms / 44.8 MiB | 0.114–0.115 |
| Node.js | 48.91 ms / 60.7 MiB | 53.81 ms / 61.2 MiB | 75.85 ms / 76.4 MiB | 0.203–0.214 |
| Node `--jitless` | 75.31 ms / 90.0 MiB | 79.41 ms / 90.5 MiB | 100.74 ms / 104.9 MiB | 0.181–0.196 |

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
| Capsid workers | Warm | 729 | 503 | 128 |
| workerd cells | Cold | 947 | 809 | 253 |
| workerd cells | Warm | 571 | 525 | 264 |

These sampled windows do not prove long-duration memory retention or universal
maximum density. Heap diagnostics, service-tree PSS, individual worker cost and
pool counts are different metrics and must not be substituted for each other.

## Evidence and Replay

The [session review](data/performance-review-v030-beta5.json) binds the three successful
lanes, four quiet windows, twelve density boundaries, nine executables and
clean tagged Git tree ([source manifest](data/performance-source-manifest-v030-beta5.json)).
Physical Windows-host exclusivity is **not** claimed.

Three complete records are retained: beta.5 local rebuild (`4546` evidence
files), the earlier untagged beta.4 `v3` point (`4533` files;
[review](data/performance-review.json)), and the archived `v1` cross-language
point (`5123` files; [source manifest](data/performance-source-manifest.json)).
All `14202` evidence files are replayed from 9 verified packed transports.
The [transport index](../perf-site/archive-index.json) records every part digest,
file count and uncompressed length. Site preparation:

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

Beta.4 and beta.5 are separate sessions; the reference versions changed, so the
charts do not establish a paired optimization gain between them. Physical-host
equivalence and engine-only attribution are not claimed. Diagnostic ABBA
percentages cannot be added to this result.

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
