# v0.3.0 当前验收与兼容边界

更新时间：2026-11-11T21:21:07.603411+00:00。**v0.3.0 尚未完成全平台和正式性能验收。** 当前冻结候选为 `2979e33b7b21909624ea571de0fc0acf10ef3009`，5131 份源文件，SDK78。冻结清单中的代码与工作区逐字节一致；本文和追加证据独立更新。主工作区 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未更新主分支、标签、Release 或 Pages。

[本轮证据索引](evidence/v0.3.0/2026-11-12-queued-http-handshake/index.json)、[候选身份](evidence/v0.3.0/2026-11-12-queued-http-handshake/candidate.json)、[验收检查点](evidence/v0.3.0/2026-11-12-queued-http-handshake/checkpoint.json)和[修改前状态](evidence/v0.3.0/2026-11-12-queued-http-handshake/qualification-before.md)保留完整范围。全部 R1–R9 要求及历史仍保存在工作区 `current-goal-requirements.json`，未裁剪。

## 本轮修复及实际验证

真实 socket 控制复现了旧握手的数据丢失：请求写入 104 字节，实际只发送 52 字节，旧实现随即释放了未发送部分。`0065` 现在通过标准 `lws_issue_raw` 队列拥有并续传剩余字节，再释放临时构造缓冲。原有请求目标修复继续保留完整路径、查询及转义；接收服务窗口仍为 `4096 + LWS_PRE`，生产调度未修改。

| 验证 | 实际结果 | 边界 |
| --- | --- | --- |
| 永久原生短写回归 | normal/short × 64/8192 共 4 项通过；旧实现的 2 项正常对照通过、2 项短写明确失败 | GNU ELF 测试链接包装器，实际发送前缀；无生产测试钩子 |
| 完整 Worker 与 pinned workerd 差分 | 强制真实正数短写的 20 场景、40 次端执行通过；源连接 FIN 和实际 Worker 退出断言保留 | 对应两单元组件，未重标为当前候选完整构建 |
| 实际 TLS | CA 与 TLS 1.2 RSA-PSS 两组原用例在强制真实短写下通过，旧实现失败 | CA/主机名反例、连接复用及原 30 秒预算保留；未声称覆盖实际零字节 WANT_READ/WANT_WRITE 状态 |
| 原有读模型 | Windows/Linux 各 152 个场景通过，共 304 个 | 8/96 MiB gzip 及 raw/gzip overlap/cancel/abort 六 shard；预算、输入、断言未改 |
| 资源观测 | 实际 POSIX 回收测试通过；描述符/子进程恒零反例各被拒绝 | Linux 原生执行；Darwin 分支仍待原生编译和运行 |
| 临时目录与隔离夹具 | 祖先目录别名下旧夹具失败，修正后 20 项契约通过；实际 mutation 基线通过、35 个故障变体均被预期断言拒绝 | 归档路径校验未放宽；隔离夹具补齐 Darwin 两份 workerd 锁 |

[永久及 Worker 短写记录](evidence/v0.3.0/2026-11-12-queued-http-handshake/short-write-permanent-and-worker.json)、[真实差分](evidence/v0.3.0/2026-11-12-queued-http-handshake/real-worker-short-writes.json)、[TLS](evidence/v0.3.0/2026-11-12-queued-http-handshake/linux-tls.json)、[资源/路径控制](evidence/v0.3.0/2026-11-12-queued-http-handshake/posix-recovery-and-temp.json)与[夹具故障控制](evidence/v0.3.0/2026-11-12-queued-http-handshake/fixture-mutations.json)可逐项复核。严格编译还修正了进程统计解析中 `%c` 错配 `int*` 的问题，目标已改为 `char`。

## 构建身份与 macOS

`5850d24` 的[原生 macOS 任务](https://github.com/ErosZy/capsid/actions/runs/43100912796)在配置阶段被构建身份校验拒绝：补丁变化后，锁文件仍为旧 overlay key/manifest。该运行实际测试数为 **0**。[原始归档](evidence/v0.3.0/2026-11-12-queued-http-handshake/macos-configure-original.zip)和[日志](evidence/v0.3.0/2026-11-12-queued-http-handshake/macos-configure-job.log)完整保留。之前的组件验证没有覆盖这个完整配置问题。

Windows/Linux 用项目共享算法独立计算得到相同的 78 份补丁 key 和 67 个受补丁影响文件的 manifest。当前子候选仅同步了锁文件这两个字段，vendor/QuickJS 版本及兼容性偏差声明未改，构建身份校验保留。当前候选在干净 Linux 源码上的[完整 production 配置与 78 补丁审计](evidence/v0.3.0/2026-11-12-queued-http-handshake/linux-clean-configure-and-audit.json)已通过，尚未完成该候选完整 ALL。

新 macOS [43101912311](https://github.com/ErosZy/capsid/actions/runs/43101912311)检出与当前候选等树的精确代码祖先 `d9fd796bd86de6c8aa7826606ebd86ed968bd493`，原预算复测此前剩余 26 项及本轮请求/回收用例。最近观察为 `in_progress`：原生配置已通过，正在编译 ALL；尚无原生测试通过结论。之前 `2ee13a5` 的 71 项原生通过、旧 52 项失败已关闭的结果仍有效；完整 macOS 矩阵未验收。

## 全量与正式测量

| 对象 | 实际源 | 当前结论 |
| --- | --- | --- |
| Windows 未过滤全量 | `7d393a0` | 845 项：832 通过、5 既有 WPT 失败、8 声明的平台跳过；严格参考当前为 `mandatory-platform-reference-gate-running` |
| Linux production/LTO 全量 | `7d393a0` | 1101 项：1096 通过、5 既有 WPT 失败、0 跳过；171 必需参考与严格兼容性审计通过；ordinarySuitePassed=false |
| 当前候选 Linux 完整流程 | `2979e33` | 干净源码已准备；独立队列 `waiting-for-original-windows-strict-reference-terminal`，待原 Windows 严格参考终结后顺序执行 TLS、ALL、未过滤全量和严格参考 |

上述父快照的完整结果没有重标为当前生产修正后的全量。原五份标准文件失败和对应子项超时仍保留。当前候选已核对[实际 Linux 注册](evidence/v0.3.0/2026-11-12-queued-http-handshake/linux-registration-check.json)为 1107 项：原有 1101 项的超时、跳过码和标签全部保留；新增 6 项不可通过退出码 77 跳过。注册验证不替代实际执行。

后续仍需：当前候选的三平台 ALL/完整矩阵、ASan/TSan/UBSan 与 RSS/资源入口、原预算 fuzz（10000 runs、max_len=65536 等均保留）、ABI/分配故障和组合验收，以及同一安静会话的正式 QPS/cold/density。正式性能点仍为 **0**，完整 R3/R5/R6/R7/R8 组合及 R9 开放。

密度继续保留一个 App、生产 least-loaded/最低槽优先及 64 并发。八 Worker 冷热阶段已逐个触达；65 READY Worker 的既有观察仍只触达 49 个，尚未验收。未扩大并发或修改调度，后续仍须满足全部后代 PSS、2048 MiB、两次相等正读数和退出证据。

性能页历史数据仍明确为 unverified。1280/960/820/600 四视口已有实际截图与布局检查；正式数据载入后再检查展示。用户无需连接浏览器。
