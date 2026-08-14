# Capsid Host / Worker 完成度审计与性能报告（2026-08-14）

## 结论

以 `dd2e05b1b1cb0e489bd436481c1504062b08557b` 为审计基线，对照
`docs/capsid-remediation-execution-spec-2026-08-09.md` 的 WP-00 至 WP-09、
对应实现、测试注册和 Linux Release 门逐项检查后，未发现尚未实现的 Host 或
Worker 工作包。当前实现可判定为完成；本轮继续工作属于性能优化，而不是补齐功能。

本轮没有修改 `vendor/quickjs`、`vendor/txiki.js`、txiki overlay 或相关补丁。
优化只发生在第一方 Host 调度、日志和 HTTP 响应写出路径；Worker 二进制参与了
完整协议、回归和 TSan 验证，但未修改 JS 引擎实现。

## 发现与优化

perf 的最显著第一方热点是 `StructuredLog::writer_run()` 在空队列时持续加锁轮询。
基线 c1 / JSON 1 KiB 中，`pthread_mutex_lock`、`writer_run`、
`pthread_mutex_unlock` 合计占 56.82% CPU 采样。优化后改为条件变量等待，并将
实际为 LIFO 的 `vector::back/pop_back` 改为符合日志语义的 FIFO `deque`。

第二个热点是正常 `RESPONSE_END` 后，Host 仍向已经终止的 Runtime 请求提交一次
冗余 `CANCEL` 并唤醒 worker。现在由 `retire_terminal_request()` 在 Host 本地清理
共享命令队列；worker 线程保留 tombstone 到当前本地批次排空，覆盖“命令已经从
共享队列 swap 出去”的竞态，然后再回收 tombstone。断连、超时和失败路径的真实
cancel 语义保持不变。

第三个热点来自大流式响应。Worker 的 4 KiB `ResponseBody` IPC 帧可能已经连续
排在 Host 的有界队列中，但 Host 仍逐帧调用 Beast `async_write`，因此 64 KiB
流式响应最多产生 16 次独立 body 写出。现在静态池与 managed listener 共用一个
64 KiB 上限的合并器：只合并相邻且 `credit_returned_early` 状态相同的已排队帧，
不跨 credit 边界、不扩大已有响应窗口，也不改变 IPC 格式或背压记账。

为保证 benchmark 有效，还补齐了 15 个固定矩阵路由（JSON / bytes / stream ×
1/4/16/32/64 KiB）、逐字节正确性校验、load-generator CPU 计量、测量阶段标记和
可复用 perf 采集脚本。

## 测量方法

- 环境：Colima Linux 6.8 / Ubuntu 24.04，Intel i5-7360U，4 vCPU，4 GiB。
- 服务固定到 CPU 0-1，load generator 固定到 CPU 2-3；Capsid 4 workers。
- Release/GCC 13；baseline image
  `sha256:ef774987...`，optimized image `sha256:2905a094...`。
- 全矩阵：15 workloads × c1/c64 × 2 rounds，warmup 2 s，measure 5 s。
- 三轮确认：JSON 1 KiB、bytes 64 KiB、stream 64 KiB × c1/c64 × 3 rounds。
- perf：`cpu-clock` 299 Hz，DWARF call graph，同时采集 Host 与 4 个 Worker PID。
- 所有正式运行校验状态码、精确长度、完整 body/hash、错误、超时和 OOM；错误不计为
  成功请求。

全矩阵 baseline/optimized 共 120 个运行样本，三轮确认 36 个样本，三栈对照
36 个样本；全部为 0 errors、0 timeouts、0 mismatches、0 OOM。

## A/B 结果

全矩阵几何聚合如下。p99 和 PSS 是各 workload 百分比变化的中位数；虚拟机内仅
两轮，适合观察整体方向，不单独作为小幅变化的显著性证明。

| 并发 | QPS | CPU / request | p99 | peak PSS |
|---|---:|---:|---:|---:|
| c1 | +3.4% | **-57.9%** | **-28.8%** | -0.2% |
| c64 | **+23.5%** | **-26.6%** | **-15.8%** | -0.1% |

三轮无干扰确认用于校准代表负载和两轮方差：

| workload | 并发 | QPS（前 → 后） | CPU ms/request（前 → 后） | p99（前 → 后） |
|---|---:|---:|---:|---:|
| JSON 1 KiB | c64 | 2164.7 → 2551.1 (**+17.9%**) | 0.909 → 0.743 (**-18.3%**) | 70.76 → 60.04 ms (**-15.1%**) |
| bytes 64 KiB | c64 | 896.0 → 1220.7 (**+36.2%**) | 2.049 → 1.382 (**-32.6%**) | 291.94 → 133.41 ms (**-54.3%**) |
| stream 64 KiB | c64 | 620.0 → 732.2 (**+18.1%**) | 3.105 → 2.518 (**-18.9%**) | 254.93 → 206.89 ms (**-18.8%**) |
| JSON 1 KiB | c1 | 688.3 → 643.8 (-6.5%) | 2.524 → 1.101 (**-56.4%**) | 5.31 → 4.37 ms (**-17.7%**) |
| bytes 64 KiB | c1 | 481.2 → 514.1 (**+6.8%**) | 3.583 → 1.423 (**-60.3%**) | 7.18 → 4.86 ms (**-32.3%**) |
| stream 64 KiB | c1 | 266.6 → 321.7 (**+20.7%**) | 6.813 → 2.676 (**-60.7%**) | 10.06 → 6.57 ms (**-34.7%**) |

c1 / JSON 1 KiB 的吞吐在不同独立批次为 -10.6% 至 +3.7%，所以不能声称该单元
吞吐提升；但 CPU/request 与 p99 的改善在正式矩阵和三轮确认中方向一致。c64 的
三项代表负载与独立 perf 运行方向一致。

### 大响应 / streaming 第二轮

在上述 optimized image（`sha256:2905a094...`）与响应块合并 image
（`sha256:091199c2...`）之间，单独对 Capsid 运行 5 workloads × c1/c64 ×
3 rounds，共 60 个正式样本；全部通过精确 body、长度、hash 和 framing 校验，
0 errors、0 timeouts、0 mismatches、0 OOM。表中均为三轮中位数：

| workload | 并发 | QPS（前 → 后） | CPU ms/request（前 → 后） | p99（前 → 后） |
|---|---:|---:|---:|---:|
| stream 16 KiB | c1 | 514.1 → 539.9 (**+5.0%**) | 1.558 → 1.450 (**-6.9%**) | 4.65 → 4.63 ms (-0.6%) |
| stream 16 KiB | c64 | 1464.8 → 1543.8 (**+5.4%**) | 1.246 → 1.211 (**-2.8%**) | 91.33 → 104.71 ms (+14.7%) |
| stream 32 KiB | c1 | 423.2 → 453.6 (**+7.2%**) | 1.932 → 1.730 (**-10.5%**) | 5.39 → 5.59 ms (+3.5%) |
| stream 32 KiB | c64 | 1087.1 → 1186.9 (**+9.2%**) | 1.674 → 1.526 (**-8.9%**) | 129.70 → 107.30 ms (**-17.3%**) |
| stream 64 KiB | c1 | 330.3 → 382.7 (**+15.9%**) | 2.596 → 2.138 (**-17.7%**) | 6.15 → 5.80 ms (**-5.6%**) |
| stream 64 KiB | c64 | 708.1 → 841.0 (**+18.8%**) | 2.568 → 2.149 (**-16.3%**) | 178.83 → 143.21 ms (**-19.9%**) |
| bytes 64 KiB | c1 | 520.4 → 523.9 (+0.7%) | 1.406 → 1.403 (-0.2%) | 4.86 → 5.06 ms (+4.2%) |
| bytes 64 KiB | c64 | 1145.7 → 1248.6 (**+9.0%**) | 1.454 → 1.343 (**-7.6%**) | 173.35 → 134.17 ms (**-22.6%**) |
| JSON 64 KiB | c1 | 430.5 → 422.9 (-1.8%) | 1.787 → 1.822 (+1.9%) | 5.55 → 5.84 ms (+5.4%) |
| JSON 64 KiB | c64 | 978.3 → 1008.1 (+3.0%) | 1.809 → 1.763 (-2.5%) | 159.55 → 171.46 ms (+7.5%) |

stream 16 KiB / c64 的 p99 单元存在回退，但三轮原始值的分布重叠且 QPS、
CPU/request 同时改善；32/64 KiB 的收益随帧数增加而扩大，符合减少独立 HTTP
写出的预期。JSON 64 KiB 对照基本停留在批次方差范围内。bytes 64 KiB 也能受益，
因为它在 Worker → Host 路径同样可能由多个 response-body IPC 帧组成。

## perf 因果验证

| profile | 基线可见热点 | 优化后 |
|---|---:|---:|
| JSON 1 KiB / c64 | lock 5.59% + writer 5.50% + unlock 5.08% | lock 0.44%；writer/unlock 低于 0.25% 报告阈值 |
| stream 64 KiB / c64 | lock 7.77% + writer 6.76% + unlock 5.26% | unlock 0.26%；writer/lock 低于阈值 |
| JSON 1 KiB / c1 | lock 19.14% + writer 19.11% + unlock 18.57% | lock 0.32%；writer/unlock 低于阈值 |

这表明 CPU 收益来自已修改的第一方 Host 日志忙等消失，而不是 QuickJS/txiki.js
变化。第一轮优化后，大响应的剩余主要成本转到 Worker 执行及 Host streaming
写入/背压，因此第二轮选择 stream 64 KiB 作为第一方优化方向。

响应块合并后的独立 stream 64 KiB / c64 profile 为 853.9 req/s，合并前独立
profile 为 757.3 req/s（+12.8%）。Host 的
`_raw_spin_unlock_irqrestore` 从 2.68% 降到 1.29%；`sendmsg` 与
`write_body_block` 在合并前 flat report 中可见，合并后均低于 0.25% 报告阈值。
这为三轮吞吐/CPU 结果提供了与改动机制一致的热点证据。

## 三栈代表性对照

相同协议、镜像约束和 load generator 下，c64 中位数：

| workload | Capsid | Flask | PHP Slim | Capsid peak PSS |
|---|---:|---:|---:|---:|
| JSON 1 KiB | 2434.0 req/s | 1833.4 | 1584.6 | 24.7 MiB |
| bytes 64 KiB | 1194.0 req/s | 1509.1 | 1115.3 | 39.5 MiB |
| stream 64 KiB | 739.1 req/s | 1131.5 | 907.4 | 31.9 MiB |

这不是框架“胜负榜”：实现语言、服务器模型不同。它只用于确认量级，并暴露 Capsid
大流式响应相对弱、常规 JSON 高并发相对强的形态。

## 验证门

- Linux Release：330/330 passed；另 4 项按环境条件 skip（namespace/cgroup 和
  A/B evidence fixture），0 failed。
- build 容器 Node 18 无 `File`/完整 Web API 参考环境的 7 个 framework differential
  用例，在本机 Node 26 环境单独复跑为 7/7 passed。
- 优化影响面的 Linux 定向门：34 passed、1 environment skip、0 failed。
- TSan：GCC 13 Debug 插桩；在 VM 层用 `setarch x86_64 -R` 满足地址空间前置条件，
  19/19 受影响并发用例 passed，0 ThreadSanitizer reports。
- 协议 smoke：Capsid 15/15 路由；三栈 45/45 路由，长度、hash、content type、
  fixed/chunked framing 全部一致。
- 响应块合并定向 Release 门：credit 合并边界、静态池生命周期/并发 start-stop、
  activation/worker-exit isolation、managed listener contract 共 11/11 passed。
- 响应块合并镜像协议 smoke：Capsid 15/15 路由 passed；同提交构建的 Host/Worker
  compatibility ID 一致。

## 产物

- 全矩阵基线：`bench/results/profile-20260814/baseline-formal-v3/`
- 全矩阵优化版：`bench/results/profile-20260814/optimized-formal/`
- 三轮确认：`baseline-confirm-3r/`、`optimized-confirm-3r/`
- perf：`baseline-profile-*`、`optimized-profile-*`
- 响应块合并三轮确认：`stream-v1-confirm-3r/`、`stream-v2-confirm-3r/`
- 响应块合并 perf：`stream-v2-profile-stream64k-c64/`
- 三栈对照：`three-stack-formal-optimized/`

每个正式目录包含环境快照、image ID/digest、逐运行 JSONL、正确性证据、summary
JSON/CSV 和 `REPORT.md`；perf 目录包含 `perf.data`、flat/callgraph 报告、PID、
loadgen 样本和 correctness 结果。
