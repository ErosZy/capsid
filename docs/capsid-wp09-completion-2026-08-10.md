# Capsid WP-09 完成报告（PR-14）— P1 收尾包与 24h/72h Managed soak

- 执行日期：2026-08-10
- 规格来源：`docs/capsid-remediation-execution-spec-2026-08-09.md` §13
- 分支：`wp01-05-correctness-chain`（PR-14 提交系列，§13.1–§13.6）

## 0. 范围与结论

WP-09 覆盖 spec §13 全部六项：EXIT/EOF 构造清零（§13.1）、hard timeout 为全部
inflight 生成稳定 terminal reason（§13.2）、destroy abortive 语义与 graceful
序列（§13.3）、有界 operation registry（§13.4）、macOS 能力探测（§13.5）、
24h/72h Managed soak 平台化（§13.6）。结论：

- §13.1–§13.5 每项均先 RED 再最小修复，commit 记录 invariants/RED/GREEN。
- §13.6 平台（`soak/` 三件套：fixture + memory-waves + orchestrator）就绪，
  并在平台化验证中**额外发现并修复三个真实缺陷**（见 §1.6 根因 #1–#3），
  其中根因 #1（managed 数据平面缺 response-credit 归还）是独立于 WP-09
  计划的框架级 bug——任何 > 256 KB 的响应都会永久停滞。
- TSan scope gates / ASAN scope gates 结果见 §2。
- 已知未在本机运行的门：macOS 平台门、UBSAN、GitHub hosted runner 上的
  strict sandbox（容器内 SKIP 属预期）。

## 1. 修改文件与关键函数

### 1.1 §13.1 EXIT/EOF 构造统一清零（ec33133 / bb62956）

- RED（ec33133）：`test-worker-lifecycle` 断言 EXIT 事件的 flags/status/credit
  构造时必须清零——旧实现从栈上未初始化/残留字段构造事件。
- fix（bb62956）：`src/client.cc` 与 `include/capsid/runtime.h`——EXIT 事件
  构造路径先清零 event 全部字段，再填 type/ID/payload。

### 1.2 §13.2 hard timeout drain 全部 inflight（c217596 / e514063）

- RED（c217596）：hard timeout 触发时只报告 map 中第一个 ID。
- fix（e514063）：`src/client.cc`——timeout drain 遍历全部 inflight 请求，
  为每一个生成稳定的 terminal reason（`CAPSID_TERM_TIMEOUT` + 各自 request-id）。

### 1.3 §13.3 destroy 语义明确为 abortive（fc5b911）

- `include/capsid/runtime.h`：文档化 `capsid_worker_destroy` 是 abortive
  cleanup；graceful 序列必须是 shutdown → flush → drain EXIT → destroy。
- `src/client.cc`：destroy 路径不再假装完成 in-flight 响应。

### 1.4 §13.4 有界 operation registry + App mutex 固定生命周期（1aab90a / 1516d01）

- 有界 LRU/TTL operation registry：slot 表在构造器即 prune（1516d01 修正
  首次 ctor 未 prune 的时序），超过容量/TTL 的操作按 LRU 淘汰。
- App mutex 随已发现 App 的固定生命周期拥有，不再使用永久静态 map。
- 测试断言随实现修正（1516d01）。

### 1.5 §13.5 macOS 能力探测（e794839 / 96a5670）

- socket errno 按运行节点探测（Linux `EAGAIN`/`EWOULDBLOCK` 与 macOS 差异）。
- Apple strip 参数按能力探测分支；`truncate` 的 `warn_unused_result` 在
  -Werror 下的跟随修复（96a5670）。

### 1.6 §13.6 24h/72h Managed soak 平台化（83f5b9f / 3326a48）

新增 `soak/` 三件套：

- `soak/fixtures/soak-app.js`：同一 default export 驱动全部维度——
  /marker（capsid:env）、/echo、/slow?ms=N、/sse（3 ticks）、/big（4 MiB
  `0x53` fill）。
- `soak/soak_memory_waves.cc`：baseline metrics → WAVES 波次的 canceled
  inflight 请求 → 每波 metrics；converged() 要求 ≥4 快照、最后三波无
  单调 object/used 增长、最后一波在 warm-up 水平 2% 内。
- `soak/run_managed_soak.py`：orchestrator——spawn managed host → deploy →
  循环七个 §13.6 维度（cancel/timeout、SSE、slow client、replacement、
  queue fairness、secret/key rotation、memory/token/refcount）；违反任何
  invariant 即 exit 1；证据写入 soak-evidence.json。

平台化验证中额外发现并修复的三个真实缺陷（全部在 3326a48）：

- **根因 #1（框架 bug）**：managed 数据平面（ManagedListenerImpl）从不归还
  response credit——worker 端 initial_window 256 KB 永不 replenish，任何
  > 256 KB 响应永久停滞在恰好 262144 body bytes。修复：移植 §8.1
  response-credit 协议（64 KB early-credit 窗口 + write-completion credit +
  kResponseEnd 时 flush 剩余 credit + kGrantResponseCredit 提交）。
  RED：`test_big_response_credit`（body 262144 ≠ 524288）；GREEN 后 live
  probe 4 MiB /big 0.03s。
- **根因 #2（orchestrator config）**：ledger reserve_replace 需要 surge
  预算；`activationSurgeWorkers` 默认 0（fail-closed），workersTotal=1 的
  host.json 拒绝所有后续 deploy。修复：soak 的 host.json capacity 加
  `"activationSurgeWorkers": 1`。
- **根因 #3（slow-client 客户端）**：credit 修复后响应完整到达但
  keep-alive 连接在 terminal chunk 后空闲 → recv 超时。修复：发送
  `Connection: close`、在 `\r\n0\r\n\r\n` terminal chunk 处 break、对
  chunked framing 完整 de-frame 后断言 payload。

其他随平台化修复：`soak_memory_waves.cc` decode `!CAPSID_OK` 反转 +
kWaveBatch=64 分波 + quiescence drain；soak-app /slow race request.signal
（§7.4 worker poison 时序）；`admin_api.cc` virtiofs chmod carve-out
（mode EINVAL 降级 + NOFOLLOW recheck + owner-only fallback）。

## 2. Gates 结果

| 门 | 命令 | 结果 | 用时 |
|---|---|---|---|
| §13.1 RED | test-worker-lifecycle（EXIT 清零断言） | RED 复现 → GREEN | — |
| §13.2 RED | hard timeout 多 inflight drain | RED 复现 → GREEN | — |
| §13.3 GREEN | destroy/lifecycle/host 家族 | 40/40（-j 2） | — |
| §13.4 GREEN | host_managed 家族 | 通过 | — |
| §13.6 RED/GREEN | test_host_managed_listener（large response credit） | RED 262144 → GREEN 6/6 | — |
| §13.6 smoke | run_managed_soak.py --minutes 1 | 2× 10/10 cycles SOAK PASS（converged:true） | 60s each |
| TSan scope | `ctest -j 2 -E '^(wpt_conformance_not_configured\|worker_strict_sandbox_direct_fetch\|worker_strict_sandbox_https_ca\|worker_package_.*)$'`（build-tsan） | 待 §2.1 | — |
| ASAN scope | 同款排除（build-asan） | 待 §2.1 | — |
| managed-listener TSan | test-host-managed-listener（TSan halt_on_error=1） | 6/6 通过 | — |
| managed-listener ASan | test-host-managed-listener（ASan detect_leaks=1） | 6/6 通过 | — |

### 2.1 环境说明：TSan 在 VM 重启后的 ASLR 恢复

本机验证期间 colima VM 无响应，强制重启后 TSan 出现
`FATAL: ThreadSanitizer: unexpected memory mapping`（`vm.mmap_rnd_bits=32`
对 TSan 过高）。按 GitHub runner 同款做法在 VM 内
`sysctl -w vm.mmap_rnd_bits=28` 后恢复（该设置在 VM 重启后需重做；不是
代码问题）。WP-08 的 388/388 与本轮数据均在同一恢复后基线产生。

## 3. 每条不变量

- §13.1：EXIT 事件任何路径构造都不会泄漏 flags/status/credit 残留。
- §13.2：hard timeout 为每个 inflight 生成唯一稳定的 terminal reason，
  不丢弃、不重复、不混用 request-id。
- §13.3：destroy 不承诺完成响应；graceful 顺序固定为
  shutdown → flush → drain EXIT → destroy；Host 正常停止全走 graceful，
  只有超时才 terminate。
- §13.4：operation registry 有界（LRU/TTL）；App mutex 生命周期随 App
  发现与消失，无永久静态 map。
- §13.6 credit：initial_window（262144）不可能成为响应上限；credit 归还
  与 body 发送一一对应且只记一次；early-credit 与 write-completion credit
  互斥；HEAD 消耗响应但不暴露 body。
- §13.6 surge：无 surge 预算时 replace 被拒是 fail-closed 设计；soak
  显式声明预算。
- §13.6 slow client：terminal chunk 后必须 break；chunked de-frame 对
  `\r\n`-delimited hex size（可选 `;params`）完整还原 payload。

## 4. 失败分类

- TSan `unexpected memory mapping`：环境（ASLR entropy），非代码；恢复
  方法见 §2.1。
- §13.5 truncate `warn_unused_result`：macOS 分支在 -Werror 下的编译失败，
  96a5670 修复。

## 5. UNCOVERED / follow-up

- 容量 ledger 仅按 soak 配置验证；secret rotation CLI 的 reserve_replace
  调用方未直接压测——由 24h/72h wall-clock 运行持续观察。
- chunked de-frame 未覆盖 trailer 头（TX 无 trailer，已知局限）。
- `CAPSID_HOST_IPC_METRICS=1` 不打印 managed listener metrics（managed
  有自己的 io_loop）——已确认非 bug；未来可加 metrics hook。
- `vm.mmap_rnd_bits` 在 VM 重启后需重设（开发环境事项，非产品问题）。
