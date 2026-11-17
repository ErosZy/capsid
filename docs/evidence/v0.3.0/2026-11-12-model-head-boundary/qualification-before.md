# v0.3.0 当前验收与兼容边界

更新时间：2026-11-12T04:25:30.090520+00:00。**v0.3.0 尚未完成全平台、全量资源和正式性能验收。** 当前冻结候选 `45bb9f47d7955a0ccd1e2297a8d422aae2809eea` 含 5133 份源文件、SDK78；除独立更新的本文外，5132 份文件与工作区逐字节一致。主工作区 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`。

[密度及上一轮证据索引](evidence/v0.3.0/2026-11-12-density-platform-controls/index.json)、[候选身份](evidence/v0.3.0/2026-11-12-density-platform-controls/candidate.json)与[验收检查点](evidence/v0.3.0/2026-11-12-density-platform-controls/checkpoint.json)记录实际源码和验证范围。全部 R1–R9 及历史仍保存在工作区 `current-goal-requirements.json`；要求和状态未裁剪。此前的[握手短写修复证据](evidence/v0.3.0/2026-11-12-queued-http-handshake/index.json)继续保留。

## 最新强制回收修复

当前 `45bb9f4` 在 `ef53262` 上合入实际 OS 强制退出和回收修复，并增加 Linux 永久 `worker_forced_reap` 回归。公开 C ABI 要求 destroy 返回前子进程已回收；原实现强制结束后最多等待 250 毫秒，延迟的退出观测可能使其返回时仍未回收。修复保留原 50 毫秒自然退出窗口，强制结束后等待真实 OS 退出结果，遇到 `EINTR` 继续等待。Host 调度与回收执行器并发数不变；内部文档已明确自然退出窗口与总回收耗时的区别，不再将其写为固定 300 毫秒上限。C ABI 布局未变。

实际对照中，旧实现三组在夹具清理前未回收；新实现四组通过，真实 `waitid` 见证和退出状态均检查。Windows 同源方案通过四次真实强制结束和三项原退出状态控制。另将当前 client、独立故障用例、原退出状态用例和当前 cold controller 四个翻译单元完整置于 ASan/UBSan，核对每个目标的编译参数及实际 sanitizer 符号；四组故障控制、四项原状态模式及七项原 cold-tool 测试通过，包含实际 SIGINT、SIGTERM 清理，cold-tool 原 100 秒预算保留。该仪器化组合使用较早完整基线的 runtime/worker/provider，属于当前组件验证，未宣称最终候选全量 instrumentation 接受。

[当前源码差异](evidence/v0.3.0/2026-11-12-forced-reap-integration/candidate.diff)、[四单元 instrumentation 与实际控制](evidence/v0.3.0/2026-11-12-forced-reap-integration/linux-asan-ubsan-component.json)、[七项 cold-tool 原日志](evidence/v0.3.0/2026-11-12-forced-reap-integration/asan-actual-original-seven-cold-controls.stderr.log)和[本轮证据索引](evidence/v0.3.0/2026-11-12-forced-reap-integration/index.json)均已保留。当前冻结源码已在独立、清洁的 Linux checkout 开始 production/LTO 完整 ALL，随后顺序执行未过滤全量（预期 1108 项）和严格参考 171 项；当前仍在 ALL 构建，未计为通过。最新 client 的 Darwin 执行及最终三平台组合仍开放。

## 连接结束与原生夹具父候选

父候选 `ef53262` 在 `467d77d` 上修改 9 份既有文件并增加一份真实 Darwin primary 归档。已合入：已知长度且不复用的 H1 响应在 wire 完成时结束传输；Darwin abort 的精确 `EPIPE` 源关闭观测；无反向 DNS 依赖的数字回环 HTTP 夹具；不回收的原生子进程归属观察器；POSIX 实际 READY、C ABI marker 正向控制和失败后 GET 状态/字节恢复；macOS 独立回环地址的 CI 准备。生产调度和 QPS 并发保持原契约。

库修正在真实 Darwin、Windows、Linux Worker 中合计通过 6 组大 gzip、32 组 abort 和 8 组 TLS 用例，原 8/96 MiB 字节模型、源 EOF 与退出断言及原预算均保留。Darwin 同会话旧实现明确复现 8 MiB 全部解码后没有 EOF，替换一个库目标文件后两组大 gzip 均通过。78 个补丁在干净 vendor 上完整应用；67 个受补丁影响的文件中只有 `client-http.c` 变化。新 overlay key 为 `71736ec71e3eef46f1e1eb3d7a8a25a59a3dba2f87893b0798665c04864c70a6`，manifest 为 `1097d8e961db87f45f5bcdffc2eb3edd7b86f4d62ca8b55847df00a382ee0171`。Windows 对照使用较早完整构建，仅应用同一关闭 guard；Linux 与 Darwin 的新库源文件逐字节一致，这些组件结果不代表最终全量构建。

增强的 spawn 测试补上了旧 POSIX 测试未实际等待 READY、未在失败后验证请求的缺口。Windows/Linux 已实际验证 25 次 READY 周期、marker 子进程、25 次拒绝、5 次缺失 executable 清空输出，以及随后真实 GET 的 200 和完整 `healthy` 字节；错误响应体被拒绝，Linux 恒定 FD/child 观察器也被拒绝。Darwin 的同一归属观察器及两种恒定计数器已经通过完整 C ABI 对照；新增 READY/请求增强仍等待当前候选原生执行。

密度控制夹具三平台各 21 项均通过原 30 秒预算。原 macOS 四项 fetch boundary/transition 失败对应六个 `127.0.0.2` 绑定错误，全部发生在相应用例启动 backend 之前；未降低跨地址与权限断言。当前 CI 准备第二个回环地址，要求同端口、两个不同地址的真实连接分别返回正确内容，再运行原四项契约。

[本轮候选与证据索引](evidence/v0.3.0/2026-11-12-native-close-recovery/index.json)、[10 份源码差异](evidence/v0.3.0/2026-11-12-native-close-recovery/candidate.diff)、[三平台组件审计](evidence/v0.3.0/2026-11-12-native-close-recovery/checkpoint.json)和[78 补丁证明](evidence/v0.3.0/2026-11-12-native-close-recovery/all78-overlay-proof.json)可复核。已提交[当前候选完整 macOS 与原 9 项 fuzz](https://github.com/ErosZy/capsid/actions/runs/43150779702)，精确 checkout `7402d3c` 的树与 `ef53262` 相同；CI 已报告 9 项 fuzz 通过；原 artifact 的七份随机输出均被截断，尚不能独立核对最终执行次数和完整编译/注册参数。新 macOS 的回环真实连接准备及受影响定向步骤均报告通过，完整普通矩阵仍在执行，原始报告待终结后审计。

## 出站连接额度与异步回调生命周期

回收父候选 `467d77d` 在其父候选 `f3a17a9` 上仅修改 `websocket_session.cc` 与其原单测。可控回归实测旧实现关闭连接后仍保留出站 channel/active 额度，直到持有 session 的异步回调对象销毁。修复在传输关闭完成时释放出站额度；入站流仍保留物理 socket 所有者，未完成写入仍保留真实缓冲和字节额度直到写完成。

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

原生观测进一步确认，失败 spawn 会短暂出现在父 PID 清单中，此时既没有 BSD 身份，`waitid(WNOHANG|WNOWAIT)` 也可能返回成功但 `si_pid=0`；真实僵尸则给出匹配的 `si_pid`。当前主候选观察器仍将前者计为子进程，完整父矩阵因此失败。私有修正以不回收方式确认身份；对于模糊 PID，有界复查完整清单并要求其消失。macOS 已实际通过 10 次缺失 executable spawn、真实存活/僵尸/回收正向控制，并拒绝恒零和恒一观察器；实际完整 C ABI fixture 现已通过并合入当前候选；其增强版的 Darwin 执行单独待验。

macOS 密度控制的原 30 秒超时已由真实栈定位：卡在 `HTTPServer.server_bind()` 的 `socket.getfqdn()`，尚未发出请求。私有夹具直接绑定数字回环地址，保留全部 HTTP 请求、发送屏障、内容校验和失败断言，并增加反向 DNS 故障控制。Windows/Linux/macOS 各 **21** 项均在原 30 秒预算内实际通过，Darwin 为 5.45 秒。生产密度控制器和调度均未修改，夹具修正已合入当前冻结候选。

`ws_stream.cc` 将每块数据的输入游标保持在局部变量中，逐字节解码语义和信用窗口保持原样。未修改 fuzzer oracle。相同不可变语料、ABBA 顺序的诊断比较约减少 13% 解码时间；该结果不作为产品性能点。当前源码原单测通过 ASan/UBSan。

密度父候选 `f3a17a9` 的[托管 fuzz](https://github.com/ErosZy/capsid/actions/runs/43139534152)已通过全部 **9** 个原门禁：7 个随机目标各完成 10000 次，另 2 个语料生成校验通过，无跳过。归档核对了 `max_len=65536`、单输入 2 秒、RSS 512 MiB、CTest 60 秒，以及全部目标的 `fuzzer,address,undefined` 实际编译/链接参数。[原始归档](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-original.zip)、[完整日志](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-job.log)和[审计](evidence/v0.3.0/2026-11-12-density-platform-controls/fuzz-terminal.json)可复核。这不替代完整运行时和依赖库的 sanitizer 验收。

## 全量与 workerd 参考

| 平台及实际源码 | 实际结果 | 严格参考 |
| --- | --- | --- |
| Linux production/LTO，较早父候选 `2979e33` | 完整 ALL；未过滤 1107 项执行，1102 通过、5 个既有 WPT 失败、0 跳过 | 171 项必需参考通过；严格兼容性接受；`ordinarySuitePassed=false` |
| Linux production/LTO，密度父候选 `f3a17a9` | 完整 ALL；未过滤 1107 项：1102 通过、5 个既有 WPT 失败、0 跳过 | 171 项必需参考通过；严格兼容性接受，原 5 项 WPT 失败保留 |
| Windows，较早候选 `7d393a0` | 未过滤 845 项：832 通过、5 个既有 WPT 失败、8 个声明的平台跳过 | 134 项必需平台参考通过；严格平台审计接受 |
| macOS，父候选 `2979e33` | 完整 ALL；定向 100 项执行，82 通过、18 失败、0 跳过 | 原 26 项失败中新增关闭 9 项，剩 17 项，另有新观察器用例失败；并非完整矩阵接受 |
| macOS，密度父候选 `f3a17a9` | 完整 ALL；757 项普通矩阵：730 通过、16 失败、11 个声明的平台跳过 | 两组真实配额用例新增通过；16 项仍未接受 |
| macOS，回收父候选 `467d77d` | 完整 ALL；757 项：737 通过、9 失败、11 个声明的平台跳过 | 比 `f3a17a9` 的失败集合少 7 项；仍未完整接受 |
| 关闭父候选 `ef53262` | 三平台关闭组件、观察模型和密度夹具通过；macOS 受影响定向步骤报告通过，完整矩阵执行中；原 fuzz 9 项通过但原始输出截断 | 完整普通矩阵与补充原始 fuzz 审计待结果 |
| 最新候选 `45bb9f4` | 强制回收定向 Windows/Linux 与四单元 ASan/UBSan 控制通过；独立 Linux production/LTO ALL 已启动 | 原 1108 项全量、171 项严格参考和最终全平台组合待验 |

Linux 的[完整参考接受](evidence/v0.3.0/2026-11-12-density-platform-controls/linux-parent-compatibility.json)、Windows 的[完整平台审计](evidence/v0.3.0/2026-11-12-density-platform-controls/windows-prior-platform-audit.json)保留原五份标准文件失败及对应子项超时。父快照的完整结果没有重标为最新候选全量。

macOS 父候选的 16 项失败仍完整保留，包括 spawn 观察器、密度夹具超时、native abort/large gzip，以及 WebSocket 关闭/共享额度回收和 ACCEPT/ACK 压力时序。[完整 757 项审计](evidence/v0.3.0/2026-11-12-native-terminal-review/macos-parent-full-terminal.json)逐项记录失败和 11 个声明的平台跳过；未增加平台豁免。

## 原生源连接终态及修正验证

pinned workerd 在 Linux 与 Darwin 各执行原 11 组输入，全部包与 payload 哈希一致；10 组已有 gzip/abort 状态模型完全一致。诊断用 raw 1 MiB 的实际读块长度不同，逐块数据及 backing 哈希分别独立验证，保留原始几何而未归一化。Darwin 的两组 raw 8 MiB abort 在参考进程停止前已实际关闭，观察为写端 `EPIPE`；其字节状态与原模型一致。这是平台源连接观测的支持缺口，不能据此放宽其他平台或 gzip 的关闭契约。

私有 abort 方案仅接纳已实测的 Darwin raw 8 MiB 两种 view 的 `EPIPE`，仍要求源 socket 在 helper 结束前实际关闭，保留完整字节、退出和时间预算检查。13 项契约通过，原 407 个故障变体和新增 62 个平台/生命周期变体均被拒绝；Windows/Linux 的 16 组真实 C ABI 用例通过。实际更新后的 Darwin 生产者现已通过 16 组真实 C ABI 对照，并合入当前候选。大 gzip 的 EOF 断言保持严格：参考端均在停止前 EOF，而产品在 8 MiB 或 96 MiB 完整读取后仍可能没有结束源连接，库层修正已完成三平台定向对照并合入，完整组合仍待验证。

[本轮证据索引](evidence/v0.3.0/2026-11-12-native-terminal-review/index.json)、[22 组原生输入对照](evidence/v0.3.0/2026-11-12-native-terminal-review/native-darwin-linux-comparison.json)和[Darwin 夹具实际控制](evidence/v0.3.0/2026-11-12-native-terminal-review/fixture-darwin-terminal.json)可复核。私有诊断准备阶段的依赖遗漏、Host 关闭导致 helper 缺失和观察器编译错误均归档为 **0 项实际测试**；修正配置后使用新的临时验证分支，未将准备失败视作产品结果。

回收父候选 `467d77d` 的 [5 个原完整 CI 任务](https://github.com/ErosZy/capsid/actions/runs/43142872561)继续分别记录：5 个任务现均已终结；TSan 的 1000 项矩阵中 994 通过、0 失败、6 个声明跳过，原两项 RSS 场景中一项通过、一项失败，独立 typed memory 接受步骤报告通过；任务仍因冒号路径上传失败，完整 instrumentation artifact 未取得。Windows 严格参考 134 项中 132 通过、2 失败、0 跳过；普通 Windows 全量因 gate 失败没有执行。Release 的 1107 项中 1094 通过、8 失败、5 个声明跳过；失败为原五份 WPT 加三项控制测试超时，严格兼容性审计和后续接受重放均拒绝，上传另因同一冒号路径失败。UBSan 1011 项中 1004 通过、2 项控制测试超时、5 个声明跳过；ASan 1012 项中 1003 通过、2 项同样的控制超时、1 项 cold-tool SIGINT 后 Worker 未回收失败、6 个声明跳过。日志没有 ASan/UBSan runtime diagnostic，但矩阵不接受。ASan 原 2 项 RSS 失败保留，已接受的独立内存边界保持；本次独立内存接受因矩阵仍有失败而明确拒绝。

这两份完整 sanitizer 日志及所有失败已归档。新增诊断目录包含 Linux 合法的 `sha256:…` 路径，上传服务因冒号拒绝原目录，未取得对应 artifact；不能据日志声称已独立核对完整 instrumentation archive。后续诊断捕获准备为保留原文件名的 tar 归档，归档故障和两项控制测试超时均未被隐藏。当前 macOS 737 项通过也不代表新候选全量；其中旧 spawn 观察器的偶发通过不消除已独立复现的 transient PID 问题。

## 强制回收的原私有审校与证据限制

针对 ASan 的 SIGINT 后未回收 Worker，已另建可控 C ABI 对照。先前针对阻塞 `waitpid` 的探测没有触达实际销毁路径，四组 blocking 调用数均为 0，未算作成功复现，也未据此改代码。进一步让真实子进程先停止，观察实际强制结束，并将非阻塞退出观测推迟 350 毫秒：旧实现三组均在 250 毫秒等待后返回，独立 `waitid(...WNOWAIT)` 在夹具清理前确认子进程尚未回收，退出状态也未获得；正常无延迟正向控制通过。

私有方案保留原 50 毫秒自然退出窗口，强制结束后使用真实阻塞退出观测，并重试 `EINTR`。Linux 四组均在夹具清理前实际回收，包含 0、1、3 次中断；Windows 同一源文件通过四次真实强制退出、独立原生 process handle 见证和三项原退出状态检查。没有生产注入开关，原实现的子进程也在失败判定后由夹具清理。该方案现已合入 `45bb9f4`，同步了内部描述并完成 ASan/UBSan 组件上的七项原 cold-tool 验证；最终完整候选验证仍开放。尚未将此可控缺口认定为某次 ASan SIGINT 失败的唯一原因。

当前 `ef53262` 的 9 项 fuzz 门禁均报告通过、零跳过；七份 JUnit 随机输出均按原 CI 的 1024 字节阈值截断，不能把配置的每目标 10000 次当作已独立观察的最终次数。已准备同源码、同选择器和预算的完整 LastTest、注册与编译命令补采工作流；本机 Windows 和 WSL 到 API 的 TLS 连接在 HTTP 前失败，尚未派发，记录显示零请求、零远程变更。现有 macOS 全矩阵不受影响，GitHub 连接器仍可读其状态。

[私有回收差异](evidence/v0.3.0/2026-11-12-forced-reap-review/private-forced-reap-proposal.diff)、[Linux 原实现](evidence/v0.3.0/2026-11-12-forced-reap-review/linux-original.stdout.log)、[Linux 方案](evidence/v0.3.0/2026-11-12-forced-reap-review/linux-proposed.stdout.log)、[Windows 方案](evidence/v0.3.0/2026-11-12-forced-reap-review/windows-private-proposal.json)与[证据索引](evidence/v0.3.0/2026-11-12-forced-reap-review/index.json)保留结果和范围。该节保留先前私有审校记录；当前生产源码为 `45bb9f4`，合入与新证据见本文开头。

## 托管父候选最终结果

`467d77d` 的五个托管任务均已终结。TSan 的所选矩阵 **1000 项中 994 通过、0 失败、6 个声明跳过**，日志没有 TSan runtime diagnostic；原 RSS 一项失败保留，独立 typed memory 接受步骤报告通过。任务因诊断目录的冒号路径上传失败，无法据此声称已独立核对完整 instrumentation archive。它与当前 `45bb9f4` 的新全量验证分开记录。

Windows 完整 ALL 已构建，但后续严格参考 **134 项中 132 通过、2 失败、0 跳过**，所以普通全量没有执行。失败一为 `workerd_ws_gate_archive_controls` 在原 120 秒预算超时；另一为真实 pinned workerd 的 `codec-supp-fragment-crc-late` 未在 helper 停止或夹具释放前观测到源 EOF，使 metadata reference 报告 `complete=false`。该参考端失败、31 项原观测及原 ZIP 均保留，尚未复现并解释前不修改关闭契约，也未将旧 134 项通过记录重标为本次通过。

[五个任务最终状态](evidence/v0.3.0/2026-11-12-hosted-native-terminals/all-prior-hosted-job-terminal-states.json)、[TSan 完整审计](evidence/v0.3.0/2026-11-12-hosted-native-terminals/tsan-terminal.json)、[Windows gate 审计](evidence/v0.3.0/2026-11-12-hosted-native-terminals/windows-gate-terminal.json)、[Windows 原始归档](evidence/v0.3.0/2026-11-12-hosted-native-terminals/windows-gate-original.zip)和[证据索引](evidence/v0.3.0/2026-11-12-hosted-native-terminals/index.json)可复核。当前 `45bb9f4` 的 Linux 全量序列及 `ef53262` 的 macOS 完整普通矩阵仍独立运行。

## 仍需完成

当前 `45bb9f4` 三平台完整 ALL/矩阵与组合验收仍开放；新的独立 Linux production/LTO ALL 已启动，后续串行执行 1108 项原全量与 171 项严格参考。密度父候选 `f3a17a9` 的 Linux production/LTO 完整 ALL 已通过，实际注册 1107 项，未过滤全量已结束，1102 通过、5 个既有 WPT 失败、0 跳过，171 项必需参考已通过并获得严格兼容性接受；其 macOS 完整 ALL 与普通矩阵已结束，730 通过、16 失败、11 个声明的平台跳过。最新回收候选的三平台原生定向复测与 fuzz 均已通过并单独记录，未覆盖或中止父快照的完整证据。最终候选完整 ASan/TSan/UBSan、RSS 与资源入口仍须重验。新增待修项目包括两个控制测试的原预算超时、强制回收修复的最终全量覆盖及 Darwin 出站 ACCEPT/ACK 时序的全矩阵复核；旧快照所有失败未删除。ABI/旧接口、分配故障、长时间组合运行也需与最终源码对应。R1/R2/R4 仅完成各自定向要求，R3/R5/R6/R7/R8 仍在推进，R9 未完成。

正式 QPS/cold/density 仍需同一安静会话、候选/v0.2.2/pinned workerd 对照和完整原始证据。密度继续要求全部后代 PSS、2048 MiB、两次相等正读数及退出证据。性能页历史数据保持 unverified，1280/960/820/600 四视口已有真实截图与布局检查，正式数据载入后再验展示，用户无需连接浏览器。
