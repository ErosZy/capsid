# v0.3.0 当前验收与兼容边界

更新时间：2026-11-12T03:26:07.613009+00:00。**v0.3.0 尚未完成全平台、全量资源和正式性能验收。** 当前冻结候选 `467d77d0c671f02abb4e39ae63bb6317df71bcec` 含 5131 份源文件、SDK78；除独立更新的本文外，5130 份文件与工作区逐字节一致。主工作区 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`。

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

Darwin 的两组实际配额驱动现已在完整父矩阵中通过，原四个请求级与一个 Worker 级观测保持完整；旧失败报告仍保留。

原生观测进一步确认，失败 spawn 会短暂出现在父 PID 清单中，此时既没有 BSD 身份，`waitid(WNOHANG|WNOWAIT)` 也可能返回成功但 `si_pid=0`；真实僵尸则给出匹配的 `si_pid`。当前主候选观察器仍将前者计为子进程，完整父矩阵因此失败。私有修正以不回收方式确认身份；对于模糊 PID，有界复查完整清单并要求其消失。macOS 已实际通过 10 次缺失 executable spawn、真实存活/僵尸/回收正向控制，并拒绝恒零和恒一观察器；实际完整 C ABI fixture 仍待验证，未合入主候选。

macOS 密度控制的原 30 秒超时已由真实栈定位：卡在 `HTTPServer.server_bind()` 的 `socket.getfqdn()`，尚未发出请求。私有夹具直接绑定数字回环地址，保留全部 HTTP 请求、发送屏障、内容校验和失败断言，并增加反向 DNS 故障控制。Windows/Linux/macOS 各 **21** 项均在原 30 秒预算内实际通过，Darwin 为 5.45 秒。生产密度控制器和调度均未修改，夹具修正仍待与其余修正一并冻结。

`ws_stream.cc` 将每块数据的输入游标保持在局部变量中，逐字节解码语义和信用窗口保持原样。未修改 fuzzer oracle。相同不可变语料、ABBA 顺序的诊断比较约减少 13% 解码时间；该结果不作为产品性能点。当前源码原单测通过 ASan/UBSan。

密度父候选 `f3a17a9` 的[托管 fuzz](https://github.com/ErosZy/capsid/actions/runs/43139534152)已通过全部 **9** 个原门禁：7 个随机目标各完成 10000 次，另 2 个语料生成校验通过，无跳过。归档核对了 `max_len=65536`、单输入 2 秒、RSS 512 MiB、CTest 60 秒，以及全部目标的 `fuzzer,address,undefined` 实际编译/链接参数。[原始归档](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-original.zip)、[完整日志](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-job.log)和[审计](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-terminal.json)可复核。这不替代完整运行时和依赖库的 sanitizer 验收。

## 全量与 workerd 参考

| 平台及实际源码 | 实际结果 | 严格参考 |
| --- | --- | --- |
| Linux production/LTO，较早父候选 `2979e33` | 完整 ALL；未过滤 1107 项执行，1102 通过、5 个既有 WPT 失败、0 跳过 | 171 项必需参考通过；严格兼容性接受；`ordinarySuitePassed=false` |
| Linux production/LTO，密度父候选 `f3a17a9` | 完整 ALL；未过滤 1107 项：1102 通过、5 个既有 WPT 失败、0 跳过 | 严格参考顺序执行中，尚未宣称通过 |
| Windows，较早候选 `7d393a0` | 未过滤 845 项：832 通过、5 个既有 WPT 失败、8 个声明的平台跳过 | 134 项必需平台参考通过；严格平台审计接受 |
| macOS，父候选 `2979e33` | 完整 ALL；定向 100 项执行，82 通过、18 失败、0 跳过 | 原 26 项失败中新增关闭 9 项，剩 17 项，另有新观察器用例失败；并非完整矩阵接受 |
| macOS，密度父候选 `f3a17a9` | 完整 ALL；757 项普通矩阵：730 通过、16 失败、11 个声明的平台跳过 | 两组真实配额用例新增通过；16 项仍未接受 |
| macOS，最新候选 `467d77d` | 当前定向六单元组件与三种故障控制通过；原预算 fuzz 9/9 通过；完整普通矩阵已另行启动 | 完整三平台组合未验收 |

Linux 的[完整参考接受](evidence/v0.3.0/2026-11-12-density-platform-controls/linux-parent-compatibility.json)、Windows 的[完整平台审计](evidence/v0.3.0/2026-11-12-density-platform-controls/windows-prior-platform-audit.json)保留原五份标准文件失败及对应子项超时。父快照的完整结果没有重标为最新候选全量。

macOS 父候选的 16 项失败仍完整保留，包括 spawn 观察器、密度夹具超时、native abort/large gzip，以及 WebSocket 关闭/共享额度回收和 ACCEPT/ACK 压力时序。[完整 757 项审计](evidence/v0.3.0/2026-11-12-native-terminal-review/macos-parent-full-terminal.json)逐项记录失败和 11 个声明的平台跳过；未增加平台豁免。

## 原生源连接终态及修正验证

pinned workerd 在 Linux 与 Darwin 各执行原 11 组输入，全部包与 payload 哈希一致；10 组已有 gzip/abort 状态模型完全一致。诊断用 raw 1 MiB 的实际读块长度不同，逐块数据及 backing 哈希分别独立验证，保留原始几何而未归一化。Darwin 的两组 raw 8 MiB abort 在参考进程停止前已实际关闭，观察为写端 `EPIPE`；其字节状态与原模型一致。这是平台源连接观测的支持缺口，不能据此放宽其他平台或 gzip 的关闭契约。

私有 abort 方案仅接纳已实测的 Darwin raw 8 MiB 两种 view 的 `EPIPE`，仍要求源 socket 在 helper 结束前实际关闭，保留完整字节、退出和时间预算检查。13 项契约通过，原 407 个故障变体和新增 62 个平台/生命周期变体均被拒绝；Windows/Linux 的 16 组真实 C ABI 用例通过。实际更新后的 Darwin 生产者仍待原生构建结果，尚未合入主候选。大 gzip 的 EOF 断言保持严格：参考端均在停止前 EOF，而产品在 8 MiB 或 96 MiB 完整读取后仍可能没有结束源连接，另行验证库层关闭方案。

[本轮证据索引](evidence/v0.3.0/2026-11-12-native-terminal-review/index.json)、[22 组原生输入对照](evidence/v0.3.0/2026-11-12-native-terminal-review/native-darwin-linux-comparison.json)和[Darwin 夹具实际控制](evidence/v0.3.0/2026-11-12-native-terminal-review/fixture-darwin-terminal.json)可复核。私有诊断准备阶段的依赖遗漏、Host 关闭导致 helper 缺失和观察器编译错误均归档为 **0 项实际测试**；修正配置后使用新的临时验证分支，未将准备失败视作产品结果。

当前候选还在执行 [5 个原完整 CI 任务](https://github.com/ErosZy/capsid/actions/runs/43142872561)：Release/LTO/WPT/严格参考/包与委派检查、TSan、UBSan、Windows Release、ASan。精确检出 `991adfc`，与 `467d77d` 的源码树相同；原选择器、预算、instrumentation 和 RSS 分项验收边界保留。当前 macOS 完整普通矩阵与新的私有原生组件探测分别记录，均待终结。

## 仍需完成

当前候选三平台完整 ALL/矩阵与组合验收仍开放。密度父候选 `f3a17a9` 的 Linux production/LTO 完整 ALL 已通过，实际注册 1107 项，未过滤全量已结束，1102 通过、5 个既有 WPT 失败、0 跳过，现顺序进行严格参考；其 macOS 完整 ALL 与普通矩阵已结束，730 通过、16 失败、11 个声明的平台跳过。最新回收候选的三平台原生定向复测与 fuzz 均已通过并单独记录，未覆盖或中止父快照的完整证据。完整 ASan/TSan/UBSan、RSS 与资源入口仍须在最终源码上重验，较早快照的失败未删除。ABI/旧接口、分配故障、长时间组合运行也需与最终源码对应。R1/R2/R4 仅完成各自定向要求，R3/R5/R6/R7/R8 仍在推进，R9 未完成。

正式 QPS/cold/density 仍需同一安静会话、候选/v0.2.2/pinned workerd 对照和完整原始证据。密度继续要求全部后代 PSS、2048 MiB、两次相等正读数及退出证据。性能页历史数据保持 unverified，1280/960/820/600 四视口已有真实截图与布局检查，正式数据载入后再验展示，用户无需连接浏览器。
