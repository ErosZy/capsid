# 性能：证据规则与当前形态

本文是性能主题的唯一维护文档，只保留证据规则与最新（2026-08-14）结论。
历史优化过程（M1P、E1-E14、Host 优化循环）在 git 历史与
`bench/results/` 原始 artifact 中，不在此维护。

## 1. 证据规则

### 结论门槛

一次可以写入产品文档的性能结论必须同时具备：

- 相同硬件、操作系统、编译类型、Runtime、worker、bundle 和资源限制；
- 相同 load generator、连接数、inflight、响应内容和校验逻辑；
- warm-up 与 measured phase 分离，至少三轮交错 A/B 原始样本；
- QPS、p50/p95/p99、错误、超时、取消、drain、CPU 和内存一起保存；
- gateway 与 worker 分别采集 profile，能解释时间花在哪一层；
- 记录 commit、依赖 identity、构建 flags、命令、环境和结果文件 SHA-256；
- 正控证明返回内容正确，负控证明错误响应不会被记作成功。

缺少原始 A/B 或任一侧 profile 时，可以报告"观察到的样本"，不能写
"优化有效"、"提升 N%"或据此调整默认容量。

### 不可混用的测量

以下数据回答不同问题，报告必须分开：完整 HTTP stack 总成本；Host A/B；
单 worker 执行与内存；冷启动（进程创建、握手、校验、加载、READY 和首
响应）；密度/稳定性。完整容器 RSS/PSS 不能与单 worker PSS 直接比较；
源码与可信字节码冷启动也不能替代预热后的请求吞吐测试。

### 结果保存格式

每次运行至少保存 manifest（commit、身份、环境、命令和文件摘要）、原始
样本（不只聚合值）、correctness 结果、两侧 profile 和只从上述文件生成
的 report。自动审计应拒绝孤立报告、缺 profile 的"提升"结论、未绑定
commit 的结果，以及只提交汇总数字而没有原始样本的变更。

### 当前优化原则

- 先用 profile 找热点，再写优化和对应 RED benchmark；
- 不为了推测收益引入 io_uring、共享内存 IPC、自定义 HTTP parser 或复杂调度；
- 正确性、隔离和 fail-closed 契约不能为了 QPS 绕过；
- 默认 worker/inflight/queue 数值只有在代表性 workload 扫描后才能冻结；
- 每次性能改动都跑正确性、sanitizer、故障注入和同条件回归。

## 2. 测试环境（2026-08-14）

所有最新结论共用以下环境：

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 3 3300X（4C/8T） |
| OS | Alpine Linux v3.24（WSL2，内核 6.6.87.2-microsoft-standard-WSL2） |
| 内存 | 8 GB |
| 进程协议 | SUT taskset 0-3 / loadgen 4-7；双进程模型 |
| 负载协议 | conns=64，12 workloads × 3 轮（warmup 3s + measured 8s），correctness 逐轮校验 |

被测栈（版本均记录在各自 manifest）：

| 栈 | 组件与版本 |
|---|---|
| capsid + hono | capsid commit 9bde135（build-m1d）+ hono bundle（sha256 见 manifest）；static-pool 2 workers |
| PHP 8 + Slim | PHP 8.5.8 + Slim 4.15.2 + nginx 1.26.3 + php-fpm（pm.max_children=2） |
| Python 3 + Flask | Python 3.14.5 + Flask 3.1.3 + Gunicorn 26.0.0（2 workers） |
| 冷启动附加 | Node v24.18.0、Deno 2.9.3 |

## 3. 三栈全矩阵（4C，2026-08-14，c64，64K 窗口）

payload 逐字节对齐、0 errors/0 timeouts、**33/36 格结论级（CV ≤ 7%）**；
php bytes16k、capsid stream32k、python stream32k 三格 CV 超标，按观察
样本记录（表内不标注，原始样本可查）。实现语言与服务器模型不同，
**不是胜负榜**，只用于确认量级。capsid 侧使用产品默认
`--initial-stream-window 65536`。原始样本：
`bench/results/three-stack-20260814T172510/`。

| workload | capsid + hono | PHP 8 + Slim | Python 3 + Flask |
|---|---:|---:|---:|
| json 1k | **6820** | 1826 | 4625 |
| json 8k | **5213** | 1727 | 4683 |
| json 16k | **5304** | 1679 | 4495 |
| json 32k | **4558** | 1592 | 3865 |
| bytes 1k | **4591** | 1727 | 4510 |
| bytes 8k | **4405** | 1641 | 4375 |
| bytes 16k | 3971 | 1557 | **4252** |
| bytes 32k | 3414 | 1572 | **3908** |
| stream 1k | **4593** | 1745 | 4442 |
| stream 8k | **3952** | 1708 | 3570 |
| stream 16k | **3501** | 1652 | 3377 |
| stream 32k | 2886 | 1592 | **3756** |

**形态**：常规 JSON 全胜，小载荷优势最大（json 1k 为 Python 3 栈的
1.47×、PHP 8 栈的 3.74×）；大字节流载荷（bytes ≥16k、stream 32k）
Python 3 栈反超，其中 stream 32k（2886 vs 3756）掉队成因待查。PHP 8
栈全矩阵垫底（约为 capsid 的 0.26-0.40×），CV 最优。QuickJS 解释器
（无 JIT）仍是单 worker 延迟主导；JIT 是 vendor 级变更，属独立评估
项目。

## 4. 冷启动对照（4C，2026-08-14，中位数 ms）

第 4 类测量（进程创建、握手、校验、加载、READY 和首响应）。fixture 为
真实形态 JS 源码（三种模板轮转：循环+对象字面量函数、class、
箭头/map/filter/sort 链），10k/100k/1M 三档（36/355/3547 个顶层单元），
各端加载同一函数体逐字节对齐，仅入口点不同。capsid 用 C ABI
spawn→load（源码/可信字节码）→READY→首响应（bodyless IPC 请求）；
Node/Deno 用进程启动→stdout READY→curl 首请求。每格 1 轮 warmup 丢弃 +
5 轮取中位数。原始样本：`bench/results/cold-start-20260814T171047/`。

| 尺寸 | capsid 源码 | capsid 可信字节码 | Node 24 源码 | Deno 2.9 源码 |
|---:|---:|---:|---:|---:|
| 10k | **9.5** | **8.2** | 110 | 39 |
| 100k | **19.6** | **10.6** | 110 | 40 |
| 1M | 141 | **42** | 149 | 53 |

READY 时刻（同样本）：capsid 源码 9.1/19.2/141.0、字节码 7.8/10.1/41.4、
Node 97/97/137、Deno 31/32/45。

1M 的阶段拆分（median）：capsid 源码 spawn 0.2 + 传输 7.6 + **编译 133**
= 141；capsid 字节码 spawn 0.2 + 传输 21.3 + **反序列化 20** = 41.4
（qjsb 2.46MB，QuickJS 字节码不压缩，体积为源码的 2.5×）；Node 97ms
启动基数 + 40ms 解析；Deno 31ms 启动基数 + 14ms 解析。

- **真实形态下尺寸显著敏感**：capsid 源码 10k→1M 总耗时 +132ms，编译
  成本与 AST 节点数成正比（3547 个顶层单元 ≈133ms）。
- **可信字节码收益随编译成本放大**：1M 真实源码 141 → 41.7ms（−70%，
  3.4×），字节码路径全面第一（比 Deno 快 21%、比 Node 快 3.6×）；小
  尺寸收益收敛（10k 只快 1.3ms）。但字节码不免费：体积 2.5× 使传输
  21.3ms 成为该路径第二大成本，反序列化仍需重建 AST（≈20ms）。
- **源码路径与 Node 同量级、慢于 Deno**：1M ready capsid 141 ≈ Node
  137（QuickJS 全量编译 vs V8 解析+懒编译），Deno 45ms 是源码路径
  最快——V8 解析器的优势在 AST 密集源码上显现。
- **小尺寸下启动基数主导**：10k 时 capsid 源码 9.1ms ready 仅为 Node
  的 1/11、Deno 的 1/3；Node/Deno 的 97/31ms 启动基数在小 bundle 下
  无从摊销。
- 语义说明：capsid 首响应走进程内 IPC，Node/Deno 走本地 HTTP curl；
  "就绪后首个请求完成"对齐，请求路径实现不同（ready→total 差：
  capsid ≈0.4ms、Deno ≈8ms、Node ≈13ms），不构成同构比较。

## 5. 资源形态（4C，2026-08-14，进程数/PSS/RSS）

三栈常驻（双进程协议，空闲）时由 `bench/sample-sut-memory.sh` 每 15s
采样进程树 PSS（`smaps_rollup`）与 RSS，8 个空闲 tick 取中位数；负载
轮（json c64）2 个 tick 仅观察。原始样本：
`bench/results/sut-memory-20260814T173000/`。

| 栈 | 进程数 | 空闲 PSS | 空闲 RSS | json 负载 PSS |
|---|---|---:|---:|---:|
| capsid + hono | 3（host + 2 workers） | **12.3 MB** | 21.8 MB | 13.5 MB |
| PHP 8 + Slim | 12（php-fpm + nginx） | —* | 124.1 MB | —* |
| Python 3 + Flask | 3（gunicorn + 2 workers） | 62.6 MB | 89.4 MB | 62.6 MB |

*php 容器进程跨用户，非 root 读不到 `smaps_rollup`，PSS 不可得；其 RSS
含 nginx master+workers，与其余两栈的"应用进程树"口径不同。

- 空闲 PSS capsid 为 Python 3 栈的 **1/5**（12.3 vs 62.6 MB）；RSS 为
  PHP 8 栈的 1/5.7（21.8 vs 124.1 MB，后者口径偏大见上注）。
- 负载增量：capsid json c64 时 PSS +1.2 MB（QuickJS 堆随请求涨落），
  Python/PHP 侧无可见增量（2 tick 观察，样本少）。
