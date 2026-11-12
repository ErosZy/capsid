# v0.3.0 当前验收与兼容边界

更新时间：2026-11-12T13:23:46.416142+00:00。**v0.3.0 尚未完成全平台、全量资源和正式性能验收。** 当前冻结候选 `4e469b362f2f32d35d1a9ef9874f5e6456996e44` 含 5140 份源文件、SDK78；除独立更新的本文外，5139 份文件与工作区逐字节一致。主工作区 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`。

[密度及上一轮证据索引](evidence/v0.3.0/2026-11-12-density-platform-controls/index.json)、[候选身份](evidence/v0.3.0/2026-11-12-density-platform-controls/candidate.json)与[验收检查点](evidence/v0.3.0/2026-11-12-density-platform-controls/checkpoint.json)记录实际源码和验证范围。全部 R1–R9 及历史仍保存在工作区 `current-goal-requirements.json`；要求和状态未裁剪。此前的[握手短写修复证据](evidence/v0.3.0/2026-11-12-queued-http-handshake/index.json)继续保留。

## 冷启动工具后代清理

当前 `4e469b3` 在父候选 `63a3cc1` 上合入 4 份 benchmark/测试文件。Linux 专用 cold-start 工具在计时开始前建立自己的 [child subreaper](https://man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html)，公开 C ABI 先销毁并回收直接 worker，工具再回收由自身收养的后代，完成后恢复原状态；成功样本必须在清理完成后输出。逐个使用内核自有 child 身份和精确 PID 等待，重试 EINTR，兼顾无 SIGCHLD 退出信号的 clone 后代；没有按进程名称扫描或修改公开 C ABI 的进程归属。原冷启动计时、PSS 两次相等判据、采样次数及原 7 项测试保持不变。

新增独立 `benchmark_cold_descendants`，预算 30 秒。包装器真实创建 fork/clone 子进程，随后 exec 原生 worker；协议、响应和内存来自原生运行时。SIGINT、SIGTERM、正常输出、错误响应、超时合计 10 场景，在外围夹具执行清理前断言整树已回收，正常样本还核对后代确实计入 PSS。GCC ASan 和 Release 各通过原 7 项及新增 10 场景，旧工具各被同一 10 个断言拒绝。

Clang ASan/UBSan 的同 worker 原始 7 项对照还再次复现旧工具 SIGINT 后留下真实僵尸后代；候选工具取消清理通过。但两者都出现成功采样时 PSS 持续变化，旧 7 项为 2 失败、新 7 项为 1 失败；新增场景 8 项完成、2 项正常 PSS 样本拒绝。这些结果不能计为完整 Clang 接受，具体僵尸进程来源仍未证明。此次合入仅确认已独立验证的后代回收修复；Clang PSS、父候选 TSan 的 3 项、Windows 原参考 EOF、当前 macOS 和最终资源/正式性能接受继续开放。

下文 1110/171、UBSan、fuzz 和静态包结果属于明确标出的父候选 `63a3cc1`，不替代新组合的全量结果。父候选正在运行的完整 ASan/UBSan 构建保持原冻结输入，用于取得真实诊断；当前源码尚未完成全组合验收。正式性能点仍为 0。[候选及源码差异](evidence/v0.3.0/2026-11-12-cold-descendant-cleanup/candidate.json)、[同 worker 原始对照](evidence/v0.3.0/2026-11-12-cold-descendant-cleanup/clang-same-worker-comparison.json)与[本轮证据索引](evidence/v0.3.0/2026-11-12-cold-descendant-cleanup/index.json)保留完整边界。

## 父候选全量、静态包及新发现

当前 `63a3cc1` 的 Linux Release/LTO 完整 ALL 已通过，未过滤 1110 项实际为 1105 通过、0 跳过、5 项原 WPT 失败；原始结果与时限保留，同源严格 171 项 workerd 对照已全部通过，版本化兼容接受完成；原 5 项 WPT 差异和 4 项超时保留，ordinarySuitePassed 与 standardsConformanceClaimed 仍为 false。这不是普通套件全通过。[未过滤原始结果](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/current-linux-unfiltered.json)与 [JUnit](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/current-linux-unfiltered-junit.xml)可复核。

完整 CI 的 UBSan 选择 1014 项，1009 通过、0 失败、5 项原声明跳过；ASan 选择 1015 项，1008 通过、1 失败、6 项原声明跳过。新增失败来自 SIGINT 冷启动取消：工具退出后仍有一个真实僵尸后代，原断言在外围夹具清理前发现它。两份 typed ASan 资源报告虽然完成，内存接受门禁仍因普通套件失败而正确拒绝。隔离的原 7 项与随后 200 轮取消未复现，不能计作修复，也尚未证明遗留进程的具体来源。冷启动工具的专有后代回收方案正以真实原生 worker、普通 fork 与 Linux clone 后代做旧实现负向和三种构建验证，尚未合入。[ASan 审计](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/current-asan-review.json)和 [UBSan 审计](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/current-ubsan-review.json)保留完整选择与跳过名单。

同源 Alpine 3.24 Docker 原发布配方已实际构建和打包，9 项原生/包消费测试通过、0 跳过，包括 9 个启动回收及 4 个压力子场景；3 个归档内程序分别核对 SHA256，并独立解析 ELF，均无解释器和共享库依赖。静态 PIE 的动态重定位节不等于共享库依赖。此项不代表完整 musl 全量或 workerd 接受，未执行发布。[静态包独立审计](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/current-musl-review.json)与 [原始测试输出](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/current-musl-LastTest.log)可复核。

当前 Windows 原 134 项 workerd 对照中 133 通过、1 失败、0 跳过；失败为参考 workerd 在 `codec-supp-fragment-crc-late` 后未在原期限内让源端观察到真实 EOF，产品两种模式的同分片通过。原始失败保留，正在同源、同锁定二进制上观察，尚未放宽关闭断言。Hosted Linux 接受还拒绝了依赖 perf 的普通测试跳过，须补齐真实前提后重验。当前 Windows、ASan、macOS runner 和最终资源/正式性能接受仍开放。已重新完成 3×20 秒 Windows QPC/WSL 时钟校准和三条正式命令输入核对；正式性能点仍为 0，生产调度、QPS 64 并发及你批准的按池扩展密度预热协议保留。[本轮检查点与证据索引](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/index.json)明确这些边界。

本轮继续终结的 TSan 选择 1003 项，994 通过、3 失败、6 项原声明跳过。失败为 `native_fetch_large_gzip` 的辅助进程状态观测，以及 managed 模式的 native-reader-lifecycle、reader-scope 完整采集/清理断言；原结果保留，须检查详细场景后修正，未接受完整 TSan。Windows 同输入、同二进制的 3 轮复查各 31 场景全部通过；9 个输入摘要逐一与失败 CI 对齐，5 份文件只在隔离目录调整换行，原失败未替换。GCC ASan 和 Release 各通过原 7 项与新增 10 个真实后代场景，旧实现各被 10 个断言拒绝；Clang ASan/UBSan 原成功采样因 PSS 持续变化失败，正在同一 worker 上比较旧、新工具，方案尚未合入。真实 perf 软件计数器前提已验证，但独立控制任务因未构建实际 Host/worker 身份夹具而未完成，未计为测试通过。当前 macOS 单独重试已被服务接受并取得真实 runner，原架构、源码和预算保留。[当前严格接受](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/current-linux-compatibility-acceptance.json)、[TSan 原始结果](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/current-tsan-review.json)和[三个 Windows 复查](evidence/v0.3.0/2026-11-12-linux-musl-and-cold-audit/windows-codec-three-replays.json)保留。

## 启动失败回收与归档测试复制

父候选 `63a3cc1` 在 `fa40606` 上合入 6 份文件。真实子进程创建之后，如果外层分配、构造内存分配或构造异常使公开 spawn 返回失败，POSIX 旧实现仅调用一次 `waitpid()`，遇到 EINTR 会把未回收子进程留给调用者；Windows 旧实现只等 250 毫秒便关闭句柄，而 [TerminateProcess 的官方契约](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess)要求调用者另行等待异步结束。现在 POSIX 重试被中断的阻塞回收，Windows 保留自有句柄直到内核确认进程退出。

新增 `worker_spawn_rollback` 的预算为 15 秒。测试先后完成真实健康 GET，故障期间使用不会因 IPC EOF 自行退出的真实子进程；公开错误码、清空输出指针、错误文本和完整 FD/句柄数均检查。Windows 6 个、Linux/macOS 各 9 个场景通过，共 24 个。只恢复旧 client 的同夹具对照实际出现 3/6/6 个返回时尚未结束或回收的场景，共 15 个；观察发生在夹具清理之前。Windows 350 毫秒窗口是显式控制的异步完成模型，进程信号与等待使用真实内核 API。Linux/macOS 各 3 个实际目标对象核对 ASan/UBSan 编译参数和符号，尚不等于完整 provider 验收。强制包含的 native-call 适配只属于单独测试对象，生产 client 没有测试适配。[三平台回收证据](evidence/v0.3.0/2026-11-12-startup-and-archive/candidate.json)和[源码差异](evidence/v0.3.0/2026-11-12-startup-and-archive/candidate.diff)保留原始结果。

归档测试仅调整构造数据的复制方式：对象、数组、共享边和循环每次独立复制，只复用不可变原始值；每次读取当前输入，复杂对象回退 structuredClone。原 296 项控制全部保留，新增 1 项图归属与输入新鲜度检查，Windows/Linux 各 297 项全部通过、0 跳过，原 120 秒分组预算不变。旧复制器经过语法检查后，被新增断言正确拒绝。同机对照的 117 项归档组分别约 86→76 秒、93→77 秒；这是测试执行开销，不是产品性能数据。validator、真实 workerd 输入和生产调度均保持原样。首次 CI 的换行摘要前置失败、重复 import 负向脚本错误以及健康请求前置夹具错误继续留档，没有计为通过。[归档组完整对照](evidence/v0.3.0/2026-11-12-startup-and-archive/component-review.json)和[证据清单](evidence/v0.3.0/2026-11-12-startup-and-archive/index.json)可复核。

当前组合源码的[完整 CI](https://github.com/ErosZy/capsid/actions/runs/43211057022)保留原 Release/LTO、三类 sanitizer、macOS、Windows、fuzz 与最终汇总接受规则。原 9 项 fuzz 已通过，7 个随机目标各完成 10000 次，共 70000 次；27 个编译单元以及原输入、时间和 RSS 上限已核对。[当前 fuzz 审计](evidence/v0.3.0/2026-11-12-startup-and-archive/current-fuzz.json)与[原始归档](evidence/v0.3.0/2026-11-12-startup-and-archive/current-fuzz-original.zip)保留。其他完整矩阵继续运行；本轮 macOS 作业因 GitHub runner 容量不足，在 runner_id=0、执行步骤为空的状态被服务取消，没有测试结果。单独重试 API 暂因整个工作流仍在运行而拒绝；待本轮终结后只重试该 macOS 作业，保留原架构、源码、选择与时限。[服务取消及重试响应](evidence/v0.3.0/2026-11-12-startup-and-archive/current-macos-service-cancellation.json)保留，完整 CI 尚未接受。本地新 Linux 组合已经开始 ALL，随后执行 1110 项未过滤全量及 171 项严格参考。当前源码的 ASan/UBSan、TSan ALL 和未过滤全量已另行排队，等待 production 全量与参考终结，原 WPT、资源检查和单项时限保留；这些是准备状态，尚未计作通过。只替换了没有创建源码、构建或测试子进程的旧计划，原父候选参考完整终结。R1–R9 范围未裁剪，正式性能点仍为 0。[本轮检查点](evidence/v0.3.0/2026-11-12-startup-and-archive/checkpoint.json)记录当前验收边界。

## 原生 poll 修复及父候选完整结果

父候选 `fa40606` 在文档门禁父候选 `2c8bd7c` 上合入 6 份运行时及测试文件。真实 Darwin 追踪显示 filler 请求已经进入 Worker，后续 ACCEPT 没有到达 frame handler；只读堆栈显示主线程持续经过 kqueue 的写回调。运行时每次 flush 都调用 `uv_poll_start()`，即使监听事件集合没有变化。仓库内 libuv 的 [poll 实现](../vendor/txiki.js/deps/libuv/src/unix/poll.c)每次 start 都先停止旧监听，而 [kqueue 实现](../vendor/txiki.js/deps/libuv/src/unix/kqueue.c)会将同一 FD 在当前批次中未分发的事件作废。持续的可写回调因而可反复删除待处理的可读事件。

修复将已应用事件集合初始化为 0，只在集合变化时重新注册。真实 macOS 同一会话中，仅把 runtime 源文件恢复为旧版本，当前 Host 测试和当前观测夹具保持相同，即在 5.32 秒再次复现缺失 ACK 的原局部期限失败；新实现的原 gate 控制和两项实际压力测试全部通过。它验证的是实际 Worker、libuv 和队列行为。[同源源码差异](evidence/v0.3.0/2026-11-12-native-poll-interest/candidate.diff)、[macOS 前后原始输出](evidence/v0.3.0/2026-11-12-native-poll-interest/macos-native-LastTest.log)与[旧 runtime 对照](evidence/v0.3.0/2026-11-12-native-poll-interest/macos-original-poll-negative.stderr.log)可复核。

同时合入真实背压观测夹具，消除固定 200 毫秒关闭计时器先于 ACCEPT 的独立测试竞态。Host 等到真实首段 body、物理写停顿、4096 字节满队列和未提交 ACK 都被观察，再由原请求的计时器上下文执行关闭。Windows、Linux、macOS 各通过普通与延迟 ACCEPT 共 4 个真实场景，合计 12 个；各平台旧计时器在新运行时下仍实际失败。原 1 MiB 完整 filler、请求归属、ACK/CANCEL 顺序、禁止误 open、自然退出与实际回收断言全部保留，原 gate 10 秒、两项压力各 30 秒不变。POSIX receipt 使用夹具自己的 stdout，生产 FD 关闭规则保持原样。

Linux/macOS 两个[原生 ASan/UBSan 任务](https://github.com/ErosZy/capsid/actions/runs/43202496647)均成功，六个实际目标对象逐一核对编译参数和 sanitizer 符号；这仍不是完整运行时及 provider instrumentation 接受。Windows 使用既有原生构建，重编译当前 runtime 的普通和夹具两个单元，实际执行四个压力场景及旧计时器负向控制，属于当前组件验证。之前对照编译缺少测试头文件的两个失败任务及 JUnit 输出截断失败继续保留。诊断用 runtime 输入日志没有合入。

父候选 `fa40606` 的[完整 macOS 普通矩阵](https://github.com/ErosZy/capsid/actions/runs/43203769715)已经终结：758 项中 747 项通过、0 失败，11 项原声明跳过保留；20 项受影响定向检查全部通过。原 757 项注册属性与预算逐项一致，新增文档控制为 20 秒；此前两项真实背压失败均已通过。[父候选完整审计](evidence/v0.3.0/2026-11-12-startup-and-archive/parent-macos-full.json)和[原始归档](evidence/v0.3.0/2026-11-12-startup-and-archive/parent-macos-full-original.zip)可复核。同源 9 项 fuzz、70000 次及 27 个编译单元也已独立核对，保留在[父候选 fuzz 审计](evidence/v0.3.0/2026-11-12-native-poll-interest/current-fuzz.json)。这些结果属于明确标出的父源码。文档父候选 `2c8bd7c` 未过滤 1109 项也已终结：1104 项通过、0 跳过、5 项原 WPT 失败完整保留；同源 171 项严格参考全部通过，版本化兼容接受完成。原 5 项 WPT 失败及 4 项超时保留，ordinarySuitePassed 与 standardsConformanceClaimed 仍为 false。[父 Linux 接受记录](evidence/v0.3.0/2026-11-12-startup-and-archive/parent-linux-compatibility-acceptance.json)可复核。生产 Host 调度、QPS 64 并发及已批准密度协议保持不变，正式性能点仍为 0。

## 文档归档门禁父候选

文档门禁父候选 `2c8bd7c` 在运行时父候选 `3a324a9` 上仅合入 5 份文档及门禁文件，生产运行时、provider、调度和原压力测试未变。根工作区原文档审计把搬到证据目录中的历史 `qualification-before.md` 当作当前维护文档，错误地按新目录解释旧相对链接。修正仅识别日期目录中这个确切文件名，并要求相邻原始 inventory 的 SHA256 与归档字节一致；冲突摘要、缺失索引、修改字节及换行变化均拒绝。其他 Markdown 仍接受原链接、当前措辞与可达性检查。此前一个只完成部分保存的 Darwin 证据目录补充了当前文件清单，原 `complete:false` 和准备失败状态保留，没有将其改写为通过。

Windows、Linux 各 6 组控制通过；两平台在实际工作区验证 47 份维护文档与 19 份旧归档。端到端又分别注入普通断链、未索引文档、归档字节篡改、证据目录内普通 Markdown 断链、冲突摘要、缺失索引，六种错误全部拒绝。原文档门禁仍为 20 秒，新增独立 `documentation_archive_contract_controls` 也为 20 秒。[源码差异](evidence/v0.3.0/2026-11-12-documentation-archive-and-native-pressure/candidate.diff)与[实际控制](evidence/v0.3.0/2026-11-12-documentation-archive-and-native-pressure/documentation-controls.json)可复核。

该文档门禁父候选已独立启动 Linux production/LTO ALL、未过滤全量和严格参考，注册 1109 项；原 1108 项的输入、预算、跳过和失败规则保留。下文的 1108/171 与 macOS 757 结果属于运行时父候选 `3a324a9`，不能替代当前 1109 项组合。[本轮检查点](evidence/v0.3.0/2026-11-12-documentation-archive-and-native-pressure/checkpoint.json)和[证据索引](evidence/v0.3.0/2026-11-12-documentation-archive-and-native-pressure/index.json)保留当前边界。

## 本轮模型独立性、HEAD 和原生地址修复

运行时父候选 `3a324a9` 包含前一候选 `00ea61b` 的模型独立性修复。原 native argument factory 将固定 payload 数组直接暴露给调用者；实际反例将一个返回模型的字节改为 99 后，后续新模型也变为 99，并被原 validator 接受。现在每次构建独立数组，只共享容量 32 的深冻结输入模板，每次验证仍完整比较当前报告。原 9246 项全部保留，新增 3 项模型完整性检查；Binding 的原 13 组、10920 组输入和 19189 个错误样本保留，再新增 1 组复制独立性检查。其构造样本先深冻结，只缓存构造样本的序列化字节，每个反例仍得到完整、独立、可变的数据图；没有缓存通过结果。一个更慢的复制方案已保留证据并放弃。

[三平台实际审计](evidence/v0.3.0/2026-11-12-model-head-boundary/all-three-argument-models.json)核对 Linux、Windows、macOS 均通过 **9249 项 native argument、14 组 Binding**，每平台原 19189 个错误样本全部拒绝。原 60/30 秒预算不变；native 约 6.38/11.02/13.62 秒，Binding 约 16.07/25.11/28.74 秒（依次 Linux/Windows/macOS）。Windows 托管 checkout 的 Binding 控制文件仅有 Git CRLF 换行差异，实际 hash 单独保留；冻结 LF 文件也在本地 Windows 实际通过。Binding 在 macOS 仍接近原预算，不能据此宣称全部负载下的超时已解决。

真实 macOS pinned workerd 的原 ordinary core 30 项与 codec metadata 31 项通过；boundary 22 项完整捕获，但原契约拒绝目标连接的 `127.0.0.2` 客户端地址。产品两种模式呈现相同原生路由选择。修正仅允许跨地址重定向目标连接使用该回环别名；原其他地址、权限、TCP 身份、请求/响应字节和关闭断言保留。新增 25 项地址控制，原 640 项完整保留，共 665 项通过；同一模型通过 workerd 与两个产品模式共 66 项原始捕获重放，未重写实际捕获或将重放称作新执行。

HEAD 回归已独立复现：立即关闭已知长度 H1 的 guard 同时命中了非零 Content-Length 的 HEAD。原 codec metadata 的 1 项、ordinary core 的 2 项没有在 helper 结束前观测到 EOF。仅给该 guard 增加 HEAD 排除后，Windows 原 codec 31 项、ordinary core 30 项全部通过，原 2 组大 gzip、8 组 abort、2 组 TLS 也通过。此 Windows 对照使用较早完整 native 基线，只替换一个库编译单元；它不代表最终候选全量验收。78 个补丁在干净 vendor 全部应用，67 个输出中仅 `client-http.c` 改变。新 overlay key 为 `01d8261928bdf579eafcca4325487edbaf47ea5d5f8d7456176ece0e44869eee`，manifest 为 `6a33c04fa93773aac536b4c146c3595ce308e29ed352bfd96243873568d12784`。

[本轮源码差异](evidence/v0.3.0/2026-11-12-model-head-boundary/candidate.diff)、[Windows 原始对照](evidence/v0.3.0/2026-11-12-model-head-boundary/windows-header-paired-observations.json)、[66 项实际捕获重放及原正向控制](evidence/v0.3.0/2026-11-12-model-head-boundary/windows-boundary-and-header-positive-controls.json)和[78 补丁证明](evidence/v0.3.0/2026-11-12-model-head-boundary/all78-header-overlay-proof.json)可复核。运行时父候选 `3a324a9` 的 Linux production/LTO 完整 ALL 已通过。**未过滤 1108 项为 1103 通过、5 项原始 WPT 失败、0 跳过**；原退出码仍为 8，随后原 171 项严格 workerd 参考门禁全部通过。实际固定参考及三种产品模式的五份 WPT 报告匹配既定 profile，原四项超时保留。该组合接受为 workerd 兼容，未将普通全量或标准一致性报告改写为全部通过。[Linux 全量](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/linux-full.json)、[严格接受](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/linux-compatibility-acceptance.json)和[全部原参考捕获归档](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/linux-strict-reference-original.zip)均保留。

同一精确 checkout `bae2709` 的 [macOS 完整 ALL/普通矩阵](https://github.com/ErosZy/capsid/actions/runs/43158688231)已结束：757 项中 744 通过、2 失败、11 个原声明跳过；18 项受影响用例为 16 通过、2 失败。原 HEAD 关闭与跨回环地址六项失败全部消除；剩余为 `worker_outbound_websocket` 和 `worker_outbound_result_ack_pressure`。完整注册属性、原预算和声明跳过与父快照相同；不能把快速步骤的成功状态当成测试通过。[macOS 审计](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/macos-full.json)与[原始归档](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/macos-full-original.zip)可复核。

首个 fuzz 任务在 apt 安装阶段失败；同一精确源码的[独立补跑](https://github.com/ErosZy/capsid/actions/runs/43159419477)已完整审计：9 项通过、0 跳过，7 个随机目标各实际执行 10000 次，合计 70000 次。27 个实际编译单元及原 60 秒预算、64 KiB 输入、2 秒单输入、512 MiB RSS 限制均核对；这不代表完整运行时和依赖库 sanitizer 验收。[fuzz 审计](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/current-fuzz.json)和[完整原始归档](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/current-fuzz-original.zip)保留，原[安装失败记录](evidence/v0.3.0/2026-11-12-fuzz-setup-recovery/index.json)也继续保留。

## 背压观测方案的前置复现记录

Windows 实际延迟 350 毫秒提交 ACCEPT，复现旧固定 200 毫秒计时器关闭的失败：没有 blocked ACK，也未形成要求的 assigned CANCEL。私有方案等到真实首段 body、4096 字节满队列、物理写停顿和有主未提交 ACK 均被观测，再发送关闭信号，由原 HTTP 请求的计时器执行关闭。原 1 MiB filler、ACK/CANCEL 顺序、禁止误 open、请求归属、完整终结与自然退出断言保留；两项 CTest 仍各 30 秒。原 11 个无效压力控制和两种调度顺序均保留，并增加通知前提与发布失败控制。

私有 `cb5a8f1` 在 Windows 和当前 `3a324a9` Linux production/LTO 运行时分别通过普通和延迟 ACCEPT 共 4 个真实场景，每平台含 2 个延迟场景；两平台均无所属 receipt 残留。POSIX receipt 使用夹具自己的 stdout，生产 Worker 的无关 FD 关闭规则保持原样。但原生 macOS ASan/UBSan 构建的 gate 控制通过后，两项实际压力测试均在等待 receipt 的原 5 秒局部期限失败；六个实际目标文件的 instrumentation 已记录，**上述仅改夹具的旧快照没有合入，不能据此宣称当时 macOS 通过**。此前 Linux 缺少 Hono/c-ares 依赖、macOS 使用不适用的 profile 等准备失败也全部保留。

私有诊断快照 `862b4d1` 的[Linux/macOS 原生仪器化验证](https://github.com/ErosZy/capsid/actions/runs/43197676808)已结束：Linux 三项实际测试通过，包含四个压力场景，但后处理读取的 JUnit 通过输出被截到 1024 字节，导致任务最终失败；完整 LastTest 的字节、顺序、自然退出及六个目标对象的 sanitizer 参数/符号均已独立核对。该次旧计时器负向控制尚未执行，未算作通过。macOS gate 控制通过，两项压力仍失败。进一步私有 `5fee708` 的输入日志确认最初 filler 请求已进入处理器，随后 ACCEPT 没有在原期限内进入同一处理器，也没有 ACK 观察事件；这还不能确定最终根因。[只读堆栈诊断](https://github.com/ErosZy/capsid/actions/runs/43199420623)已经完成保存；本轮另行完成了上述同会话旧 runtime 实际对照，诊断运行时日志没有合入生产源码。生产运行时、生产调度和 QPS 64 并发均不变。[私有源码差异](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/writer-pressure-private.diff)、[本轮检查点](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/checkpoint.json)和[证据索引](evidence/v0.3.0/2026-11-12-linux-terminal-writer-pressure/index.json)分别记录已通过范围和未通过项。正式性能点仍为 0。

## 强制回收父候选

父候选 `45bb9f4` 在 `ef53262` 上合入实际 OS 强制退出和回收修复，并增加 Linux 永久 `worker_forced_reap` 回归。公开 C ABI 要求 destroy 返回前子进程已回收；原实现强制结束后最多等待 250 毫秒，延迟的退出观测可能使其返回时仍未回收。修复保留原 50 毫秒自然退出窗口，强制结束后等待真实 OS 退出结果，遇到 `EINTR` 继续等待。Host 调度与回收执行器并发数不变；内部文档已明确自然退出窗口与总回收耗时的区别，不再将其写为固定 300 毫秒上限。C ABI 布局未变。

实际对照中，旧实现三组在夹具清理前未回收；新实现四组通过，真实 `waitid` 见证和退出状态均检查。Windows 同源方案通过四次真实强制结束和三项原退出状态控制。另将当前 client、独立故障用例、原退出状态用例和当前 cold controller 四个翻译单元完整置于 ASan/UBSan，核对每个目标的编译参数及实际 sanitizer 符号；四组故障控制、四项原状态模式及七项原 cold-tool 测试通过，包含实际 SIGINT、SIGTERM 清理，cold-tool 原 100 秒预算保留。该仪器化组合使用较早完整基线的 runtime/worker/provider，属于当前组件验证，未宣称最终候选全量 instrumentation 接受。

[当前源码差异](evidence/v0.3.0/2026-11-12-forced-reap-integration/candidate.diff)、[四单元 instrumentation 与实际控制](evidence/v0.3.0/2026-11-12-forced-reap-integration/linux-asan-ubsan-component.json)、[七项 cold-tool 原日志](evidence/v0.3.0/2026-11-12-forced-reap-integration/asan-actual-original-seven-cold-controls.stderr.log)和[本轮证据索引](evidence/v0.3.0/2026-11-12-forced-reap-integration/index.json)均已保留。父候选 `45bb9f4` 的独立 Linux production/LTO 完整 ALL 已通过；未过滤 1108 项已结束，为 1097 通过、11 失败、0 跳过，其中六项 HEAD 关闭失败和五项原始 WPT 失败。严格参考因原始全量前提不满足而在执行 171 项之前退出，不能计为接受；其原始记录继续保留。随后当前 `3a324a9` 的独立组合已按原输入与预算完成上述验证。最新 client 的 Darwin 执行及最终三平台组合仍开放。

## 连接结束与原生夹具父候选

父候选 `ef53262` 在 `467d77d` 上修改 9 份既有文件并增加一份真实 Darwin primary 归档。已合入：已知长度且不复用的 H1 响应在 wire 完成时结束传输；Darwin abort 的精确 `EPIPE` 源关闭观测；无反向 DNS 依赖的数字回环 HTTP 夹具；不回收的原生子进程归属观察器；POSIX 实际 READY、C ABI marker 正向控制和失败后 GET 状态/字节恢复；macOS 独立回环地址的 CI 准备。生产调度和 QPS 并发保持原契约。

库修正在真实 Darwin、Windows、Linux Worker 中合计通过 6 组大 gzip、32 组 abort 和 8 组 TLS 用例，原 8/96 MiB 字节模型、源 EOF 与退出断言及原预算均保留。Darwin 同会话旧实现明确复现 8 MiB 全部解码后没有 EOF，替换一个库目标文件后两组大 gzip 均通过。78 个补丁在干净 vendor 上完整应用；67 个受补丁影响的文件中只有 `client-http.c` 变化。新 overlay key 为 `71736ec71e3eef46f1e1eb3d7a8a25a59a3dba2f87893b0798665c04864c70a6`，manifest 为 `1097d8e961db87f45f5bcdffc2eb3edd7b86f4d62ca8b55847df00a382ee0171`。Windows 对照使用较早完整构建，仅应用同一关闭 guard；Linux 与 Darwin 的新库源文件逐字节一致，这些组件结果不代表最终全量构建。

增强的 spawn 测试补上了旧 POSIX 测试未实际等待 READY、未在失败后验证请求的缺口。Windows/Linux 已实际验证 25 次 READY 周期、marker 子进程、25 次拒绝、5 次缺失 executable 清空输出，以及随后真实 GET 的 200 和完整 `healthy` 字节；错误响应体被拒绝，Linux 恒定 FD/child 观察器也被拒绝。Darwin 的同一归属观察器及两种恒定计数器已经通过完整 C ABI 对照；新增 READY/请求增强已在 `ef53262` 的实际 macOS 定向及完整矩阵中通过。

密度控制夹具三平台各 21 项均通过原 30 秒预算。原 macOS 四项 fetch boundary/transition 失败对应六个 `127.0.0.2` 绑定错误，全部发生在相应用例启动 backend 之前；未降低跨地址与权限断言。当前 CI 准备第二个回环地址，要求同端口、两个不同地址的真实连接分别返回正确内容，再运行原四项契约。

[本轮候选与证据索引](evidence/v0.3.0/2026-11-12-native-close-recovery/index.json)、[10 份源码差异](evidence/v0.3.0/2026-11-12-native-close-recovery/candidate.diff)、[三平台组件审计](evidence/v0.3.0/2026-11-12-native-close-recovery/checkpoint.json)和[78 补丁证明](evidence/v0.3.0/2026-11-12-native-close-recovery/all78-overlay-proof.json)可复核。已提交[当前候选完整 macOS 与原 9 项 fuzz](https://github.com/ErosZy/capsid/actions/runs/43150779702)，精确 checkout `7402d3c` 的树与 `ef53262` 相同；CI 已报告 9 项 fuzz 通过；原 artifact 的七份随机输出均被截断，尚不能独立核对最终执行次数和完整编译/注册参数。原生归档已终结审计：757 项中 738 通过、8 失败、11 个声明跳过；受影响的原 11 项实际为 7 通过、4 失败，退出码为 8。步骤显示成功源于保存退出码后继续完整矩阵，不能据此判定测试通过。该快照的原 9 项 fuzz 后续同源补充归档已取得，7 个随机目标各实际完成 10000 次，27 个相关编译单元及原 60 秒 CTest、单输入 2 秒、64 KiB 输入、512 MiB RSS 上限已核对。

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

## 父候选 macOS 完整结果与剩余执行

`ef53262` 的原完整 macOS 矩阵 **757 项中 738 通过、8 失败、11 个声明跳过**。相较 `467d77d`，gzip、abort、密度控制和两项 fetch transition 已通过；剩余 8 项为两项 WebSocket writer 压力、两项 boundary 地址断言和四项 HEAD 关闭观测。当前源码已修正后两类，仍等待新的完整原生执行；writer 压力中真实 ACK 被观测前 JS 关闭计时器已触发的路径还须定位，不把推断写成已修复。[原始 macOS ZIP](evidence/v0.3.0/2026-11-12-model-head-boundary/parent-macos-original.zip)与[完整审计](evidence/v0.3.0/2026-11-12-model-head-boundary/parent-macos-complete-reviewed.json)保留全部失败。

同源 `ef53262` 的 [9 项完整 fuzz 审计](evidence/v0.3.0/2026-11-12-model-head-boundary/same-source-complete-fuzz.json)和[原始未截断归档](evidence/v0.3.0/2026-11-12-model-head-boundary/same-source-complete-fuzz-original.zip)已补齐；这份材料与先前只含截断 JUnit 的归档分开保留，不代表最终产品及依赖库的完整 sanitizer 接受。当前新源码的原 9 项也已独立发起。

最新 Linux 序列等待父候选按原计划完成。父候选当前已见的 6 项 HEAD 失败不会被删除；新快照使用独立源码和构建目录，不复用旧接受结论。四视口证据、R1–R9 和历史失败继续保留。正式密度/性能边界仍开放；已获授权的密度预热可按池大小扩大并发，必须逐 worker 验证冷至少 1 次、热至少 100 次后再采集 PSS，生产调度与 QPS 64 并发不变。

## 仍需完成

当前 `3a324a9` 三平台完整 ALL/矩阵与组合验收仍开放；新的独立 Linux production/LTO ALL、1108 项原全量与 171 项严格参考已串行排队，macOS 完整验证已启动。密度父候选 `f3a17a9` 的 Linux production/LTO 完整 ALL 已通过，实际注册 1107 项，未过滤全量已结束，1102 通过、5 个既有 WPT 失败、0 跳过，171 项必需参考已通过并获得严格兼容性接受；其 macOS 完整 ALL 与普通矩阵已结束，730 通过、16 失败、11 个声明的平台跳过。最新回收候选的三平台原生定向复测与 fuzz 均已通过并单独记录，未覆盖或中止父快照的完整证据。最终候选完整 ASan/TSan/UBSan、RSS 与资源入口仍须重验。新增待修项目包括两个控制测试的原预算超时、强制回收修复的最终全量覆盖及 Darwin 出站 ACCEPT/ACK 时序的全矩阵复核；旧快照所有失败未删除。ABI/旧接口、分配故障、长时间组合运行也需与最终源码对应。R1/R2/R4 仅完成各自定向要求，R3/R5/R6/R7/R8 仍在推进，R9 未完成。

正式 QPS/cold/density 仍需同一安静会话、候选/v0.2.2/pinned workerd 对照和完整原始证据。密度继续要求全部后代 PSS、2048 MiB、两次相等正读数及退出证据。性能页历史数据保持 unverified，1280/960/820/600 四视口已有真实截图与布局检查，正式数据载入后再验展示，用户无需连接浏览器。

[本轮完整证据索引](evidence/v0.3.0/2026-11-12-model-head-boundary/index.json)记录各归档和当前文档、账本的校验值。
