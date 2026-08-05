# 性能证据规则

性能结论必须能回到同一 commit 的原始样本和 profile。当前 runner 位于 `bench/`；历史
结果只有在 manifest、二进制身份、原始样本和两侧 profile 均绑定同一被测 commit 时才可
复用。缺少这些条件的旧 QPS、PSS 或百分比只能视为诊断记录，不能用于发布承诺。连续优化
的执行流程见 [M1P 无人值守性能优化作战手册](performance-optimization-playbook.md)。

## 结论门槛

一次可以写入产品文档的性能结论必须同时具备：

- 相同硬件、操作系统、编译类型、Runtime、worker、bundle 和资源限制；
- 相同 load generator、连接数、inflight、响应内容和校验逻辑；
- warm-up 与 measured phase 分离，至少三轮交错 A/B 原始样本；
- QPS、p50/p95/p99、错误、超时、取消、drain、CPU 和内存一起保存；
- gateway 与 worker 分别采集 profile，能解释时间花在哪一层；
- 记录 commit、依赖 identity、构建 flags、命令、环境和结果文件 SHA-256；
- 正控证明返回内容正确，负控证明错误响应不会被记作成功。

缺少原始 A/B 或任一侧 profile 时，可以报告“观察到的样本”，不能写“优化有效”、
“提升 N%”或据此调整默认容量。

## 不可混用的测量

以下数据回答不同问题，报告必须分开：

1. 完整 HTTP stack：listener、HTTP parser、路由、Host、IPC 和 worker 的总成本；
2. Host A/B：同一 Runtime/worker 下，只替换 gateway/Host；
3. 单 worker：执行、QuickJS heap、PSS/RSS 和应用负载；
4. 冷启动：进程创建、握手、校验、加载、READY 和首响应；
5. 密度/稳定性：长时间运行、SSE、崩溃替换、drain 和蓝绿双池。

完整容器 RSS/PSS 不能与单 worker PSS 直接比较；同样，源码与可信字节码冷启动也不能
替代预热后的请求吞吐测试。

## 第一方 C++ Host 的早期检查点

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

## 结果保存格式

重新引入 benchmark 时，每次运行至少保存：

```text
bench/results/<run-id>/
├── manifest.json       # commit、身份、环境、命令和文件摘要
├── samples.jsonl       # 每轮原始指标，不只保存聚合值
├── correctness.json    # 内容、错误和负控结果
├── host-profile.*      # gateway/Host profile
├── worker-profile.*    # worker profile
└── report.md           # 只从上述文件生成的解释
```

`report.md` 是派生视图。自动审计应拒绝孤立报告、缺 profile 的“提升”结论、未绑定 commit
的结果，以及只提交汇总数字而没有原始样本的变更。

## 当前优化原则

- 先用 profile 找热点，再写优化和对应 RED benchmark；
- 不为了推测收益引入 io_uring、共享内存 IPC、自定义 HTTP parser 或复杂调度；
- 正确性、隔离和 fail-closed 契约不能为了 QPS 绕过；
- 默认 worker/inflight/queue 数值只有在代表性 workload 扫描后才能冻结；
- 每次性能改动都跑正确性、sanitizer、故障注入和同条件回归。

## M2 基线重定义（2026-08-05）

三组隔离 A/B（64/64、5 轮、20s、cpuset 0-7）证明：fixed-1k 单 worker 在本机为
**~1300-1345 QPS**（空白对照 delta +4.2%、single vs static-pool-1 -1.2%、fda9dcc Host vs
当前 Host -1.4%，loadgen ~0.25 core 未饱和）。此前的 1600+ QPS 数字来自不同环境/commit，
**不得再作为本机对比基准**；任何跨版本对标一律以同机同条件重跑为准。

## M2 池规模结论（Phase 1，2026-08-05）

**方法**：fixed-1k × 并发 {64,128} × workers {1,2,4,6,8}，5 轮交错、20s
measured、mean、errors=0/timeouts=0。**cpuset 全程恒定**：被测端（host+全部
worker）→ CPU 0-5（6 核），loadgen → CPU 6-7（2 核）；交叉验证组 loadgen
放宽 5-7（与被测重叠 1 核）。worker 数是被测端唯一自变量。

**64 并发**：

| workers | QPS | speedup | efficiency | p50 ms | p99 ms |
|---------|-----|---------|------------|--------|--------|
| 1 | 1736 | 1.00 | 1.00 | 35.3 | 58.5 |
| 2 | 3037 | 1.75 | 0.87 | 19.8 | 40.7 |
| 4 | 5094 | 2.93 | 0.73 | 12.2 | 24.0 |
| 6 | **6007** | 3.46 | 0.58 | 10.4 | 22.8 |
| 8 | 5640 | 3.25 | 0.41 | 10.5 | 28.2 |

**128 并发**：

| workers | QPS | speedup | efficiency | p50 ms | p99 ms |
|---------|-----|---------|------------|--------|--------|
| 1 | 1778 | 1.00 | 1.00 | 70.7 | 98.5 |
| 2 | 3208 | 1.80 | 0.90 | 39.2 | 57.8 |
| 4 | **5169** | 2.91 | 0.73 | 24.0 | 40.9 |
| 6 | 5190 | 2.92 | 0.49 | 23.1 | 58.0 |
| 8 | 5739 | 3.23 | 0.40 | 21.4 | 47.4 |

**判定：假设 A（核数超订）成立，B（Host 内部争用）被排除。**

1. **CPU 记账**（candidate profile 20s 窗口 vs 6 核上限 120s）：1w 17%、
   2w 34%、4w 65%、6w 78%、8w 79%——总 CPU 钳制在 ~79%；每 worker 平均
   CPU 随 N 稀释（4w 0.83 → 6w 0.66 → 8w 0.50 core）。8 个 worker × 2
   线程 = 16 线程挤 6 核，超订直接体现。
2. **REUSEPORT 分布**（逐 shard task-clock，WSL2 无 tracefs，以 CPU 均匀性
   作代理）：4w spread 7.5%（均匀）；6w 13.9%；8w 30.3%——分发不均只在
   超订临界出现（调度竞争的后果，非独立原因），4w 的 eff 0.73 非分发问题。
3. **worker-bound**：逐 shard profile 的 JS_CallInternal 单符号 25.5%（4w
   与 8w 一致），QuickJS 解释器路径主导；Host 占比未随 N 上升。
4. **交叉验证**：最优 6w-64 组 loadgen 放宽 3 核 → 5855 QPS（vs 2 核
   6007，下降来自与被测端重叠 1 核）——QPS 未随 loadgen 余量上升，
   **发压端未饱和**。loadgen 实测 ≤0.3 core。

**推荐默认池规模**：128 并发（生产形态）下 **4 worker 为饱和平台**
（5169 vs 6w 5190 无增益），且 eff 0.73 为可用性/效率权衡点；64 并发下
6w 峰值但 eff 0.58。按"每 shard ≈2 线程"的先验（可用核数/2），本机 6 核
对应 3-4 worker，实测 4 worker 是稳健默认。**推荐公式：默认池规模 =
min(4, 可用核数/2)，超订配置（workers > 可用核数/2）只换 eff 不换吞吐。**

**基线修正**：loadgen 与被测端隔离后，单 worker 实测 **1700-1830 QPS**
（此前 cpuset 共享时的 1300-1345 基线受发压端抢核压低，作废）。本机新
基线以本文档为准。

**CV 注**：64 并发全部组 CV ≤6.2%（门槛 7%）；128 并发的 6w（11.0%）与
8w（8.6%）超订临界区 CV 超标，其 QPS 数值为观察样本、不作容量决策依据。
