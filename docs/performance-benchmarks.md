# 性能结论

本文只保留当前权威结果和解释边界。运行协议见
[`bench/README.md`](../bench/README.md)，原始数据和自动报告位于
[`bench/results/`](../bench/results/)。

## 如何解读

仓库有五类不能混写的测量：

1. Python/Flask、Ruby/Sinatra、PHP/Slim 与 Capsid/Hono 的完整 HTTP stack；
2. 四个完整 stack 对同一个只读 SQLite 数据库执行 indexed query；
3. Capsid 源码/可信 QuickJS 字节码与 Deno Web Worker 的冷启动；
4. 单个 `capsid-worker` 的产物、PSS/RSS 和 QuickJS heap；
5. Capsid 与 Deno 的 Vue SSR + SQLite worker 扫描。

完整 stack 内存取整个容器 cgroup；单 worker 密度取应用进程 PSS。二者回答
不同问题，不能拿 PHP-FPM 单进程和 Capsid gateway + worker 池互相比较。
所有数字只适用于固定版本、硬件和协议；QPS 必须与 p95/p99、错误、drain、
CPU、内存和内容校验一起解释。

## 产品定位

现代完整 stack 结果不支持“Capsid 普遍快于 Python/Ruby/PHP”。更准确的定位是：
可嵌入、能力可审计、进程强隔离、资源边界明确的 JavaScript runtime。

当前证据支持两项相对优势：

- 单个已预热 `capsid-worker` 的 PSS 低于本轮 PHP-FPM、Python Gunicorn 和
  Ruby Puma 应用 worker；
- parse-heavy 大 bundle 使用同构建可信 QuickJS 字节码时，冷启动明显改善。

它不支持“Capsid 完整 HTTP stack 内存最低”或“吞吐普遍领先”。

## 完整 HTTP stack

权威结果：
[`external-compare-modern-r39-20260730`](../bench/results/external-compare-modern-r39-20260730/report.md)。

固定版本为 Python 3.14.6 / Flask 3.1.3 / Gunicorn 26.0.0、Ruby 4.0.6 /
Sinatra 4.2.1 / Puma 8.0.2、PHP 8.5.8 / Slim 4.15.2 / PHP-FPM /
nginx 1.26.3，以及 Capsid L2 / Hono 4.12.32。576 个 measured sample 共完成
5,912,606 个响应，失败、内容错误、丢弃、超时和取消均为 0。

三轮中位数：

- Ruby 在 48/48 个 cell 中吞吐第一，但内存最高；
- Capsid/Python QPS 比值中位 0.61，Capsid 无胜场；
- Capsid/Ruby QPS 比值中位 0.42，Capsid 无胜场；
- Capsid/PHP QPS 比值中位 0.94，Capsid 在 14/48 个 cell 领先；
- PHP 在 64 KiB JSON 和 32/64 KiB stream 出现稳定断崖，但现有数据不能把
  原因归到 Slim、FPM、FastCGI 或 nginx 的某一层。

完整容器 cgroup measured peak 中位：

| vCPU | Python | Ruby | PHP/nginx/FPM | Capsid gateway/workers |
| ---: | ---: | ---: | ---: | ---: |
| 2 | 75.4 MiB | 144.7 MiB | **21.1 MiB** | 22.2 MiB |
| 4 | 115.1 MiB | 263.3 MiB | **28.3 MiB** | 38.4 MiB |

[单 worker 补测](../bench/results/external-compare-modern-r39-20260730/worker-memory.md)
把四组 stack 都配置为一个应用 worker，并在预热后读取 `smaps_rollup`：

| 应用 worker | PSS 中位 |
| --- | ---: |
| Capsid `capsid-worker` | **7.15 MiB** |
| PHP-FPM worker | 8.89 MiB |
| Python Gunicorn worker | 22.61 MiB |
| Ruby Puma worker | 29.88 MiB |

因此只能主张本轮单 worker 密度优势；完整 stack 内存最低的是 PHP。

## SQLite stack

权威结果：
[`sqlite-modern-r40-20260730`](../bench/results/sqlite-modern-r40-20260730/report.md)。

每次请求执行同一个 indexed query：

```sql
SELECT payload FROM payloads
WHERE size_bytes = ? AND slot = (random() & 255)
```

四个 stack 使用字节完全相同的 31,014,912-byte 数据库，BLOB 为
1/16/32/64 KiB，每档 256 行；SQLite page cache 固定为两个 4-KiB page，
host page cache 为 warm。

- 96 个 load sample、24 个容器轮次、742,005 个响应全部通过校验；
- Ruby 在 8/8 个 cell 吞吐第一，Python 在 8/8 个 cell 第二；
- Capsid/Python QPS 比值中位 0.46，Capsid/Ruby 为 0.38；
- Capsid/PHP QPS 比值中位 0.80；Capsid 只在 64 KiB 的两个 cell 领先；
- 完整容器 measured peak 中位仍是 PHP 最低；
- 各 stack 使用自身固定的现代 SQLite engine，所以这是 stack comparison，
  不是 same-engine microbenchmark，也不是物理磁盘延迟测试。

`CAPSID_BUILD_SQLITE_BENCHMARK` 默认关闭。Capsid benchmark build 只允许固定
只读数据库、禁止 extension，并启用 query-only authorizer；这不代表产品
`capsid:sqlite` 已开放。

## 冷启动

权威结果：
[`capsid-bytecode-startup-r41-20260730`](../bench/results/capsid-bytecode-startup-r41-20260730.md)。
文件名保留历史内部命名，产品仍称 Capsid。

每档 3 次预热、30 个样本。Capsid 包含 fresh OS process、HELLO、IPC 输入、
加载/执行、READY 和首响应；Deno 在常驻 controller 中创建 Web Worker，隔离
边界不同。

| 源码规模 | Capsid source | Capsid bytecode | source/bytecode | Deno Web Worker |
| ---: | ---: | ---: | ---: | ---: |
| 1 KiB | 8.239 ms | 8.333 ms | 0.99× | 15.634 ms |
| 10 KiB | 9.302 ms | 8.842 ms | 1.05× | 16.267 ms |
| 100 KiB | 20.826 ms | 11.138 ms | 1.87× | 22.030 ms |
| 1 MiB | 415.378 ms | 40.790 ms | 10.18× | 84.526 ms |

小 bundle 由进程固定成本主导；100 KiB 起解析收益明显。1 MiB fixture 由大量
有效函数声明构成，只能视为 parse-heavy 信号。QuickJS bytecode 不跨版本、
构建选项或架构稳定，也不是安全输入格式；只能加载宿主同构建生成并校验摘要的
可信产物。

## 单 worker 资源

权威结果：
[`linux-artifact-resource-r42-20260730`](../bench/results/linux-artifact-resource-r42-20260730/report.md)。

Release/LTO、system allocator、无 sanitizer，加载 Hono 4.12.32 真实 bundle：

- Hono bundle 22,264 bytes；
- worker 10,734,776 bytes，strip 后 4,534,824 bytes；
- 静态宿主库 882,860 bytes；
- READY 冷启动 median/p95：29.75/31.97 ms；
- READY PSS median/p95：6,053/6,103 KiB；
- 10 次请求后 PSS median/p95：6,346/6,424 KiB；
- 体积、启动、PSS、响应和 QuickJS heap 阈值全部通过。

该测量明确排除 Go gateway 和控制进程，只用于单 worker 容量规划。

## Vue SSR + SQLite

权威扫描：
[`vue-ssr-sweep-r3-20260731`](../bench/results/vue-ssr-sweep-r3-20260731/report.md)。

双方使用 Vue 3.5 SSR、同一个只读 SQLite 数据库和每请求 50 ms 人工 I/O
等待。closed-loop 每点预热 2 秒、计量 8 秒；open-loop 每点预热 5 秒、
发压 15 秒。健康检查必须返回 `sqlite: true`，每个计量响应必须包含
`x-sqlite: 1`。所有有效请求均通过内容检查，服务错误为 0。

### Deno 进程扫描（C=32）

| 进程数 | QPS | p50 | p95 |
| --- | ---: | ---: | ---: |
| 1p | 546.4 | 57.5ms | 70.2ms |
| 2p | 609.0 | 52.6ms | 55.6ms |
| 4p | 612.8 | 52.2ms | 54.0ms |
| 8p | 610.8 | 52.5ms | 54.9ms |

2p 后已经接近 `32 / 50ms = 640 QPS` 的 workload 上限。因此这个点不能证明
4p 是整机全局最优，只能说明 C=32 时继续增加进程没有收益。

### Capsid Worker 扫描（C=32）

| Workers | 三轮 QPS | 平均 | 中位数 | CV | 平均 p99 |
| ---: | --- | ---: | ---: | ---: | ---: |
| 12 | 475.9 / 476.9 / 428.8 | 460.5 | 475.9 | 6.0% | 90.2ms |
| **16** | **499.8 / 491.9 / 495.6** | **495.8** | **495.6** | **0.8%** | **85.1ms** |
| 20 | 501.0 / 475.1 / 484.0 | 486.7 | 484.0 | 2.7% | 106.8ms |
| 24 | 495.0 / 470.2 / 491.4 | 485.5 | 491.4 | 2.8% | 93.9ms |
| 28 | 491.4 / 480.5 / 481.2 | 484.4 | 481.2 | 1.3% | 95.6ms |
| 32 | 482.8 / 413.0 / 475.8 | 457.2 | 475.8 | 8.4% | 113.3ms |

全部轮次保留，没有事后剔除。**推荐 Capsid-16w**：三轮均值 495.8 QPS、
中位数 495.6、CV 0.8%，同时拥有最低的平均 p95/p99。

### 多并发扩展（Deno-4p vs Capsid-16w）

| 并发 | Deno-4p | Capsid-16w | Capsid/Deno | Capsid p50/p99 |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 154.0 | 140.5 | 91.2% | 57.1/61.8ms |
| 16 | 309.1 | 271.1 | 87.7% | 58.3/87.1ms |
| 32 | 616.6 | 497.6 | 80.7% | 64.4/81.5ms |
| 64 | 1218.6 | 723.0 | 59.3% | 88.1/123.2ms |
| 128 | 1760.8 | 748.4 | 42.5% | 165.8/397.1ms |
| 256 | 1749.9 | 762.5 | 43.6% | 353.3/465.2ms |

Capsid C64→C256 吞吐只增加 5.5%，p50 增加 4 倍，是容量平台与排队增长；
本轮没有复现 C64→C128 吞吐下降。Deno C128→C256 也已进入平台。

### 开放式到达速率（Capsid-16w）

旧数据用 465 QPS 作为“峰值”，但 closed-loop 已达到约 760 QPS，所以原
105% 点不是过载。本轮按实测平台重新校准：

- 600 QPS 连续两轮完成率 100%，0 dropped、0 服务错误，p99 分别
  102.7/179.5ms；
- 650 QPS 两轮结果不稳定，完成率为 100%/95%；
- 800/910 QPS 完成率只有 83.7%/79.8%，并出现近秒级延迟。

因此本机已重复验证的可持续点是 600 QPS；约 750–760 QPS 只是
closed-loop 吞吐平台，不是低延迟容量。

Deno 1225 QPS 时仍以 p50/p99 52.0/82.3ms 完成全部请求。1490 QPS 以上
Python 线程式 open-loop loadgen 的 schedule lag 已达数百毫秒到数秒，
所以这些负向文件不能用于 Deno 服务端过载归因。

本轮只确认完整 Capsid 路径在高并发下饱和，没有证据把它归因于 gateway。
gateway 使用并发 `net/http` handler；进一步归因必须同步采集 gateway/worker
CPU、queue wait、worker execution、IPC 和 scheduler 数据。

## 当前优化决策

保留：

- 每 service vCPU 8 个 connection/inflight；
- 默认调度器 placement；
- response 前 64 KiB 即时 framing，之后采用有界、credit-charged coalescing；
- SSE 与 `X-Accel-Buffering: no` 绕过 coalescing；
- opt-in IPC profile 只用于明确的 CPU/syscall/queue/scheduler 归因。

不进入当前路线：

- 固定 worker 绑核；
- 16 inflight/vCPU；
- 无证据的 zero-copy、`writev` 或 shared-memory ring；
- 默认 mimalloc；
- 盲目增加 worker 或 inflight。

新的性能改动必须由固定 workload 回归或 profile 热点触发，并同时通过
conformance、module/global surface、sandbox、cancel/credit 和完整 benchmark
回归。
