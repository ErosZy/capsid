# v0.3.0 当前验收与兼容边界

更新时间：2026-11-11T20:37:17.051156+00:00。**v0.3.0 尚未完成全平台和正式性能验收。** 当前冻结候选为 `b52f5075a7c2579b6b48436b0a7686a687a4ab24`，5130 份源文件，SDK78（78 份补丁）。工作区代码与冻结清单逐字节一致，独立更新的本文除外。主工作区 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`；未更新主分支、标签、Release 或 Pages。

[本轮证据索引](evidence/v0.3.0/2026-11-12-native-request-targets/index.json)、[候选身份](evidence/v0.3.0/2026-11-12-native-request-targets/candidate.json)和[验收检查点](evidence/v0.3.0/2026-11-12-native-request-targets/checkpoint.json)给出哈希和实际执行范围。[上一版本文](evidence/v0.3.0/2026-11-12-native-request-targets/qualification-before.md)保留之前的详细记录；全部 R1–R9 要求及历次证据仍保存在工作区 `current-goal-requirements.json`，未裁剪。

## 当前实际结果

| 对象 | 实际源/构建 | 已证实的结果 | 验收边界 |
| --- | --- | --- | --- |
| Windows 普通 Release 未过滤全量 | `7d393a0` | 845 项：832 通过、5 项既有 WPT 失败、8 项声明的平台跳过；无额外 reader-scope 失败 | 严格参考流程状态为 `mandatory-platform-reference-gate-running`；仍保留原始失败/跳过 |
| Linux LTO/production 未过滤全量 | `7d393a0` | 1101 项：1096 通过、5 项既有 WPT 失败、0 跳过；171 项必需参考 gate 及完整兼容性审计通过 | ordinarySuitePassed=false、standardsConformanceClaimed=false；五份标准文件及原子项超时均保留 |
| macOS 干净 ALL 与受影响用例 | `2ee13a5` 的等树提交 `358499c` | 71 项通过、零跳过；旧失败中的 52 项均实际执行；24 份 Darwin 参考报告覆盖 160 个真实差分场景 | 属于前置修正的定向验收；完整 macOS 矩阵仍未通过 |
| 新 HTTP 目标修正 | 当前候选的两单元组件，Windows/Linux | 每平台 20 个场景、参考与产品两端共 40 次执行；各 152 个原有读场景通过 | 两平台共 80 次差分端执行、304 个原有读场景；当前候选的整套干净 ALL/全量仍待完成 |

完整矩阵的源、provider、运行时二进制、cache 和 CTest 定义前后哈希一致。它们证明实际父快照的结果，没有重标为本轮生产修正后的全量验收。[Linux 完整兼容性结果](evidence/v0.3.0/2026-11-12-native-request-targets/linux-parent-compatibility.json)与 [Windows 原始结果](evidence/v0.3.0/2026-11-12-native-request-targets/windows-parent-full.json)可逐项复核。

## 已集成的请求目标修正

真实旧 macOS 控制请求在 1023 字节处截断；Linux 诊断也观察到从 4096 字节起截成 4095 字节。native HTTP 用 `TJS_PATH_MAX` 临时数组拼请求目标，混入了平台文件路径限制；4 KiB 接收服务缓冲同时限制了请求头构造。

`0030` 现在按实际字节长度分配并拥有完整目标，保留前导斜线、路径和查询。`0065` 按已配置的 HTTP header budget 为出站握手构造提供临时空间，回调收到实际构造容量；接收服务窗口继续为 `4096 + LWS_PRE`，没有放大原生读机会。新增实际请求回归覆盖 1023/1024/1025、4095/4096/4097、8192 字节边界、长查询及保留字符转义，并在 inline/separate 两种 await 形式下运行同一份 workerd/产品程序，程序不含计时器。

两平台均完成原 78 份补丁的整栈准备，按原编译参数重新编译 HTTP client 和 LWS client 两个真实单元再链接 Worker；其他 archive 成员逐字节相同，原完整构建未修改。Linux 原实现、仅修路径、仅修构造空间三种反例都被拒绝；Windows 原实现也被拒绝。每次产品观察均要求源连接 FIN 先于 helper finish/Worker teardown，之后的源快照不得变化，实际 Worker OS 退出必须干净且未强制结束。[真实 Windows 对照](evidence/v0.3.0/2026-11-12-native-request-targets/windows-uri-real.json)、[真实 Linux 对照](evidence/v0.3.0/2026-11-12-native-request-targets/linux-uri-real.json)和[原/不完整修复反例](evidence/v0.3.0/2026-11-12-native-request-targets/linux-negative-controls.json)保留完整观察。

每平台七组原有测试的 152 个场景通过，包括 8 MiB/96 MiB gzip 的精确读块及 backing 模型，以及 raw/gzip overlap/cancel/abort 六个完整 shard。原时间预算、输入数据和断言未修改。[Windows](evidence/v0.3.0/2026-11-12-native-request-targets/windows-original-read-models.json)和[Linux](evidence/v0.3.0/2026-11-12-native-request-targets/linux-original-read-models.json)给出实际模型报告哈希。私有短控制的早期失败仍保留；后续原程序在四种环境和多组表达式对照中正常完成，未据此另改事件循环，也未在回归中加入 Worker 计时器来兜底。

仍须对出站短写做专门控制：目前临时 header buffer 在 `lws_ssl_capable_write` 后释放，正数短写及 TLS 延后写的所有权/续传边界尚未验收。下一步以真实 socket 短写的私有链接包装器复现，再核对标准 owned raw-write queue；本轮尚未集成该后续修正。

## macOS 前置修正与新任务

原 80 项失败中，23 项由不支持的 `ps -P` 首先抛错，24 项误选 Linux workerd 二进制锁，5 项把 SO_RCVBUF 的请求值当作恒定上限。此前已修的探针和元数据另占两项。[71 项原生复测](https://github.com/ErosZy/capsid/actions/runs/43088747329)及[完整归档](evidence/v0.3.0/2026-11-12-native-request-targets/macos-driver-original.zip)证实这 52 项均执行通过，故旧 80 项中仍有 26 项待后续原生验收，不能把整套 macOS 矩阵认定为通过。

新候选的 [43095214774](https://github.com/ErosZy/capsid/actions/runs/43095214774)首先在分配 runner 前发生 GitHub 临时服务启动错误，零 job、零测试、无构建产物。原记录完整保留；已对同一 workflow SHA、同一 code SHA 发出一次重试。最近实际状态为 `completed/startup_failure`；重试后仍未分配 job，尚未取得新候选的 macOS 测试结果。仓库、工作流和测试预算未因此更改；没有把启动失败算作测试失败或测试通过。

此前日志 flush 修正已在三平台通过八项实际组件用例，两平台共 800 个 reader-scope 场景通过；本轮两份完整矩阵也没有复现额外清理失败。[日志证据](evidence/v0.3.0/2026-11-12-log-stop/index.json)与[三平台/四视口证据](evidence/v0.3.0/2026-11-12-viewport-macos-log/index.json)保留实际快照身份。旧强制退出的精确原进程时序未追踪，仍保留原始失败。

## 尚未完成的工作

1. 完成出站短写/延后写的实际反例和所有权修正，继续验收新 HTTP 目标实现。
2. 收取本次原生 macOS 任务，继续审校剩余 26 项：长请求相关 pending/control、原生 EOF/reset、配额与 channel reservation 释放、outbound ACCEPT 时序，以及 mutation fixture、临时路径、资格矩阵的真实平台覆盖。
3. 完成最新最终候选的干净 ALL、未过滤全量、必需参考和完整兼容性验收。旧 Windows/Linux 全量及旧 macOS 定向结果保持其原源身份。
4. 完成最新候选的 ASan/TSan/UBSan 全矩阵与 RSS/资源入口，解决原预算 fuzz WebSocket frames 的 60s 问题，保留 10000 runs、65536 max length、单输入预算及全部独立 oracle/无效向量检查；完成 ABI/分配故障及组合检查。
5. 在同一个安静会话完成正式 QPS/cold/density 三车道。必须包括全部后代 PSS、2048 MiB 预算、连续两次相等正读数、逐 Worker 触达和完整退出证据。正式性能点仍为 **0**。

密度保持一个 App、生产 least-loaded/最低槽优先及 64 并发。八 Worker 的冷热阶段已逐个触达；65 READY Worker 的既有观察仍只触达 49 个，不能验收。仅预热阶段是否允许扩大并发的选择仍待回复，当前没有扩大，也没有改生产调度。

性能页当前历史数据明确标为 unverified。1280/960/820/600 四视口的真实顶部/整页截图与布局已验收；正式 0.3.0 数据加载后仍需检查展示。用户无需连接浏览器。R1/R2/R4 已有定向完成记录；R3/R5/R6/R7/R8 的最终组合和 R9 仍开放。
