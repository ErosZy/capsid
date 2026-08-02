# 性能证据规则

性能结论必须能回到同一 commit 的原始样本和 profile。当前仓库没有 benchmark runner
与历史原始结果，因此不把以前文档中的 QPS、PSS 或百分比继续当作当前可复核结论。
恢复 runner 并重新采样之前，只能把这些数字视为容量假设，不能用于发布承诺。

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
