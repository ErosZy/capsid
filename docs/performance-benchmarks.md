# Performance: Current Measurements and Methodology

Reviewed 2026-11-18. The latest accepted local measurement is a **development
snapshot**, not a `0.3.0-beta` or stable v0.3.0 benchmark. The
[current status](current-status.md) and [generated data](current-status.json)
separate release identity, measured source, qualification and historical data.

## Latest Accepted Record

| Field | Recorded value |
| --- | --- |
| Measured source | `485b29be6cc8d0dcdb653ee6226956ddece02948` |
| Measured tree | `25bbf46247b68478a005cf779d884970fb98dc13` |
| Session | `local-wsl-v030-headers-ready-20261115T235007Z-7368b4ec` |
| Completed measurement date | 2026-11-16 UTC |
| Machine/lane | i5-12400F; 12 logical CPUs; measured set 0–3; WSL2 Linux |
| Runtime posture | Clean Release/LTO; strict C-ABI cold workers; managed/strict serving and density |
| Record status | All three original lanes and whole-session independent review accepted |
| Public `0.3.0-beta` | Source `af8acc1` (and earlier `87ed463`); **not measured by this session** |

The archival `release` label is `v0.3.0`, but its source tag is empty: this is
not evidence that a stable v0.3.0 release was published. Source-tree equality
also does not change a binary's embedded commit or make a later binary
performance-equivalent. This record does not qualify its measured snapshot for
release or close all historical runtime issues.

## Serving and Memory

These are medians of five 15-second measured rounds, after five seconds of
warm-up, using 64 keep-alive connections and 64 in-flight requests. Each round
requires the exact frozen response and zero errors/timeouts/mismatches.
The 1 KiB stream is the frozen b/c/d payload, not an arbitrary application.

| Stack | JSON 1 KiB req/s | JSON 16 KiB req/s | Stream 1 KiB req/s | Trickle PSS MiB | Loaded PSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Capsid | 9341.6 | 8090.2 | 6491.7 | 14.0 | 18.0 |
| workerd | 26856.8 | 20560.4 | 15985.8 | 47.6 | 676.2 |
| FastAPI | 6818.2 | 6613.7 | 3813.7 | 35.0 | 39.9 |
| Flask | 3435.3 | 2777.5 | 3898.3 | 40.8 | 43.1 |
| PHP Slim | 2274.5 | 1904.2 | 1925.2 | 26.5 | 27.0 |

Trickle is 15 seconds at 10 req/s on a fresh forest, **before** loaded traffic.
PSS includes the complete owned service forest; rounded roles need not sum to
the independently rounded total. Latency percentiles in the raw summaries are
from the last measured round, not medians across five rounds.

## Cold Start and Density

Cold start ends at the first validated response. Each cell has one discarded
run plus five measured rounds; 50 warm requests and stable worker-tree PSS are
recorded separately. For the 10 KiB Capsid fixture:

| Load path | Total to first valid response ms | Ready ms | Warm request ms | Worker-tree PSS MiB |
| --- | ---: | ---: | ---: | ---: |
| Source | 6.22 | 5.98 | 0.121 | 6.6 |
| Trusted bytecode | 5.85 | 5.63 | 0.120 | 6.6 |

The C-ABI probe itself is excluded from cold worker PSS. Other stacks use the
recorded HTTP client/transport rather than the IPC entry, so transport overhead
must not be presented as an engine-only comparison. All sizes/stacks and the
matching binary/compiler identities are in the site data and archives.

Density uses a 2048 MiB budget and original coverage/startup/request deadlines.
Every worker/cell must complete one cold or 100 warm requests; counts below are
budget-limited, not arbitrary cap-limited observations.

| Stack | Phase | 10 KiB | 100 KiB | 1 MiB |
| --- | --- | ---: | ---: | ---: |
| Capsid workers | Cold | 746 | 510 | 128 |
| Capsid workers | Warm | 738 | 509 | 128 |
| workerd cells | Cold | 1041 | 901 | 243 |
| workerd cells | Warm | 604 | 553 | 223 |

These sampled windows do not prove long-duration memory retention or universal
maximum density. Heap diagnostics, service-tree PSS, individual worker cost and
pool counts are different metrics and must not be substituted for each other.

## Evidence and Replay

The [accepted independent review](data/performance-review.json) binds the
original exits, all four quiet windows, twelve density boundaries, eleven
executables and measured source closure. The
[source manifest](data/performance-source-manifest.json) records the snapshot's
file identities. Physical Windows-host exclusivity is **not** claimed.

All 5123 sealed lane files are retained in deterministic compressed transports
under `perf-site/archives/`. The [transport index](../perf-site/archive-index.json)
records every part digest, file count and uncompressed length. Site preparation:

1. verifies part and complete transport digests;
2. restores only bounded regular portable members into a new scratch directory;
3. runs the unchanged complete lane/point validation and replays raw metrics;
4. verifies every original checksum and the exact bytes published for download.

Compression changes transport, not the accepted source, samples, counts, budgets
or verdict. It is not permission to repair a failed run. The
[site instructions](../perf-site/README.md) describe replay/downloads.

## Historical Comparisons and Limits

The historical v0.2.2 point is preserved unchanged. It is unverified under the
current full archive contract and differs in protocol/tool/dependency context.
Its 9489.7 / 7109.1 / 6523.2 QPS and 882 / 568 / 132 warm worker counts are not
`0.3.0-beta` measurements. In particular, the latest local density must not be called
“unchanged” relative to those historical counts.

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
