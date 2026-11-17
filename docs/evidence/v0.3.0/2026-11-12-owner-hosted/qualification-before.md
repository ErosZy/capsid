# v0.3.0 当前验收与兼容边界

更新时间：2026-11-11T17:40:58.333145+00:00。**v0.3.0 尚未完成全平台和正式性能验收。** 当前冻结候选为 `c56e2e1ed561586123b84fe993fa935edc98a4a5`，SDK78、5125份源码；代码逐字节核对通过。主工作区HEAD仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未更新主分支、标签或发布。

[本轮证据索引](evidence/v0.3.0/2026-11-12-byob-density/index.json)、[当前候选](evidence/v0.3.0/2026-11-12-byob-density/candidate.json)、[任务清单快照](evidence/v0.3.0/2026-11-12-byob-density/goal-ledger-at-capture.json)和[此前原文](evidence/v0.3.0/2026-11-12-byob-density/qualification-before.md)保留各轮身份与失败。旧结果不改标成当前候选通过。

## 本轮完成

- **BYOB参考监听端口。** 旧代码释放临时端口后才启动workerd。Windows/Linux受控竞争均能使明文探测误入另一个TLS监听器，得到与原失败同型的 `ERR_SSL_HTTP_REQUEST`，workerd同时绑定失败。新代码由同一锁定workerd通过 `--control-fd=3` 报告实际绑定的端口0结果，再探测该端口；严格验证事件类型、socket名和有效端口。不忽略TLS错误，不更改参考版本、兼容flags、原请求断言或启动总预算。复现的是机制，没有精确追踪Hosted原轮进程调度。
- **BYOB原生完整分片。** Windows参考/single-worker/static-pool共21分片，Linux再含managed共28分片；合计49分片、3409个实际场景和两平台模式比较通过。每模式保留全部487场景，原120秒分片预算保持。合同控制每平台9894项，零失败、零跳过；原始参考捕获、gzip及哈希未重写。实际产品二进制仍绑定Windows的6df和Linux的b284父候选，这不是最新候选的重新ALL/全量验收。[原始记录](evidence/v0.3.0/2026-11-12-byob-density/byob-native-matrices.json)可复核。
- **8 Worker逐个预热。** 保留单App、least-loaded/最低槽优先的生产调度。HTTP客户端先连接，再同步发请求；每Worker最低计数停滞时扩大请求批次，仍不超过64并发和原请求总预算。Windows/Linux各16项控制通过。实际新建8-worker进程池中，冷阶段24个正确响应覆盖全部8个、每个至少1次；独立热池1432个正确响应使每个至少101次，最大批次32。READY事件、真实后代进程、精确正文、逐Worker日志计数及清理均核对，剩余拥有PID为0。[冷/热证据](evidence/v0.3.0/2026-11-12-byob-density/density-eight-workers.json)不代替PSS验收。

本轮相对a4父候选只改五份测试/benchmark工具文件；生产runtime和调度未变。此前的IPv6配置、原始fixture字节、Darwin参考锁、CI归档和合成producer锁修正继续保留。

## 仍未完成的明确边界

**大池预热仍失败。** 新建单App的65-worker池有65个实际READY，64并发、520个正确响应后只观测到49个Worker，覆盖入口正确拒绝并清理所有拥有进程。[原始边界](evidence/v0.3.0/2026-11-12-byob-density/density-sixty-five-worker-boundary.json)保留该失败。已向用户询问是否允许仅密度预热临时按池大小扩并发；在收到选择前，原64并发协议保持，不宣称高密度验收完成，也不改成多个App或改变生产调度。

**正式性能点仍为0。** 完整后代PSS、2048MiB预算、连续两次相等正读数及同一安静会话的正式三车道仍须实际完成。上述覆盖观测未采纳正式PSS点。四视口1280/960/820/600仍无实际截图验收；当前不要求用户处理浏览器连接。

## 运行中的全量及父候选结果

- Windows本机原始842项全量仍绑定6df候选，并在冻结目录独立运行；新的源码和工具没有写入该目录。严格参考验收待原全量结束。
- [macOS修正版任务43065514871](https://github.com/ErosZy/capsid/actions/runs/43065514871)绑定fixture字节修正祖先96cea，已通过锁定运行时/workerd、干净Release配置和ALL，进入原POSIX完整矩阵。尚未验收最新候选，也未删除既有socket、receive-buffer、channel和配额等失败用例。
- 父候选6df的UBSan任务为997通过、0失败、5跳过，工作流成功；跳过没有计作通过。ASan普通矩阵为994通过、3失败、6跳过，另两项原始RSS失败，独立内存入口拒绝。QPS锁冲突与BYOB端口竞争已各有修正/对照，原Hosted失败仍保持；新完整ASan验收未完成。
- 父候选6df的macOS原始全量仍为752项：555通过、183失败、14跳过，保留原始清单；不能把它重标成新环境或新源码通过。

## 后续验收

R1/R2/R4已有定向完成记录；R3/R5/R6/R7/R8组合验收及R9仍未完成。还需收齐当前运行结果，修正macOS剩余原生差异、Windows reader-scope强制SIGKILL场景、fuzz原60秒预算超时、冷控制PSS不稳定，并完成链接后的ABI分配故障检查及最新候选全量。原数据集、原预算、五项既定WPT差异之外的额外失败和不符合契约的跳过继续由严格入口拒绝。
