# v0.3.0 当前验收与兼容边界

更新时间：2026-11-12T02:36:12.041929+00:00。**v0.3.0 尚未完成全平台、全量资源和正式性能验收。** 当前冻结候选 `467d77d0c671f02abb4e39ae63bb6317df71bcec` 含 5131 份源文件、SDK78；除独立更新的本文外，5130 份文件与工作区逐字节一致。主工作区 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`。

[密度及上一轮证据索引](evidence/v0.3.0/2026-11-12-density-platform-controls/index.json)、[候选身份](evidence/v0.3.0/2026-11-12-density-platform-controls/candidate.json)与[验收检查点](evidence/v0.3.0/2026-11-12-density-platform-controls/checkpoint.json)记录实际源码和验证范围。全部 R1–R9 及历史仍保存在工作区 `current-goal-requirements.json`；要求和状态未裁剪。此前的[握手短写修复证据](evidence/v0.3.0/2026-11-12-queued-http-handshake/index.json)继续保留。

## 出站连接额度与异步回调生命周期

当前 `467d77d` 在父候选 `f3a17a9` 上仅修改 `websocket_session.cc` 与其原单测。可控回归实测旧实现关闭连接后仍保留出站 channel/active 额度，直到持有 session 的异步回调对象销毁。修复在传输关闭完成时释放出站额度；入站流仍保留物理 socket 所有者，未完成写入仍保留真实缓冲和字节额度直到写完成。

Windows/MSVC 与 Linux 当前源码的原单测均在原 15 秒预算内通过。两平台各三种故障实现被明确拒绝：不归还出站额度、过早归还活跃额度、误释放入站 stream anchor。真实存活连接正向控制、延迟读/写回调、入站物理所有者正向控制及原 Close 字节和 ACK 时序断言全部保留。Linux 六个组件翻译单元全部使用 ASan/UBSan 的独立构建也通过，不代表完整运行时和依赖库 sanitizer 验收。

[本轮源码与证据索引](evidence/v0.3.0/2026-11-12-session-anchor/index.json)、[源码差异](evidence/v0.3.0/2026-11-12-session-anchor/candidate.diff)、[Windows](evidence/v0.3.0/2026-11-12-session-anchor/windows-current-unit.json)、[Linux](evidence/v0.3.0/2026-11-12-session-anchor/linux-current-unit.json)与[六单元 instrumentation](evidence/v0.3.0/2026-11-12-session-anchor/linux-all-six-instrumented.json)可复核。最新候选的 [macOS 原生定向组件与原预算 fuzz](https://github.com/ErosZy/capsid/actions/runs/43141798897)均已通过。归档核对了精确源码、六个原生目标文件各自的 ASan/UBSan 引用、原 15 秒用例预算及三种错误实现的实际拒绝结果；[原生审计](evidence/v0.3.0/2026-11-12-session-anchor/macos-current-component-terminal.json)和[实际观测](evidence/v0.3.0/2026-11-12-session-anchor/macos-current-component-observation.json)均保留。当前源码的 9 个 fuzz 门禁无跳过，7 个随机目标各完成 10000 次，原最大输入、单输入/CTest 超时和 RSS 预算全部核对保留；[fuzz 原始归档](evidence/v0.3.0/2026-11-12-session-anchor/current-fuzz-original.zip)及[审计](evidence/v0.3.0/2026-11-12-session-anchor/current-fuzz-terminal.json)可复核。当前候选的[macOS 完整普通矩阵](https://github.com/ErosZy/capsid/actions/runs/43142326406)已另行启动，保留原 ALL、普通排除项和逐项预算，检出同一精确源码。该组件验证不替代剩余 Darwin 完整 Worker/Host 关闭与共享额度回收用例。

## 密度预热协议与实际覆盖

用户已明确允许密度预热临时按池大小扩大并发。生产仍使用 least-loaded、最低槽优先，一个 App；QPS 测试仍固定 64 并发。密度基础并发为 64，预热上限为 `max(64, 64×池大小)`。单发送者先写出整波真实 GET，再读取并校验每个 HTTP 状态和响应体；连接准备最多使用 64 个线程。

冷覆盖请求上限为 `max(64, 池大小×目标次数×8)`，热覆盖上限为 `max(64, 池大小×目标次数×32)`，覆盖期限保持 300 秒。热请求系数从旧值 8 调整为 32；该变更属于已批准的密度预热方案，已进入 SPEC、生产者元数据和独立证据校验。不能把旧协议归档重新标为新协议通过。最后一波完成时若已经到达期限，即使计数足够也必须失败。

| 65 READY Worker 实际探测 | 冷阶段 | 热阶段 |
| --- | --- | --- |
| 校验正确的响应 | 520 | 112320 |
| 实际触达 Worker | 65/65 | 65/65 |
| 每 Worker 最少请求 | 1 | 101（目标 ≥100） |
| 实际最大单波请求 | 520 | 4160 |
| 两次稳定 PSS，KiB | 190183 / 190183 | 214253 / 214253 |
| 清理后遗留的所属 PID | 0 | 0 |

[完整记录](evidence/v0.3.0/2026-11-12-density-platform-controls/density65-cold-hot.json)来自父候选 `2979e33` 的真实 production/LTO Host/Worker。该次控制器与当前控制器之间仅增加了更严格的末波超时拒绝；源哈希及组合验证边界在候选记录中明确保留。r1–r6 的不足覆盖、日志故障等原记录也已归档。这是固定 65 Worker 的正向探测；尚未证明正式密度边界、三种包大小和 workerd 对照，正式性能点仍为 **0**。

使用当前最终控制器，pinned workerd 的 65 个 cell 也已完成冷阶段每 cell 1 次、热阶段每 cell 100 次覆盖。实际响应数分别为 65/6500，两次稳定 PSS 分别为 172817/365220 KiB，清理后所属 PID 为 0。[参考端完整记录](evidence/v0.3.0/2026-11-12-session-anchor/pinned-workerd65.json)保留控制器与 workerd 二进制哈希；仍是固定 65 cell 探测，未标为正式密度边界或正式性能点。

## 密度父候选修复与测试

Windows/Linux 各 20 项密度控制测试通过，包括真实整波发送屏障、响应状态/内容错误、连接失败关闭、65 Worker 波次和末波超时反例。两平台各 21 项性能证据契约通过；各 681 个配额故障变体被拒绝。真实 C ABI 配额两组在两平台合计执行 62 个用户请求、10 次自然 Worker 退出。Windows 密度生产者保留原 4 个 Linux 专用平台跳过，未算作实际通过。

Darwin 配额模型现按真实 POSIX READY 特征校验。此前真实 Darwin 的 4 个请求级和 1 个 Worker 级观测通过新严格模型重放；原失败报告的 `complete=false` 原样保留，新的原生完整驱动仍待结果。

Darwin 缺失可执行文件的原生诊断观察到：10 次 `posix_spawn` 均返回 ENOENT、PID 输出为 -1，12 次描述符清单一致；`proc_listpids` 曾短暂列出无 BSD 信息的 PID。测试观察器现使用不回收子进程的 `waitid(...WNOWAIT)` 检查归属，并增加真实存活子进程与未回收僵尸的正向控制。Linux 实际回收通过，恒定描述符/子进程观察器均被拒绝；当前 Darwin 观察器仍待原生完整矩阵验证。[原生诊断](evidence/v0.3.0/2026-11-12-density-platform-controls/darwin-spawn-diagnosis.json)与原失败都保留。

`ws_stream.cc` 将每块数据的输入游标保持在局部变量中，逐字节解码语义和信用窗口保持原样。未修改 fuzzer oracle。相同不可变语料、ABBA 顺序的诊断比较约减少 13% 解码时间；该结果不作为产品性能点。当前源码原单测通过 ASan/UBSan。

密度父候选 `f3a17a9` 的[托管 fuzz](https://github.com/ErosZy/capsid/actions/runs/43139534152)已通过全部 **9** 个原门禁：7 个随机目标各完成 10000 次，另 2 个语料生成校验通过，无跳过。归档核对了 `max_len=65536`、单输入 2 秒、RSS 512 MiB、CTest 60 秒，以及全部目标的 `fuzzer,address,undefined` 实际编译/链接参数。[原始归档](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-original.zip)、[完整日志](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-job.log)和[审计](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-terminal.json)可复核。这不替代完整运行时和依赖库的 sanitizer 验收。

## 全量与 workerd 参考

| 平台及实际源码 | 实际结果 | 严格参考 |
| --- | --- | --- |
| Linux production/LTO，父候选 `2979e33` | 完整 ALL；未过滤 1107 项执行，1102 通过、5 个既有 WPT 失败、0 跳过 | 171 项必需参考通过；严格兼容性接受；`ordinarySuitePassed=false` |
| Windows，较早候选 `7d393a0` | 未过滤 845 项：832 通过、5 个既有 WPT 失败、8 个声明的平台跳过 | 134 项必需平台参考通过；严格平台审计接受 |
| macOS，父候选 `2979e33` | 完整 ALL；定向 100 项执行，82 通过、18 失败、0 跳过 | 原 26 项失败中新增关闭 9 项，剩 17 项，另有新观察器用例失败；并非完整矩阵接受 |
| macOS，密度父候选 `f3a17a9` | 完整 ALL 已通过，完整普通矩阵正在实际执行 | 待实际终结，未计作通过 |
| macOS，最新候选 `467d77d` | 当前定向六单元组件与三种故障控制通过；原预算 fuzz 9/9 通过；完整普通矩阵已另行启动 | 完整三平台组合未验收 |

Linux 的[完整参考接受](evidence/v0.3.0/2026-11-12-density-platform-controls/linux-parent-compatibility.json)、Windows 的[完整平台审计](evidence/v0.3.0/2026-11-12-density-platform-controls/windows-prior-platform-audit.json)保留原五份标准文件失败及对应子项超时。父快照的完整结果没有重标为最新候选全量。

macOS 父候选新增关闭了性能证据契约、Host 验收矩阵、6 个 raw/gzip pending shard 和 workerd mutation controls。剩余问题包含大 gzip 源连接结束观测、abort-prefix 源重置、WebSocket 关闭/共享额度回收和 ACCEPT/ACK 压力时序。生产回收和关闭断言仍保持严格，未按平台忽略失败。原 16 项密度控制测试在单独 macOS 原预算诊断中通过，原完整运行中的 30 秒超时记录仍保留；当前 20 项继续接受原生复测。

## 仍需完成

当前候选三平台完整 ALL/矩阵与组合验收仍开放。密度父候选 `f3a17a9` 的 Linux production/LTO 完整 ALL 已通过，实际注册 1107 项，未过滤全量正在执行，随后顺序进行严格参考；其 macOS 完整 ALL 已通过，普通矩阵仍在执行。最新回收候选的三平台原生定向复测与 fuzz 均已通过并单独记录，未覆盖或中止父快照的完整证据。完整 ASan/TSan/UBSan、RSS 与资源入口仍须在最终源码上重验，较早快照的失败未删除。ABI/旧接口、分配故障、长时间组合运行也需与最终源码对应。R1/R2/R4 仅完成各自定向要求，R3/R5/R6/R7/R8 仍在推进，R9 未完成。

正式 QPS/cold/density 仍需同一安静会话、候选/v0.2.2/pinned workerd 对照和完整原始证据。密度继续要求全部后代 PSS、2048 MiB、两次相等正读数及退出证据。性能页历史数据保持 unverified，1280/960/820/600 四视口已有真实截图与布局检查，正式数据载入后再验展示，用户无需连接浏览器。
