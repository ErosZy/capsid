# 性能：证据规则与当前形态

本文是性能主题的唯一维护文档，合并了 2026-08 优化循环（M1P、E1-E14、08-14
审计）的结论。原始样本、profile 与逐轮 evidence 保存在 `bench/results/`，
由[测试与持续门禁](testing.md)与 CI artifact 管理；本文只保留活规则与当前
可复核结论，不复制逐轮过程记录。

## 1. 证据规则

性能结论必须能回到同一 commit 的原始样本和 profile。当前 runner 位于 `bench/`；历史
结果只有在 manifest、二进制身份、原始样本和两侧 profile 均绑定同一被测 commit 时才可
复用。缺少这些条件的旧 QPS、PSS 或百分比只能视为诊断记录，不能用于发布承诺。

### 结论门槛

一次可以写入产品文档的性能结论必须同时具备：

- 相同硬件、操作系统、编译类型、Runtime、worker、bundle 和资源限制；
- 相同 load generator、连接数、inflight、响应内容和校验逻辑；
- warm-up 与 measured phase 分离，至少三轮交错 A/B 原始样本；
- QPS、p50/p95/p99、错误、超时、取消、drain、CPU 和内存一起保存；
- gateway 与 worker 分别采集 profile，能解释时间花在哪一层；
- 记录 commit、依赖 identity、构建 flags、命令、环境和结果文件 SHA-256；
- 正控证明返回内容正确，负控证明错误响应不会被记作成功。

缺少原始 A/B 或任一侧 profile 时，可以报告"观察到的样本"，不能写"优化有效"、
"提升 N%"或据此调整默认容量。

### 不可混用的测量

以下数据回答不同问题，报告必须分开：

1. 完整 HTTP stack：listener、HTTP parser、路由、Host、IPC 和 worker 的总成本；
2. Host A/B：同一 Runtime/worker 下，只替换 gateway/Host；
3. 单 worker：执行、QuickJS heap、PSS/RSS 和应用负载；
4. 冷启动：进程创建、握手、校验、加载、READY 和首响应；
5. 密度/稳定性：长时间运行、SSE、崩溃替换、drain 和蓝绿双池。

完整容器 RSS/PSS 不能与单 worker PSS 直接比较；同样，源码与可信字节码冷启动也不能
替代预热后的请求吞吐测试。

### 第一方 C++ Host 的早期检查点

第一方 Host 不等待全部 v1 功能完成才测试。单 worker path listener、
GET/HEAD 无 request body、URL/header 规范化、response credit、keep-alive 和
内容正确性形成最小闭环后，立即执行 `M1-perf`；首轮不等待 request
body、streaming、cancel 或 timeout 实现。这些契约完成后必须用同一 runner
增加第二个回归检查点：

```text
同一 load generator ─┬─ Go capsid-http-gw ─┐
                     └─ C++ capsid-host ────┤
                         同一 Runtime/worker/bundle
```

首轮目标是建立可重复 baseline，不是证明 C++ 必然更快。用户此前观察到的 gateway
约 20%、worker 约 60–65% CPU 占比，可以用来设计 profile 分组，但不能直接相加成
预期 QPS；排队、IPC、内存分配和 worker 饱和会改变端到端结果。详细验收契约见
[Host v1 详细设计 15.7](host-technical-design-review.md#157-性能验收)。

### 结果保存格式

每次运行至少保存：

```text
bench/results/<run-id>/
├── manifest.json       # commit、身份、环境、命令和文件摘要
├── samples.jsonl       # 每轮原始指标，不只保存聚合值
├── correctness.json    # 内容、错误和负控结果
├── host-profile.*      # gateway/Host profile
├── worker-profile.*    # worker profile
└── report.md           # 只从上述文件生成的解释
```

`report.md` 是派生视图。自动审计应拒绝孤立报告、缺 profile 的"提升"结论、未绑定 commit
的结果，以及只提交汇总数字而没有原始样本的变更。

### 当前优化原则

- 先用 profile 找热点，再写优化和对应 RED benchmark；
- 不为了推测收益引入 io_uring、共享内存 IPC、自定义 HTTP parser 或复杂调度；
- 正确性、隔离和 fail-closed 契约不能为了 QPS 绕过；
- 默认 worker/inflight/queue 数值只有在代表性 workload 扫描后才能冻结；
- 每次性能改动都跑正确性、sanitizer、故障注入和同条件回归。

## 2. 当前性能形态（2026-08 收敛）

以下数字来自对应 `bench/results/` 原始样本，全部满足 §1 证据规则。不同行的
方法（连接数、进程形状、机器）不同，**不可跨行直接比较**；它们回答各自的问题。

### 2.1 池规模结论（128 并发，2026-08-05，6 核 cpuset 扫描）

| workers | QPS (128c) | efficiency |
|---------|-----------:|-----------:|
| 1 | 1778 | 1.00 |
| 2 | 3208 | 0.90 |
| 4 | **5169** | 0.73 |
| 6 | 5190 | 0.49 |
| 8 | 5739 | 0.40 |

- 128 并发下 **4 worker 是饱和平台**（4w vs 6w 无增益）；CPU 记账证明 6w 以上
  进入超订（8 worker × 2 线程挤 6 核），分发不均只是超订的后果。
- **默认池规模 = `min(4, 可用核数/2)`**；workers > 可用核数/2 只换效率不换吞吐。
- loadgen 与被测端隔离后单 worker 基线 1700–1830 QPS；64 并发 CV 全部 ≤6.2%，
  128 并发超订临界区（6w/8w）CV 超标，其数值只作观察样本。

### 2.2 三栈对照（c64，2026-08-13/14）

相同双进程协议、payload 逐字节对齐、0 errors/0 timeouts。实现语言与服务器模型
不同，**不是胜负榜**，只用于确认量级：

| workload | Capsid | Flask+gunicorn | PHP Slim |
|---|---:|---:|---:|
| JSON 1 KiB | 2434 req/s | 1833 | 1585 |
| bytes 64 KiB | 1194 req/s | 1509 | 1115 |
| stream 64 KiB | 739 req/s | 1132 | 907 |

**形态：常规 JSON 高并发相对强，大流式响应相对弱。** 单 worker 延迟受
QuickJS 解释器（无 JIT）主导；流式差距来自每 chunk 的 credit/write 往返。
QuickJS JIT 属于 vendor 级变更，是独立评估项目，不在当前优化循环内。

### 2.3 第一方 Host 优化结论（2026-08-14，commit dd2e05b1 审计基线）

三个机制，全部只改第一方 Host 调度/日志/写出路径，未改 vendor：

1. **StructuredLog 忙等消除**：空队列加锁轮询 → 条件变量等待，FIFO 语义修正
   （基线 c1 JSON 1 KiB 中 `pthread_mutex_*`/`writer_run` 合计 56.8% CPU）；
2. **retire_terminal_request**：正常 RESPONSE_END 后不再向已终止的 Runtime
   提交冗余 CANCEL（tombstone 保留到本地批次排空，覆盖 swap 竞态）；
3. **响应块合并**：静态池与 managed listener 共用的 64 KiB 上限合并器，只合并
   相邻且 credit 状态相同的已排队帧，不跨 credit 边界、不改 IPC 格式。

| 指标 | c1 | c64 |
|---|---:|---:|
| QPS | +3.4%（吞吐方向不一致，不声明提升） | **+23.5%** |
| CPU / request | **−57.9%** | **−26.6%** |
| p99 | **−28.8%** | **−15.8%** |

三轮确认代表负载：JSON 1 KiB c64 QPS +17.9%、bytes 64 KiB c64 +36.2%、stream
64 KiB c64 +18.1%；`perf` 因果验证显示 lock/writer 热点从 5.5–19% 降至报告
阈值以下。**未保留的实验**：Worker 侧 ResponseBody 帧合并（v2/v3 三轮对照
收益 +0.2..+1.2% 且 stream 64 KiB p99 +11.9%，显式 revert）——Worker 事件循环
通常在后续帧排队前就开始 flush，安全合并窗口很小。

### 2.4 正确性修复对性能的约束

- 响应队列饱和修复（2026-08-04）是**正确性/活性契约**：任意合法响应必须能
  通过 credit 分段大于 `max_queued_bytes` 传输；正常队列压力只能让
  `capsidResponseWrite()` pending，不能抛 RangeError；每个请求必须精确产生
  一个 terminal（ResponseEnd/Error/Timeout）。4 MiB 上限与 wire protocol
  不允许为性能改动。
- bodyless 请求融合（2026-08-03）机制门通过（每请求 3→2 命令、4→3 帧），
  但独立性能门未达门槛（pooled −0.59%，在机器噪声带内），按 waiver 记录
  而非自动通过。

## 3. 历史索引

2026-08 优化历程的完整过程记录（含逐轮 A/B、profile、已撤销实验）在 git
历史与 `bench/results/` 原始 artifact 中：

- M1P 循环（08-03~05）：playbook 定义，硬停止于连续 3 个失败实验；
- E1-E14 记分板（08-13）：收敛于"循环内可安全消除的成本已耗尽"；
- 完成度审计与性能报告（08-14）：WP-00..09 全部完成，上述 §2.3 三个机制；
- 三栈基准两阶段（08-13）：双进程协议 + 极限调优对比。

原始样本：`bench/results/{profile-20260814,three-stack-*,m1p-*,dual-ab-*}/`。
