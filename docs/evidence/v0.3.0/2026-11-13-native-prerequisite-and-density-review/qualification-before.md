# v0.3.0 当前验收与兼容边界

更新时间：2026-11-13T00:50:37.502324+00:00。**v0.3.0 尚未完成全平台、完整资源及正式性能验收。** 当前冻结候选为 `065284f2d62123d9d5147d1235d267d023078b0e`，源码树 `f3b1f8e0d110e8d9474a8c1a593a0cabb6297ef5`，含 5158 份源文件、SDK78；除独立更新的本文外，5157 份冻结输入与工作区逐字节一致。工作区 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未推送主分支、创建标签或发布。

本轮已合入两项修正：`.gitattributes` 保留固定模型、原始 JSON、Streams bundle、补丁和许可证的原始字节，防止 Windows `core.autocrlf` 改写受 SHA 约束的输入；密度生产器合成测试从旧的 64 并发元数据改为现行 128×池大小。没有归一化原始证据、修改固定摘要或放宽断言。实际 `core.autocrlf=true` 新检出下 112 项契约/Streams 控制全部通过；Linux 密度生产器 5/5、密度控制 22/22 通过。生产运行时与调度代码未变。

上一候选 `56ba916` 的完整 CI `43275284036` 已自然结束，8 份原始归档逐一核对摘要，完整 job 日志亦已保存。macOS 为 759 项中 748 通过、0 失败、11 跳过；Windows workerd 对照为 134/134，通过了本轮 CRC 复位契约。Windows 普通矩阵为 757 项中 723 通过、26 失败、8 跳过，其中 25 项在字节约束检查失败，另一项 `spawn_failure_recovery` 报告句柄从 97 增至 139，仍须独立调查。Linux Release 为 1113 项中 1103 通过、6 失败、4 跳过；ASan/UBSan/TSan 分别为 1018/1017/1006 项，每个矩阵唯一额外失败均为旧密度合成元数据。原 5 项 WPT 差异、instrumented RSS 失败以及严格接受器的拒绝全部保留，未改记为通过。新候选的完整平台/检测配置 CI `43337806862` 正在运行。

新候选使用原版 fuzz harness，在同源码树的独立任务 `43337991791` 中完成 9/9：7 个随机目标各 10000 次，共 70000 次；27 个编译单元的 fuzzer/address/undefined 标记、原 60 秒 CTest 和各输入限制均已核对。frame 本轮耗时 53.7504 秒，实际 7 个二进制、运行前后语料及完整 LastTest 均已归档；原 60 秒超时的根因仍未确定。分配优化私有提案没有显示稳定加速，未合入。完整任务中新增的首版 fuzz 归档脚本因误处理仓库外 Python 路径而失败，属于采集准备失败；独立补验修正该脚本并执行了原始全部 9 项，没有修改产品测试。

父候选 `14fbabb` 的 1800 秒持续运行已实际结束，完成 4342 轮、69472 次回显、2171 次 worker 替换，结束时通道、活动请求、拨号、队列和 HTTP 计数均归零，正常退出。该结果属于父候选。本地 `56ba916` 的 1113 项完整 Release 测试执行后因上述额外密度失败，后续严格接受和 sanitizer 队列正确停止。新候选 13 个冻结驱动和完整对象包已重新准备，本地控制器 PID 421 已确认运行，依次执行 ALL、未过滤全量、严格 171 项、ASan/UBSan/TSan、资源、时序及 30 分钟持续运行，尚未声称完成。

密度验收仍有实质缺口：实际默认 `RLIMIT_NOFILE=10240` 低于 128-worker 热预热所需的 16384 条连接；单个源地址在第 28232 条连接报端口不足。私有客户端方案在不改变生产调度、fixture 时序、请求上限和 300 秒期限的前提下，通过了 32768 个真实 HTTP 响应的独立传输对照，并验证了子进程描述符额度继承和异常恢复。真实 128-worker 冷态曾通过，但热态未通过：NTFS 日志输出出现计数断号；原生 ext4 的并行发包方案耗尽 409600 次正确响应后，仍只观察到 108 个 worker，已观察者最少 1 次。128 个 READY 事件均在首条计数前发生，故不能归因为池尚未 READY。原始计数逐一连续、合计与响应数一致，失败后零残留 PID，未采 PSS。这些提案均未合入，65 实例的旧通过结果不能证明最大密度；正式性能点仍为 0。

[本轮完整检查点与原始记录](evidence/v0.3.0/2026-11-13-checkout-and-density-capacity-review/checkpoint.json)列出通过、失败、准备错误及仍运行的任务。以下历史分节保留其候选身份；全部 R1–R9 与最终同源完整组合仍是验收范围。

Windows 句柄增量已取得独立系统 API 对照：不加载 Capsid 的原生程序按三种顺序直接调用 CreateProcess，首个失败调用新增 44–47 个句柄，后续四次同类失败均回到相同计数。该证据支持“首次失败 API 的系统初始化前提”解释，尚不代替原 Capsid 失败的重新验收；独立见证使用缺失目录错误 3，后续 C ABI 提案仍须在实际缺失文件错误 2 下验证。私有分支 `codex/v030-failed-spawn-prerequisite-r75` 已启动：只补原生系统 API 控制，保留全部 25 次正常、25 次拒绝、5 次缺失和一次恢复、原 60 秒及精确句柄基线；须连续三次通过，并让一个真实未关闭的失败路径描述符产生 +1 句柄而被拒绝，才允许合入。[独立原始对照与审校](evidence/v0.3.0/2026-11-13-checkout-and-density-capacity-review/native-os-independent-review.json)。

本轮同源码平台报告已完成独立审校（2026-11-13T02:58:12.607187+00:00）：Release 普通矩阵 1113 项，1104 通过、5 项原始 WPT 差异、4 项按实际注册返回 77 跳过；严格 171 项 workerd 全部通过，版本化兼容接受通过，原始普通套件仍不声称全通过。ASan 为 1018 项中 1013 通过、0 失败、5 跳过；UBSan 为 1017 项中 1013 通过、0 失败、4 跳过；TSan 为 1006 项中 1001 通过、0 失败、5 跳过。每个检测配置的 12 个实际运行时编译单元均已核对 sanitizer 标志。ASan/TSan 各两项完整资源场景均通过并正常退出，ASan 保留完整泄漏检测，TSan 不声称 LSan；原始 instrumented RSS 失败保持原样。macOS 为 759 项中 748 通过、0 失败、11 跳过。实际 JUnit 名称与注册逐一相符，跳过均核对原 77 声明。原接受器在 runner 上核对了执行二进制，Release 包的三个实际二进制已另行重算摘要；sanitizer artifact 未包含二进制本体，独立审校未冒称已重算其 ELF。Windows 和最终整组仍未完成。

同候选本地 Release/LTO ALL 与未过滤 1113 项已结束，1108 通过、5 项原 WPT 差异、零跳过；严格 171 项和完整版本化接受随后通过。控制器 PID 421 仍按原序列执行完整 ASan/UBSan、TSan、资源、时序与 1800 秒持续运行，未打断或修改其输入。Windows 回收补验首版因传给 Python 的命令行过长而在执行测试前失败，原始报告和完整日志均保存，实际 C ABI 验收数为 0；修正为写入 UTF-8 脚本文件后重新执行，断言、场景数和 60 秒均未变。

另有只改客户端 HTTP/1.1 pipeline 的私有密度方案，生产调度、fixture 时序、每 worker 计数、请求上限及 300 秒均不变。五项客户端控制覆盖合并响应缓冲、混合目的端口、错误中间状态/正文和截断；已启动使用本轮已验证生产包的 65/128/256 实例共十组真实冷/热验收。首个远端任务在新控制脚本编码准备环节失败，实际 worker 验收数为 0；已独立解码并核对三个载荷后重新启动，尚未合入或声称密度通过。[本轮完整平台与本地检查点](evidence/v0.3.0/2026-11-13-current-platform-matrices/checkpoint.json)保留这些边界。正式三条性能线、最大密度、最终四视口及 R1–R9 仍未完成，正式点为 0。

## 全部要求及当前状态

完整要求、状态和历史引用保存在[原始要求账本](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/requirements.json)和[更新前完整账本](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/goal-ledger-before.json)。下表保留 R1–R9；定向完成项仍须与最终组合对应。

| 要求 | 范围 | 状态 |
|---|---|---|
| R1 | 标准字节流部分/未对齐 EOF、零 EOF backing、高 minimum 取消及 36 个延迟组合 | 定向要求完成 |
| R2 | BigInt limb 边界、转换顺序、分配限制、独立回归及上游来源 | 定向要求完成 |
| R3 | 真实 native fetch body 参数、released/detached/invalid view、全部 view/构造路径、重试/数据/owner/credit/退出、并发/context/abort/intrinsics | 组合验收进行中 |
| R4 | 真实非零 C ABI response-body 限额经 Upgrade，known/unknown 长度及 below/equal/above、Default/BYOB、取消与回收 | 定向要求完成 |
| R5 | 普通 fetch 元数据及 tee/clone/values/pipe 分支、资源与 reader 取消归属图 | 组合验收进行中 |
| R6 | DefaultReader 精确 chunk 序列/时序、额度控制、Linux rich-JSON invalid IPC frame 根因 | 组合验收进行中 |
| R7 | 历史 15 个失败组合、真实跳过前提、原始失败与 WPT/profile 证据 | 组合验收进行中 |
| R8 | 干净 Debug/Release/Linux LTO/CI、依赖 instrumentation、fuzz、持续运行、正式性能/密度及四视口 | 尚未完成 |
| R9 | 完成 R1–R8、冻结完整候选并执行最终组合接受；发布仍需授权 | 尚未完成 |

## 本轮已验证修正

冷启动工具在公开 C ABI 回收直接 worker 后，再回收自身收养的后代，成功样本须在清理完成后输出。真实 fork/clone 包装器随后 exec 原生 worker，覆盖 SIGINT、SIGTERM、正常输出、错误响应和超时。原断言继续在外围夹具清理之前检查整树。GCC ASan、Clang ASan/UBSan、Release 各通过原 7 项及全部 10 个后代场景，旧实现各被 10 个断言拒绝；新增两项正常场景各自保留 30 秒测试预算。

Clang 空闲 PSS 的映射对照中，默认一秒静置期间总增量 764 KiB，其中 680 KiB 落在 FakeStack 映射。同一 worker 保留默认 instrumentation、静置 20 秒后取得两次相等正 PSS。因此只为 Clang ASan 功能测试配置 20 秒采样前静置；原 100 秒 CTest、两次完全相等、最多 8 次采样、采样间隔及取消期限保留。Clang 原 7 项实际 65.7 秒，两项正常后代各 21.4 秒。关闭 FakeStack 的诊断对照不计验收，生产 cold 默认参数和正式测量协议保持原值。

三种构建的 4 个 CTest 均通过。临时 CI 的后置收集器因 unittest 进度点与 JSON 行交错而失败；原失败记录保留，独立复核恢复全部 10 行及正负控制，没有重写原结果。[三个构建审计](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/cold-three-profile-review.json)和[原始内存映射对照](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/fake-stack-mapping-review.json)可复核。

真实 Ubuntu runner 上安装并选择实际 perf，四个软件计数器探测通过；构建真实 Host/worker 后，原 A/B 证据控制在原 600 秒预算内输出 PASS。CI 复用该已执行的 helper，完整 sanitizer 矩阵另保存原始场景 sidecar，测试选择和单项时限不变。这不是产品性能点。[实际 perf 前提](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/actual-perf-prerequisite.json)、[控制输出](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/actual-perf-controls.stdout.log)和[真实程序摘要](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/actual-perf-identity-fixtures.sha256)保留。

## 完整结果与未通过项

下列完整结果属于父源码 `63a3cc1`，不替代本轮新组合：Linux Release/LTO 完整 ALL 通过；未过滤 1110 项为 1105 通过、0 跳过、5 项原 WPT 差异。同源严格 workerd 171 项全部通过并取得版本化兼容接受，原 5 项差异与 4 项超时保留，ordinarySuitePassed 和 standardsConformanceClaimed 仍为 false。[完整接受记录](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/parent-linux-compatibility-acceptance.json)保留输入绑定。

父源码 UBSan 为 1009 通过、0 失败、5 个原声明跳过；9 项 fuzz、7×10000=70000 次及 27 个相关编译单元已审计。Alpine 3.24 原 Docker 发布配方的 9 项原生/包消费测试通过，3 个归档程序独立解析 ELF 后均无解释器或共享库依赖；这不等于完整 musl 全量或参考接受。完整原始归档及 SHA256 仍在此前日期目录和工作区保存，[此前完整审校历史](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/qualification-before.md)逐字节归档。

父源码 ASan 1015 项中 1008 通过、1 失败、6 跳过，失败为取消冷启动后的僵尸后代；本轮已独立修正，尚需新组合完整验证。TSan 1003 项中 994 通过、3 失败、6 跳过，失败为 native_fetch_large_gzip 的辅助状态观测及 managed 的 native-reader-lifecycle、reader-scope 完整采集/清理；[TSan 原始审计](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/parent-tsan-review.json)未改写。保留原源码、依赖构建、测试与时限的补采任务已完成，3 轮各 3 项均通过并保存完整 sidecar；这些定向通过未当作原全量失败的修复。

Windows 原 workerd 134 项为 133 通过、1 失败、0 跳过，失败发生在 pinned workerd 的 codec-supp-fragment-crc-late 源 EOF 观测。同输入、同二进制的 3 次复查各 31 场景通过，未当作修复；[逐次记录](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/windows-same-input-replays.json)保留。macOS 父候选 fa40606 曾完成 758 项、747 通过、11 个原声明跳过；63a3cc1 首次构建通过后，临时采集器误调用 Ninja 导致测试未启动；修正采集方式、保留原 Unix Makefiles 生成器和测试预算后，已完成 759 项、748 通过、0 失败、11 个原声明跳过，并保存 275 份原始 driver report。当前 14fbabb 的独立 macOS 矩阵也已完成并审计：759 项、748 通过、0 失败、11 个原声明跳过，275 份原始 driver report 保留。

## 剩余执行与正式协议

父源码本地 Clang ASan/UBSan 全量已自然结束并保留全部失败；未启动的旧 TSan/资源计划已交接到当前候选，当前干净 Linux Release/LTO ALL 已完成，完整原始测试与后续资源验收继续串行执行。必须先解决上述 TSan 和 Windows 原参考失败，并完成当前源码原生平台、依赖 instrumentation、资源回收、时序、ABI/旧接口、分配故障及 1800 秒持续运行，再取得最终组合接受。

正式测量仍需同一安静会话的候选、v0.2.2 和 pinned workerd 对照及全部原始输入/输出。QPS 保持 64 并发；cold 保留原尺寸、轮次和 warm 50；密度保留 2048 MiB、全部后代 PSS、两次相等正读数与退出证据。已批准按池规模扩大密度预热并发，逐 worker 验证冷至少 1 次、热至少 100 次后采样，生产调度不变。[父候选三条正式命令输入核对](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/parent-formal-input-plan.json)属于准备，正式性能点仍为 0。

1280/960/820/600 四视口已有历史真实截图，正式数据载入后须再次验收。全部 R1–R9 完成并与同一个最终候选绑定后才能宣布版本完成；不自动发布。

父候选 `14fbabb` 的[完整 CI](https://github.com/ErosZy/capsid/actions/runs/43232371737)已结束且未通过，精确 checkout `704d1c864202dda18f1d9dc00e22c9a182686ea2` 的树与该父候选相同；所有原始产品任务、单项预算与最终汇总规则保留。[本轮源码与验收检查点](evidence/v0.3.0/2026-11-12-cold-settle-integration/checkpoint.json)区分组件通过、父候选结果和当前待验项。

当前 `14fbabb` 的 9 项 fuzz 已完成独立审计：7 个随机目标各真实完成 10000 次，合计 70000 次；27 个编译单元、原 60 秒 CTest、单输入 2 秒、64 KiB 输入及 512 MiB RSS 上限均保留。该流水线其他任务最终未全部通过。[当前 fuzz 审计](evidence/v0.3.0/2026-11-12-cold-settle-integration/current-fuzz.json)与[原始归档](evidence/v0.3.0/2026-11-12-cold-settle-integration/current-fuzz-original.zip)可复核。

本地父源码 `63a3cc1` 的另一组完整 Clang ASan/UBSan 结果为 1114 项执行、1052 通过、62 失败、0 跳过，原始结果没有转换为通过。其中 56 项属于 managed 场景；旧临时目录下典型 admin socket 路径为 114 字节，超过 Linux 108 字节容量。同一原始 `host_ws_upgrade_gates_managed` 命令、同一二进制与原 45 秒预算在长目录失败、短目录通过，完整源文件、依赖、CTest 定义和 instrumentation 选项未变。该因果对照只证明这个启动前提，56 项仍须由当前候选完整重验；原 5 项 WPT 差异和 1 项旧 cold PSS 失败保留。[独立复核及原始结果摘要](evidence/v0.3.0/2026-11-12-current-local-handoff/parent-local-asan-review.json)、[完整原始归档](evidence/v0.3.0/2026-11-12-current-local-handoff/parent-local-asan-original.zip)和[长短目录因果对照](evidence/v0.3.0/2026-11-12-current-local-handoff/same-test-long-short-controls.json)可复核。

父候选 `14fbabb` 已在全新 Linux checkout 验证 5155 份源文件，并完成生产 Release/LTO ALL、12 个 LTO runtime 编译单元与 1113 项真实注册。只有等待中的控制器收到信号，原运行测试和本轮 ALL 均自然结束；后续未启动脚本使用独立短临时目录，原断言、测试选择、instrumentation 和时限保留。新的三个 20 秒 Windows QPC/WSL 时钟窗口全部通过。当前完整测试、严格 171 项对照、完整 ASan/UBSan/TSan、资源、时序和 1800 秒持续运行仍需全部通过；正式测量点仍为 0。[当前本地验收检查点](evidence/v0.3.0/2026-11-12-current-local-handoff/checkpoint.json)记录同源构建身份与交接范围。

当前 `14fbabb` 的正式输入准备已实际完成：15 份 cold fixture 摘要、11 个真实二进制身份、5 组官方 tag/ref 及三条真实 CLI 参数解析全部核对。候选 Host/worker 摘要绑定本轮 Release/LTO ALL；尚未建立安静会话，未创建会话 metadata，也未运行正式测量。[当前输入与命令检查](evidence/v0.3.0/2026-11-12-current-local-handoff/current-formal-cli-plan-check.json)属于准备证据，正式性能点仍为 0。

父候选 macOS 补验的 41,075,216 字节原始归档已按 GitHub artifact 摘要核对，JUnit 全部 759 个名称与实际注册逐一相符，11 个跳过项保留各自原始声明。编译数据库和 Makefiles 链接命令均已采集；首次采集器失败记录仍保留。[macOS 完整复核](evidence/v0.3.0/2026-11-12-current-local-handoff/parent-macos-complete-review.json)与[原始归档](evidence/v0.3.0/2026-11-12-current-local-handoff/parent-macos-original.zip)属于父候选证据。

当前 `14fbabb` 的本地 Release/LTO 未过滤全量已实际结束：1113 项执行、1108 通过、5 项原 WPT 差异、0 跳过，严格 workerd 组合接受随后已完成。该候选 macOS 的 759 项也已独立审计，748 通过、0 失败、11 个原声明跳过；本轮记录与父候选补验区分保存。

同源 hosted Release 的结果为 1113 项执行、1099 通过、10 失败、4 个原 sandbox 声明跳过。原 A/B 证据控制和真实 perf 前提已通过，严格 171 项也全部通过；另外 5 个失败是两项 retired ownership 参考采集与三个 PSS/控制器测试，因此完整版本化接受仍被正确拒绝。ASan 的 1018 项中有 5 项失败，UBSan 的 1017 项中有 4 项失败，涉及控制器/PSS、原 30 秒参数契约控制及 managed reflect-standard 完整采集。报告目录中的 POSIX 冒号又导致这两个归档上传失败；完整原始 job log 已保留，已启动只修正归档方式的三种完整 sanitizer 重采，代码仍为父候选 `14fbabb`。[本轮完整结果及边界](evidence/v0.3.0/2026-11-12-current-matrix-findings/checkpoint.json)保留这些失败，不以组件通过替代组合结果。

Windows 当前完整参考门仍为 134 项、133 通过、1 失败、0 跳过，重复发生在 pinned workerd 的 codec-supp-fragment-crc-late 实际源 EOF 观察；97 字节解码与 Gzip 错误状态均被原始报告保留，尚未证明修复。该参考关闭问题需继续实测诊断。

历史检查点（合入前）：私有提案 `1fb5e29` 保留全部原 native 公共状态/真实上游断言、benchmark 程序与原测试时限，仅修正参考监听端口、8 项 CTest 调度属性及报告打包方式，尚未合入工作区。真实 pinned workerd 的占位端口对照中，旧 TCP 就绪检查接受外部服务并收到 201，新子进程控制通道选择实际监听端口并取得 200；原 48 项和新增 7 项回归控制全部通过。带冒号路径、符号链接和一份真实保留报告的归档读回也通过。四种构建的 12 项原测试各 3 轮观察已启动，结果仍待审计；内部双控制器并发场景保持不变。[实际端口对照](evidence/v0.3.0/2026-11-12-current-matrix-findings/actual-pinned-workerd-port-control.json)和[私有修改](evidence/v0.3.0/2026-11-12-current-matrix-findings/private-port-and-isolation-candidate.diff)可复核。正式性能点仍为 0。

`1fb5e29` 的四种构建定向结果现已全部独立审计并合入工作区：Release/LTO、GCC ASan、GCC UBSan、Clang TSan 各 3 轮、每轮 12 项原 CTest，共 144 项通过、0 失败、0 跳过；同时保留 1272 个 managed BYOB 和 144 个 native retired ownership 原场景。实际注册确认 8 项 RUN_SERIAL 属性，原时限、真实 PSS 判据、内部双控制器并发和所有原语义断言保留。工作区的 55 项 native/监听控制和 workflow 契约也通过。[同源合入记录](evidence/v0.3.0/2026-11-12-port-and-pss-integration/integration.json)区分定向资格与尚未完成的整版验收。

父候选 `14fbabb` 的本地 1113 项未过滤全量及严格 171 项已完成版本化组合接受，原 5 项 WPT 差异与 4 项历史超时仍保留；该结果不替代新源码的完整组合。当前同源 hosted TSan 的 1006 项中仍有 1 项 native_fetch_large_gzip 状态采集失败，报告上传也被 POSIX 冒号拒绝；原日志与 64 KiB 截断原行保留，完整原侧录待三种 sanitizer 重采返回。Windows 的 4 次隔离观察均通过，但原完整门两次 CRC-late 源 EOF 失败仍未解释，状态查询时序调整仅用于诊断，未合入。完整新候选平台、instrumentation、资源、时序、1800 秒持续运行、正式性能三条 lane 与四视口仍需验收，正式性能点为 0。


最新补采已独立核对完整 ZIP 与内部 tar 摘要：父候选 `14fbabb` 的 ASan 1018 项为 1012 通过、1 项原 30 秒参数契约控制超时、5 项有原始原因的声明跳过；另行保留两项原 RSS 失败，未声称资源接受。UBSan 1017 项为 1010 通过、3 项 PSS/控制器失败、4 项声明跳过。两种构建的 managed reflect-standard 各 106 个原场景与两组大 gzip 场景均通过。真实含冒号的运行目录及符号链接已完整进入归档，报告收集修正已由完整任务验证。这些是旧源码补采结果，不替代 `1fb5e29` 的整版验收。[最新检查点](evidence/v0.3.0/2026-11-13-progress-status/checkpoint.json)、[ASan 完整复核](evidence/v0.3.0/2026-11-13-progress-status/recovered-asan-review.json)和[UBSan 完整复核](evidence/v0.3.0/2026-11-13-progress-status/recovered-ubsan-review.json)保留全部原失败。

Windows 隔离诊断的 4 次 31 场景均通过，其中原观察顺序也两次通过，因此未证明改动查询顺序是修复。新的完整 134 项诊断以新候选的实际代码构建，仅在失败清理之前补采上游状态；原断言、3 秒 EOF 观察及全部预算保持，诊断不计最终资格。本地父候选 Clang ASan/UBSan 的完整 1117 项仍在串行执行，云端完整 TSan 补采仍待结束；最新状态记录保存实际 PID 和 run ID。最终仍需新候选全部平台、资源与时序、1800 秒持续运行、同会话正式 cold/QPS/密度及四视口，正式性能点为 0。


R6 的历史 IPC 根因审计备份已重新定位，文件 SHA256 与原账本的 `39ea538b…` 完全相同，原审计程序摘要也匹配。该记录包含原始 Host/worker 摘要及原始、条件修正、恢复三组对照：已经通过 registry 验证且标为 discard 的 DATA 在旧 handler 落入拒绝分支，导致 503/invalid IPC；仅修正条件跳转后，原 14 场景通过，恢复后再次失败。当前源码仍保留已验证 DATA 的允许路径和无效帧拒绝检查。原审计记录的副本现可读取，但此前获准清理的原始追踪及二进制未在本轮重新验证；这不是新的运行证明，也不关闭 R6 的精确 chunk/时序与最终组合要求。[历史审计副本](evidence/v0.3.0/2026-11-13-historical-ipc-reference/original-audit-snapshot.json)与[本轮可用范围复核](evidence/v0.3.0/2026-11-13-historical-ipc-reference/review.json)明确区分两者。


父候选 `14fbabb` 的完整 TSan 补采现已自然结束并独立审计：1006 项实际选择名称与注册逐项相符，1001 通过、0 失败、5 项原声明跳过。8 MiB 与 96 MiB gzip 的完整原始 JSON 均通过未改动的严格契约，分别解码 8388608 与 100663296 字节，源端 EOF 在清理前观察到，worker 正常退出；managed reflect-standard 的 106 个场景也通过完整契约。独立 TSan 资源接受的输入摘要、同一实际 Host/worker、运行时标记和正常退出均复核通过；原 reclamation-idle RSS 失败继续保留，生产 RSS 和完整 ASan/LSan 仍各自要求。此前完整任务的 missing-state 失败未被改写，也未宣称已找到原因；这次完整通过属于父候选，最新 `1fb5e29` 的最终整版验证仍待执行。[完整补采检查点](evidence/v0.3.0/2026-11-13-complete-tsan-recovery/checkpoint.json)、[注册及资源复核](evidence/v0.3.0/2026-11-13-complete-tsan-recovery/complete-matrix-review.json)和[完整语义契约复核](evidence/v0.3.0/2026-11-13-complete-tsan-recovery/full-contract-review.json)保留准确范围。三种 sanitizer 的补采任务均已结束，ASan 的原 30 秒控制超时和 UBSan 的三项 PSS/控制器失败仍保留；Windows 完整诊断与本地完整 Clang ASan/UBSan 仍在执行。


首次当前 `1fb5e29` 独立密度预热观察的派发检查点如下，后续结果及观测器修正见末段。实际 checkout 的树与冻结候选一致：将分别验证 capsid 的 65 个 worker 和 workerd 的 65 个 cell，冷至少 1 次、热至少 100 次后，再取得整树两次相等正 PSS 和零残留 PID。保留原 300 秒覆盖期限、冷最多 520 次和热最多 208000 次请求；65 个实例的预热并发上限为已批准的 4160，QPS 仍为 64。此前 65 实例冷/热覆盖与 PSS 通过记录属于 `2979e33` 二进制和较早脚本，当前脚本摘要已不同，故未当作当前候选结果。新任务属于工具与预热资格观察，尚无结果，也不计正式性能点或最大密度。[当前源码任务及范围](evidence/v0.3.0/2026-11-13-current-density65-dispatch/checkpoint.json)与[实际派发记录](evidence/v0.3.0/2026-11-13-current-density65-dispatch/current-density65-dispatch.json)保留输入绑定。

Windows 原完整 134 项诊断仍在执行。独立真实 TCP 子进程对照确认，FIN 会触发 Node `end`，复位会触发 `ECONNRESET` 和 `close` 而没有 `end`；该对照未改变夹具主动结束或销毁状态。锁定 workerd 源码中的解压错误与 reader 错误传播路径也已读取，但二者都不足以证明本次 workerd 失败的具体传输终止方式。必须继续检查原完整任务的清理前上游侧录，codec 断言和全部原时限尚未修改。


首次当前源码密度任务 `43260447861` 已自然结束，完整日志与归档均保留。冷阶段实际 65 个 worker 均收到请求，520 个响应全部正确，逐 worker 最少 1 次，整树 PSS 为两次相等的 197993 KiB，清理后零残留 PID；原始计数日志已独立重解析并与结果逐项核对。随后私有观测脚本错误地要求启动参数存在 `--strict`，而实际工厂使用 `--mode managed`，因此任务失败、其余三个阶段未执行；没有将其声明为四阶段通过，也没有中断原测试。修正仅涉及 CI 观测断言，真实归档命令通过，错误模式和参数两项反例均被拒绝。修正版 `43262279072` 已以同一冻结源码树重新派发四个阶段，原请求上限、300 秒期限、PSS 与退出要求保留，结果仍待收集，正式性能点仍为 0。[原始失败及冷阶段复核](evidence/v0.3.0/2026-11-13-density65-observer-correction/original-observer-review.json)、[修正版预检](evidence/v0.3.0/2026-11-13-density65-observer-correction/corrected-observer-preflight.json)和[当前检查点](evidence/v0.3.0/2026-11-13-density65-observer-correction/checkpoint.json)明确区分这些范围。


私有提案 `bc61dbe` 只修正 CRC 完整错误响应后的已观察复位分支、补采失败侧录及预热并发上限。CRC 之外仍要求原 FIN；源端全部分片、字节、端口、公开状态、清理前终止和原 3 秒观测器均保留。新增控制拒绝缺片、夹具主动销毁、错误端口、无复位及其他 codec 场景。密度提案仅将临时并发改为 `max(64, 128 * pool_count)`，不改 fixture、生产调度、QPS64、请求总上限或 300 秒期限；另保留真实 208000 响应却不足 100 次/worker 的反例。该较大波次仍待实际验证。[提案差异](evidence/v0.3.0/2026-11-13-reset-density-and-local-asan-findings/private-proposal.diff)和[全部本地控制](evidence/v0.3.0/2026-11-13-reset-density-and-local-asan-findings/private-proposal.json)可复核。

同一私有树的任务 `43270591475` 已派发：Windows 用原完整任务中摘要核对的真实生产二进制执行两轮各三种模式的 31 个 metadata 场景，共 186 项，原单项 120 秒预算不变；这属于新验收器与已保存二进制的组件验证，不是新源码完整 ALL。Linux 重新构建该候选，执行 65 个 worker/cell 的四个冷热阶段。工作区仍为 `1fb5e29`；旧源码完整 TSan 的 1105 项继续运行，后续资源、时序、1800 秒持续运行及最终同源全平台和正式性能要求保留。
