# M1P 无人值守性能优化作战手册

本文是 DeepSeek 执行下一阶段性能工作的唯一操作计划。目标不是堆叠“可能更快”的改动，
而是持续运行一个可审计闭环：**profile → 单一假设 → RED → 实现 → A/B → 保留或撤销**。
正确性、隔离、背压和 fail-closed 契约优先于吞吐；没有完整证据的改动不进入主线。

执行者无需逐项等待确认。只有触发本文的硬停止条件时才停下并提交阻断报告；其余情况按
优先级连续推进。每个被接受的改动独立提交并推送，证据另作提交，方便后续审计和回退。

## 1. 本阶段目标

先在最新干净 commit 上恢复可信基线，再从真实 profile 中选择热点。一个优化周期的目标为：

- 累计 QPS 提升 15% 以上，或 p99 降低 20% 以上；
- gateway 与 worker 的 CPU/response、IPC 帧/请求、系统调用/请求均有机制解释；
- `fixed-1k`、`json16k`、`bytes64k`、`bytes65537` 均无超过 5% 的回退；
- 1/1 非饱和延迟和 64/64 饱和尾延迟均不恶化；
- 错误、超时、协议错误、内部状态错误均为零；
- Release、ASan、UBSan、TSan 和故障回归保持通过。

这些是整个周期的目标，不是要求每个小改动都达到 15%。单个实验使用第 7 节的门槛。

## 2. 第零门：先消除伪证据

开始任何优化前必须完成以下工作：

1. 记录 `git rev-parse HEAD`、`git status --short` 和 `git rev-parse origin/main`。工作树
   必须干净且 HEAD 已推送。
2. 使用全新的 Release build 目录，禁止复用 `build-bench` 或任何增量目录。构建目录应带
   commit 短 SHA；配置、编译器、flags 写入 manifest。
3. 从这个目录重新构建 host、worker、测试和 benchmark 工具，记录各二进制 SHA-256。
4. `bench/results/dual-ab-r2-20260804T143227` 只能作为历史诊断：其中 `current` 未绑定
   最新 commit，且 `profile.txt` 不是调用栈 profile。不得把其数字当作本轮 baseline。
5. 最新 worker 的 SHA-256 若仍与旧 `3f7e8b2` evidence 相同，先排查构建/产物选择错误；
   在身份问题解决前禁止跑 A/B。
6. 先验证当前 `OutboundBuffer` compact、partial-write、EAGAIN、queue saturation 与 terminal
   语义的冻结测试。任何失败先按正确性缺陷处理，不归类为性能回退。
7. 生成一次最新 commit 对自身的 A/A。A/A 三轮 QPS 离散超过 5% 或 p99 离散超过 10%
   时，先治理环境噪声，不能进入 A/B。
8. 采集真实 host 和 worker 调用栈 profile。CPU PMU 显示 `<not supported>` 可以记录后忽略，
   但不能用资源采样文本冒充 profile。

第零门产物：一份完整 baseline evidence、调用栈热点表、每请求机制计数表和环境噪声报告。

## 3. 固定 benchmark 矩阵

Headline 轮次必须零探针；IPC metrics、strace、调试日志和额外计数只在独立 diagnostic/profile
轮开启。两侧使用同一 worker、bundle、loadgen、header/response 语义、credit 和资源限制。

### 3.1 必跑负载

| 负载 | 并发 | 回答的问题 |
| --- | ---: | --- |
| `fixed-1k` | 1 connection / 1 inflight | 非饱和端到端延迟、唤醒和小响应固定成本 |
| `fixed-1k` | 16 / 64 | 与已有 Go/C++ 公平基线连续 |
| `fixed-1k` | 64 / 64 | 饱和吞吐、排队和尾延迟 |
| `json16k` | 64 / 64 | JS 转换、分块、复制和中等响应成本 |
| `bytes64k` | 64 / 64 | 4 MiB 边界下的稳态吞吐 |
| `bytes65537` | 63 / 63、64 / 64 | 队列边界、背压、公平性和活性 |
| `cpu-template` | 16 / 64 | worker 计算占主导时避免误优化 Host |

快速筛选可用三轮、5 秒 warm-up、10 秒 measured；只有通过筛选的候选才进入正式门。正式
证据使用五轮交错 A/B、10 秒 warm-up、30 秒 measured。统计方法在运行前冻结为 median；
mean 同时报告但不驱动验收。

### 3.2 每轮必须保存

- QPS、p50/p95/p99、完成数、错误、超时、取消和 response abort；
- host/worker CPU time 与 CPU/response、RSS/PSS；
- host/worker 的 read/write/send/recv syscall 数和字节；
- IPC frames、commands、flush、wake/post、credit update、parsed event 每请求值；
- output logical/storage high-water、pending write 数、pump rounds 和复制字节；
- manifest、原始 samples、correctness、完整重放命令、文件 SHA-256；
- A/B 两侧独立的 host 与 worker 调用栈 profile。

当前 runner 已能生成标准 evidence。若新增 workload 或指标，必须先扩 runner 的 evidence gate
和 RED 测试，不能用临时 shell 汇总代替 manifest。

## 4. Profile 归因顺序

每轮先把成本分到四层，不能看到 `poll`/`epoll` 就断言 Host 慢：

1. **JS/worker 执行**：`JS_CallInternal`、转换、stream wrapper、GC、对象分配；
2. **worker IPC 输出**：copy、frame encode、queue pump、credit、flush、syscall、唤醒；
3. **Host 数据面**：event dispatch、Asio post、Beast serialize、socket write、Session 分配；
4. **排队/等待**：worker 饱和、credit 等待、socket backpressure、scheduler/off-CPU。

热点候选按下式排序：

```text
优先级 = 可归因 CPU/response × 覆盖请求比例 × 可安全消除比例 ÷ 实现与回归风险
```

必须在实验记录中写清：profile 符号、当前占比、对应机制计数、预期改变的计数以及估计收益。
“C++ 应该更快”“减少一次复制应该有用”不构成假设。

## 5. 优化漏斗

以下是候选顺序，不是无条件实施清单。只有上一轮 profile 和计数支持时才进入下一项。

### P1：低风险 IPC 与 buffer 热点

1. 检查 `OutboundBuffer` 的 compact 触发率、移动字节、峰值 storage/logical ratio。若 memmove
   占比可见，比较 frame-boundary compact、分块 buffer 或 ring；保留 partial header/frame
   写入的 RED 测试。
2. 检查一次 pump 的 frame 数、write syscall 数和 iovec 利用率。若 syscall/frame 接近 1，
   实验跨完整 frame 的 `writev`/批量发送；partial write 状态必须仍可精确恢复。
3. 检查 command flush 与 wake 次数。如果多条命令仍产生多次 wake/flush，只做有界聚合，
   同时冻结 shutdown、timeout 和 cancel 的最大唤醒延迟。
4. 检查 response credit 更新频率。仅在不改变上限和公平性的前提下按阈值聚合；SSE/小窗口
   负控必须证明不会饿死。

### P2：复制、所有权与分配

1. 量化每响应 native copy 字节、JS snapshot copy 字节和 allocator 热点。
2. 保持“应用修改 pending `Uint8Array` 不改变响应”的所有权语义。任何减少快照的方案必须
   先让 mutation RED 测试在移除保护后失败。
3. 优先尝试小响应内联、对象/command/frame metadata 复用和有界 slab；不得以持有无界 JS
   对象或扩大 4 MiB queue 规避复制。
4. 大块响应可实验固定 quantum 的 snapshot/发送流水线，但 terminal、timeout、cancel 必须
   各自恰一次且 round-robin 公平性不变。

### P3：事件调度与 owner loop

1. 先统计每请求 `asio::post`、wake、锁争用和跨线程 handoff；无可见热点则跳过。
2. 先做 event/command 批处理，不直接重写线程模型。
3. 只有跨线程 handoff 在 profile 中稳定占据显著成本，且 P1/P2 已收敛，才允许做同 owner
   loop 的 feature-flag 原型。它必须同时证明多 worker 扩展、p99 和取消/关闭正确性。
4. owner loop 若不能带来至少 10% QPS 或 15% p99 改善，撤销原型，不把复杂度留在主线。

### P4：Host 与 QuickJS

1. Host 只处理 profile 证明的热点：header 规范化、Beast serializer、Session/closure 分配、
   socket write 合并。公平基线已显示 Host CPU 较低，不可凭感觉优先重写 Beast/Asio。
2. QuickJS 侧分别测 `json`、`bytes`、`cpu-template`。优化 JS/native 转换、stream wrapper 或
   GC 前，必须证明热点不是队列等待。
3. 可信字节码、secret snapshot、URL/header 校验和响应语义不得为了 benchmark 旁路。

## 6. 每个实验的 TDD 模板

每个实验只改变一个机制，并按以下顺序执行：

1. 从已接受的干净 commit 建立 fresh build 和 baseline。
2. 写 `hypothesis.md` 或 evidence metadata：热点、机制、预期计数变化、风险、回退门。
3. 先写 RED：正确性变化用单测/集成负控；纯机制变化用可观测计数断言。必须实际证明删掉
   保护或使用旧实现会 RED。
4. 实现最小改动，不顺手重构相邻模块。
5. 跑目标测试、Release 全量，以及与改动相关的 ASan/UBSan/TSan；并发、生命周期或 metrics
   改动必须跑 TSan。
6. 单独提交候选代码并推送。
7. 在该干净 commit 上做快速 A/B；通过后做正式五轮 A/B 和前后 profile。
8. 通过第 7 节门槛则保留代码并提交 evidence；否则用新的 revert commit 撤销，记录失败
   原因，禁止 `reset --hard` 或删除历史。
9. 更新热点排行榜和已否决假设，继续下一循环。

## 7. 接受、观察和拒绝

一个实验只有满足正确性门，且满足下面任一性能门，才可标记 `pass`：

- QPS 提升至少 5%，p99 回退不超过 5%，CPU/response 回退不超过 5%；或
- p99 降低至少 10%，QPS 回退不超过 2%，CPU/response 回退不超过 5%；
- 同时至少一个预先声明的机制计数改善 10% 以上。

所有 workload 必须零错误/超时，1/1 延迟及非目标负载不得回退超过 5%，RSS/PSS 不得增加
超过 5%。若内存换性能确有价值，必须单独提出并记录上限，不能自动接受。

结果在噪声内、只有 mechanism 改善而 headline 未过门、或不同 A/B 方向不一致时，结论是
`inconclusive`，不是 pass。类似 bodyless 的机制收益必须继续以 waiver 表达，不能自动转成
性能成功。

## 8. 无人值守循环与停止条件

DeepSeek 连续执行以下循环，直到达到阶段目标或触发硬停止：

```text
校验干净状态和磁盘
  → fresh build / A/A
  → profile + 机制计数
  → 排序并选择一个假设
  → RED
  → 最小实现
  → correctness + sanitizers
  → 提交并推送代码
  → 快速 A/B
  → 正式 A/B + 前后 profile
  → pass: 提交证据；fail: revert 并记录
  → 更新排行榜，进入下一项
```

以下情况必须停止，不允许猜测性继续：

- 同一正确性、ASan、UBSan 或 TSan 故障连续三次无法闭环；
- evidence 身份、二进制 SHA、profile、原始样本或 runner gate 不可信；
- A/A 离散超过第零门，或出现无法解释的 5% 以上回退；
- 连续三个优化实验均为 fail/inconclusive，应重新 profile，而非继续同方向堆改动；
- 需要改变 wire protocol、公共 ABI、4 MiB 上限、隔离或安全契约；这些属于新设计，另行审计；
- 可用磁盘低于 20 GiB 或文件系统使用率超过 85%。

## 9. 磁盘与产物纪律

- 开始每个正式 run 前记录 `df -h`；只使用明确命名的 fresh build/result 目录。
- 保留：最新 canonical baseline、所有已接受实验、最近一个失败实验及其最小诊断。
- perf 原始数据可压缩；派生报告不得替代原始 profile。
- 删除时只删除已核对的具体临时目录，不运行 `docker system prune -a`，不使用宽泛 glob。
- 代码 commit 与 evidence commit 分开；大体积临时 strace 不入库，只保存摘要和必要片段。

## 10. 最终交付格式

每个接受或拒绝的实验报告只需包含：

1. commit 与修改文件；
2. profile 热点和原假设；
3. RED → GREEN 证据；
4. 五轮 A/B 的 QPS、p50/p95/p99、CPU/response、RSS/PSS、错误；
5. 预声明机制计数的前后值；
6. host/worker dominant stacks；
7. `pass`、`fail` 或 `inconclusive`，以及保留或 revert 的 commit；
8. 下一优先级假设。

整个 M1P 结束时再提供一张累计表，明确区分：正确性修复、机制改善、端到端性能收益、未被
证实的推测。最终数字必须来自最新接受 commit 的零探针正式证据。
