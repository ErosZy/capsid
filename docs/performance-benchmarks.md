# Performance: Evidence Rules and Current State

<!-- capsid-current-qualification-20261110 -->
本页后续历轮内容保留原记录身份。当前兼容边界、冻结输入、原始失败和封板开放项见[当前验收与边界](v0.3.0-current-qualification.md)。当前源码版本为 0.3.0 候选，v0.3.0 尚未封板或发布。

部分历史产物已按用户授权清理；[证据索引](evidence/v0.3.0/2026-11-10/index.json)及[可用性审计](evidence/v0.3.0/2026-11-10/historical-evidence-availability.json)说明时点与范围。旧章节的“当前”“仍保留”不表示本轮重新验证了已删除文件。
<!-- /capsid-current-qualification-20261110 -->


<!-- capsid-review-phase-93 -->
<a id="capsid-review-phase-93"></a>

## 第 93 轮（2026-10-20）：五栈 QPS 控制器与归档已接通

当前 QPS 公开入口已替换旧 shell，默认五轮/managed/strict，覆盖五栈三 workload、独占端口、PHP 双根与 Go 客户端拥有权、先 trickle 后另起 loaded、精确就绪/响应、递归稳定 PSS、清理和封存自校验。正式模式要求干净构建/会话元数据；短 `--observation` 无发布 envelope。下方旧的“QPS 生产器仍未接通”属于历史状态。

最终工具选择集 Windows 普通/ASan **14/14、14/14**（Python 101：70 通过/31 平台跳过），Linux **18/18**（120/120）；没有本轮完整产品重编或 sanitizer 资格。真实五栈 15 单元短观测、2,191 份封存文件与测量中 SIGINT/SIGTERM **10/10** 通过；共享 cold/density 实际小观测也通过。旧验证器 10 个失败及首次 Linux PSS 不稳定失败均保留，最终 Linux 单独复核/顺序回归未放宽协议。

完整 0.3.0 强兼容、T15/T16/T17 仍未整体关闭。干净 Release/LTO/ALL/包与消费者、完整 gate/矩阵、同一安静会话正式三车道/2 GiB 密度/四视口、原生 body/压力/soak、Windows 默认 Debug 编码流根因和其他平台/依赖 sanitizer 仍待完成。历史 **1762/1733/6/23** 未重跑；版本仍 0.2.2、4508 输入，运行时/Host 153 输入未变，无 Root 提交/推送/发布。

实现、真实与合成测试边界、保留的失败、证据入口和接棒任务见[本轮完整交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-93)。最终审计：`upgrade47-final-audit.json`；Linux 最终结果用 `upgrade47q-linux.json`，不能将失败的 `upgrade47m-linux.json` 重标为成功。

<!-- capsid-review-phase-92 -->
<a id="capsid-review-phase-92"></a>

## 第 92 轮状态（2026-10-20）：QPS 负载正确性/原始证据修复，完整生产器继续

修复真实 Go 负载工具的流正文抽查漏检、错误/超时分类与错误成功 verdict、失败退出码和无效参数；新增完整正文身份及 attempted/checked/completed 计数，正式校验要求 `exact-bytes-v2` 与 30000 ms 截止时间。旧实际工具三组红例和旧验证器 28 个失败断言均保留。最终 CTest 为 Windows **13/13**、Windows ASan **13/13**、Linux **16/16**；新增规则 8 组、Linux 实际 HTTP 8 组、合同增补 2 组。

一次真实工具的 64/64、5+15 秒轮只面对独占正确性夹具；另跑可信字节码 cold 两次启动/102 响应/38 文件小观测。均不作为正式性能点。`src/js/include` 153 份输入未变，版本仍 0.2.2；旧 QPS shell 仍未接通完整五栈生产器。完整强兼容、干净 ALL/全量矩阵、body/stream 压力、正式三车道/2 GiB/四视口及完整依赖 sanitizer/其他平台仍待完成，历史全量结果不更新。

范围、平台边界、二进制/原始失败与下一步见 [第 92 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-92)。最终审计：`build-v030-dev/evidence/native-asan-qualification-20261018/upgrade46-final-audit.json`。没有 Root 提交、推送或发布。

<!-- capsid-review-phase-91 -->
<a id="capsid-review-phase-91"></a>
## 第 91 轮状态（2026-10-20）：编译器发布兼容修复，完整目标继续

编译器补齐不支持 `RENAME_NOREPLACE` 的 Linux 文件系统后备路径与明确系统错误诊断；新增归属跟踪，避免在创建失败后删除既有临时文件。新增 8 组真实编译器回归与 7 组 C++ 操作模型，最终 CTest 为 Windows **12/12**、Windows ASan **12/12**、Linux **14/14**；挂载盘另跑 **8/8**。Windows 的符号链接组及原有 Linux 专属组继续明确跳过。源/字节码三尺寸实跑 **12 次启动、612 次响应校验、123 份封存文件**，仅作功能观测。

首轮 C++11 编译失败、Linux 链接缺依赖、旧编译器真实红例均保留。`src/js/include` 153 份实现输入未变；版本仍 0.2.2。完整 0.3.0 强兼容、QPS、干净 ALL/全量矩阵、正式三车道/2 GiB 与四视口复核仍未完成。历史 1762 项全量结果不作更新；没有新性能点、Root 提交、推送或发布。

完整范围、平台跳过、原始证据与下一步见 [第 91 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-91)。审计入口：`build-v030-dev/evidence/native-asan-qualification-20261018/upgrade45-final-audit.json`。

<!-- capsid-review-phase-90 -->
<a id="capsid-review-phase-90"></a>
## 第 90 轮状态（2026-10-20）：cold 控制器/归档接通，完整目标继续

冷启动公开入口已统一 C-ABI 源码/字节码、Node/Deno/workerd 的进程与端口拥有权、精确响应、固定成员/稳定递归 PSS、独占夹具/原生临时编译、CPU 绑定恢复、丢弃轮与测量轮归档及真实校验器。旧验证器漏检丢弃/进程/warm 证据的 15 个实际红例已修正，原始失败与修前源码保留。正式模式要求完整协议和实际干净构建/会话元数据；观测模式不能发布。

最终工具 CTest：Windows 普通 8/8、Windows ASan 8/8、Linux 10/10（`upgrade44h-*`）。新增 Python 组 21，Linux 全通过，Windows 新增为 10 通过/11 个 Linux 专属跳过；Windows 全部脚本共 67 组、46 通过/21 跳过。Linux 共 74 Python 组及既有 10 C++ 模型通过。本轮没有产品编译或新增原生 sanitizer/泄漏验收。

最终真实 cold smoke 覆盖五路径/三尺寸、30 次启动和 1530 次响应，318 份文件核验（`upgrade44i-*`）；只有一测量轮，不能代替正式五轮。四类真实 SUT 的 SIGINT/SIGTERM 共 8 次清理通过（`upgrade44j-*`）；密度入口回归 12 探针/4 小池单元、174 文件核验（`upgrade44k-*`）。合成完整档案只在临时仓库测试，未写入 data.js。站点按新 HTTP 协议展示 warm 值，旧点仍是未验证历史观测，视觉四视口复核未完成。

**完整 Linux/Windows 0.3.0 强兼容仍未完成。** QPS 合同、编译器挂载文件系统/错误诊断、实际同会话三车道与 2 GiB 密度、更广 body/资源压力、默认 Windows Debug 栈失败、新 ALL/Release/LTO/包/全量矩阵及平台和动态验证仍需继续。4495 输入、版本 0.2.2、58 补丁，Root HEAD 和 `src/js/include` 153 份实现输入不变；没有提交/推送/发布。

完整实现、证据与后续顺序见 [第 90 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-90)；审计入口 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade44-final-audit.json`。

<!-- capsid-review-phase-89 -->
<a id="capsid-review-phase-89"></a>
## 第 89 轮状态（2026-10-20）：cold 探针已修，整车道与强兼容目标未完成

冷启动 C-ABI 工具已改用公开 worker PID，严格核对状态/请求 ID/完整正文，warm 失败、超时、进程退出/被回收/PID 替换及不可读/零/不稳定 PSS 都失败。worker 子孙树在首次响应后、warm 前固定；冷启动计时在 warm/PSS 之前结束；中断会通过 C-ABI 销毁回收 worker。旧错误成功行和中断遗留的真实红例已保存（`upgrade43d-*`），独立零 tick 解析红例也已修正并纳入最终模型。

最终实际工具 CTest：Windows 普通 6/6、Windows ASan 6/6、Linux 8/8（`upgrade43k-*`）；新增 10 组跨平台 C++ 模型与 7 组 Linux 真实进程回归。另对 10k/100k/1m 的源码/可信字节码实际做 6 次 strict smoke、306 次响应核验并封存，见 `upgrade43p-real-smoke.json`。这些不是正式 cold lane 或新的 workerd 性能比较。新编译测量/模型目标及 Linux bytecode compiler；既有 host/worker、运行时库和 `src/js/include` 153 份实现输入未变，没有新整产品/泄漏/不筛选矩阵验收。

**完整目标仍在进行。** cold shell 控制层尚需统一拥有权/端口/档案契约，QPS 也未接通；真实同会话三车道和 2 GiB 密度、站点视口、更广 body/资源压力兼容、默认 Windows Debug 栈失败、新 ALL/Release/LTO/包与原计划平台和动态验证仍未完成。4489 输入、版本 0.2.2、58 补丁、Root HEAD 不变；没有提交/推送/发布，也没有生成新性能点。旧矩阵与跳过数保持历史性质。

完整修复、证据和下一步见 [第 89 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-89)；审计入口 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade43-final-audit.json`。


This document retains historical samples. The measurement protocol and
publication rules are specified in [bench/SPEC.md](../bench/SPEC.md) and the
[archive contract](../bench/perf-evidence.md).

**2026-10-20 review: the old points below are unverified historical
observations, not qualified 0.3.0 comparisons.** The archived strict cold-start
directory has no raw samples or correctness records; neither it nor the QPS
directory has the new lane envelope or isolation record. Historical digest
lists include absolute labels, and the site point's source identity differs
from its embedded machine commit. Do not infer a single clean release/session
from these combined values. Original files and numbers are retained for review.

The collector now rejects incomplete archives, conflicting origins, altered
digests and raw/summary disagreement. Parser fixtures are separate from
publication. Site staging replays qualified points and packages their evidence;
the current checked-in point is displayed as unverified. Density now has an
owned-process controller and contract producer, with small-pool real smoke and
synthetic archive checks. Cold-start also has an owned controller and sealed producer with all-stack/all-size observation coverage. **QPS producer integration, actual fill-to-budget
measurement and new quiet-session measurements remain pending.** No 0.3.0
performance point has been created from the synthetic contract tests.

Earlier optimization checkpoints remain in git history and `bench/results/`.
All environment and release descriptions below are historical claims awaiting
complete provenance, rather than assertions made by the current validator.

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
| capsid release / commit | `v0.2.2` / `aef6020` (Release build; serving through the managed coordinator with the mandatory Linux strict sandbox — namespaces, Landlock, seccomp — one-worker pool, production stream-window defaults) |

The stacks under test (versions and artifact SHA-256 values are recorded in `bench/results/five-qps-v022-20261010/manifest.txt` and `bench/results/cold-start-v022-20261010/manifest.txt`):

| Stack | Component and version |
|---|---|
| capsid | `v0.2.2` (`aef6020`; Release), managed coordinator + strict sandbox, one-worker fixed pool, precomputed-response JS fixture |
| Cloudflare workerd | workerd 2026-10-10 (single process, minimal capnp config, same fixture logic) |
| Python + FastAPI | Python 3.13 + FastAPI 0.143.0 + Uvicorn 0.54.0 (1 worker, uvloop + httptools, `--no-access-log`; framework-optimal fixture: precomputed matrix bodies, async endpoints, async stream generators) |
| Python + Flask | Python 3.13 + Flask 3.1.3 + Gunicorn 26.2.0 (1 sync worker; sync measured faster than gthread — GIL contention outweighs the keep-alive it adds) |
| PHP + Slim | PHP 8.3.6 + Slim 4 + nginx 1.24.0 + php-fpm `pm=static max_children=1`, opcache `validate_timestamps=0` + JIT tracing, nginx `fastcgi_keep_conn on` |
| Cold-start extras | Node v24.14.0, Deno 2.9.7 |

Workload set: `matrix-json-1k`, `matrix-json-16k`, `stream` (1 KiB b/c/d thirds). Payloads are byte-aligned and the loadgen verifies exact content every round. The 12-workload matrix of the rc.07 checkpoint is retired with it; these three lanes are the maintained history lanes and back the published page.

## 3. Five-Stack Serving Matrix (2026-10-10, c64, 5 rounds)

One quiet session, all five stacks back-to-back, warmup 5 s + 5 measured rounds of 15 s per workload, verdict-gated correctness every round. Raw samples, correctness verdicts, and manifests: `bench/results/five-qps-v022-20261010/`. The interactive history page built from this point: <https://eroszy.github.io/capsid/>.

Median QPS over 5 rounds (winner per row in bold):

| workload | capsid (managed, strict) | workerd | FastAPI | Flask | PHP Slim |
|---|---:|---:|---:|---:|---:|
| json 1k | 9,490 | **26,624** | 12,948 | 3,568 | 2,095 |
| json 16k | 7,109 | **20,189** | 12,599 | 2,878 | 2,008 |
| stream 1k | 6,523 | **14,192** | 6,671 | 3,987 | 2,032 |

p50/p95 latency (ms, historical values; aggregation requires revalidation):

| workload | capsid | workerd | FastAPI | Flask | PHP Slim |
|---|---:|---:|---:|---:|---:|
| json 1k | 6.6 / 8.2 | 1.7 / 3.9 | 5.0 / 7.6 | 18.6 / 20.6 | 30.3 / 36.3 |
| json 16k | 10.7 / 16.7 | 2.4 / 5.5 | 4.9 / 9.1 | 22.2 / 24.1 | 32.7 / 38.2 |
| stream 1k | 9.5 / 11.1 | 3.6 / 9.3 | 9.3 / 12.7 | 16.1 / 17.1 | 31.5 / 32.5 |

Serving-path memory (process-tree PSS after the last round; trickle = fresh process after 15 s of 10 req/s):

| stack | serving PSS (MB) | breakdown (MB) | trickle PSS (MB) |
|---|---:|---|---:|
| capsid (managed, strict) | 16.4 | coordinator 8.5 + sandboxed worker 7.8 | 12.5 |
| Flask | 17.0 | gunicorn master 0.4 + worker 16.5 | 16.3 |
| PHP Slim | 28.1 | nginx 0.7 + nginx-worker 3.6 + fpm-master 14.0 + fpm-worker 9.8 | 27.5 |
| FastAPI | 46.1 | uvicorn master 0.4 + worker 45.7 | 42.2 |
| workerd | 581.0 | single process | 44.2 |

### Reading the matrix

- **Raw throughput: workerd leads every cell** (2.0-4.7× capsid) — but it is an untuned-V8 upper bound, not a tuned production limit.
- **workerd memory is the outlier**: ≈581 MB PSS under the 64-connection load versus a ≈44 MB trickle — V8 heap + JIT growth that is never returned. It is 12× past the second-largest stack and dominates the memory axis.
- **FastAPI is the Python leader once framework-optimal**: precomputed bodies, async endpoints, and async stream generators (plus `--no-access-log`, matching gunicorn's default) put it ahead of Flask on JSON (12.9k / 12.6k vs 3.6k / 2.9k); its streaming path still trails its own static-bytes throughput. Earlier checkpoints that showed FastAPI at ~3.2k were fixture artifacts (per-request `json.dumps`, sync-def threadpool hops, and an access log redirected onto a drvfs mount), not framework behavior.
- **capsid holds the density point with isolation on**: ≈16.4 MB PSS for the whole managed, strict-sandboxed serving path (coordinator 8.5 + worker 7.8) with mid-pack QPS — about 1/3 of FastAPI's memory and 1/35 of workerd's under the same load. The sandbox itself is cheap: JSON throughput unchanged, cold start +0.3-5.4 ms, warm 10k density −1.6%.
- **PHP Slim is the consistent laggard** at ~2.0-2.1k QPS (p95 ≈32-38 ms) even with JIT tracing and FastCGI keep-alive enabled.

### Measured density: fill a pool to 2 GB (2026-10-10)

Real capacity runs, not footprint arithmetic: grow capsid's `static-pool` (or workerd's cell count in one process) until the process-tree PSS reaches 2048 MB, binary-searching the boundary (the recorded count must verify under budget, with a jitter guard). Each cell is reported twice: **initialized** (one real request per worker/cell) and **warmed** (100 requests through each, settling before a stability-checked sample). Source bundles; probes and the merged manifest (recording `CAPSID_STRICT=1` for the capsid passes and both warm/cold phases) in `bench/results/density-v022-20261010/`.

| fixture size | capsid workers (strict) · init → warmed | workerd cells · init → warmed |
|---:|---:|---:|
| 10 KiB | **891 → 882** (−1.0%) | 1,061 → 638 (**−40%**) |
| 100 KiB | **569 → 568** (−0.2%) | 917 → 580 (**−37%**) |
| 1 MiB | **132 → 132** (±0) | 244 → 214 (−12%, re-verified under strict gates) |

Reading the cold/warm gap: initialization alone understates a JIT runtime. workerd's V8 compiles hot paths and grows its heap as soon as cells serve real traffic, and no return was observed within any sampling window of these runs — 40% of its 10k/100k capacity disappears between the first request and the hundredth (the throughput lane shows a single isolate at 44 → 581 MB under sustained 64-connection load, with workerd also serving ~2.8× capsid's request rate in that window, so loaded figures compare configurations, not equal work; these warmed counts remain an optimistic middle for workerd). capsid's interpreter has no tier-up: cold and warmed read the same, which is why its numbers move less than 1%. workerd still packs more 1 MiB cells per 2 GB (compile-structure economics: V8 compiles lazily while QuickJS compiles every function eagerly); every capsid tenant remains a separate, sandboxable process, so one tenant's growth cannot shrink a neighbour's budget. Sustained per-tenant traffic density remains future work.

## 4. Cold-Start Comparison (2026-10-10, median ms, 5 rounds)

Measurement class 4: **process start → first validated response**. The shell-measured stacks (Node/Deno/workerd) previously read ~400 ms regardless of fixture size because `total_ms` was captured after a 50-request curl warm loop and the PSS walk; the endpoint now sits at the first exact-body-verified response, matching capsid's in-process runner. Fixture is real-shaped JS source (three template rotations), at 10k/100k/1M sizes; each side loads the same function body byte-aligned, differing only in entry point. capsid uses C ABI spawn→load (source/trusted bytecode)→READY→first response; Node/Deno use process start→stdout READY→curl first request; workerd uses first TCP connect→curl first request (its isolate initializes lazily on the first request, so ready→first-response carries that cost). Each cell drops 1 warmup round and takes the median of 5 rounds. Raw data: `bench/results/cold-start-v022-20261010/` (cells + manifests; the Deno pass ran separately and is stored alongside as `cells.deno.txt`).

| Size | capsid source (strict) | capsid trusted bytecode (strict) | Node 24.14 | Deno 2.9.7 | workerd |
|---:|---:|---:|---:|---:|---:|
| 10k | 19.7 | **17.7** | 57 | 20 | 20 |
| 100k | 23.4 | **19.1** | 57 | 31 | 22 |
| 1M | 81.5 | **31.1** | 81 | 55 | 38 |

Cold PSS (MB, same samples): capsid source 0.8/1.2/4.6, bytecode 0.9/1.7/10.2 (the sandbox installs no large mappings), Node 59.6/60.2/76.3, Deno 34.9/36.4/41.4, workerd 49.8/51.7/64.4.

- **capsid trusted bytecode is flat across sizes**: 17.7 → 19.1 → 31.1 ms under the full sandbox, because deserialization replaces parsing; at 1M it is 2.6× faster than capsid source, 2.6× faster than Node, and 1.8× faster than Deno.
- **Deno and workerd have the strongest small-bundle floors** (~20 ms); Node's ~57 ms floor is process + V8 bootstrap. Source-mode parsing shows up at 1M for every source-loading engine (capsid source 76.1, Node 81, Deno 55).
- **Cold memory stays small for capsid** (≤10.2 MB even at 1M bytecode) versus 35-76 MB for the comparison engines.

## 5. Retired Checkpoints

The 2026-08-20 `v0.2.0-rc.07` checkpoint (`four-qps-rc07-clean-20260820T170500`, `four-qps-rc07-clean-profile-20260820T173200`, `cold-start-rc07-clean-20260820T173500`) is superseded by the five-stack v0.2.2 point above: it measured a two-worker serving model, a 12-workload matrix, and a cold-start `total_ms` whose shell-measured side was inflated by the warm loop. The 2026-08-18 AMD Ryzen checkpoints, the 2026-08-18 Intel i5-12400F 6C/12T conclusion-adjacent tables (`b39acee`/`build-win`), and the 2026-08-14 4C tables were retired earlier. They remain available in git history and in the raw artifacts under `bench/results/` referenced by the older revisions of this document.
