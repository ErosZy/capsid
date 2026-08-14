# 三栈极限调优对比（2026-08-13，docs/three-stack-bench-2026-08.md 的第二阶段）

双进程协议对比（第一阶段）见 `bench/results/three-stack-20260813T142447/`。
本阶段解除「每栈 2 执行单元」约束：**相同硬件预算（SUT taskset 0-5 六核、
loadgen 6-7 两核、128 并发）下，每栈自选配置**，先 mini-sweep 定配置、
再跑完整 12-workload 矩阵。

## 每栈调优菜单

| 栈 | 参数空间 | sweep 点 |
|----|---------|---------|
| capsid+hono | static-pool `--workers`（M2 池规模扫描已证本机 128 并发最优在 6-8w 超订区） | 4/6/8 |
| python+flask+gunicorn | sync worker 进程数（每进程独占一核时吞吐线性） | 2/4/6 |
| php-fpm+nginx+slim | `pm.max_children` + opcache/JIT（官方镜像默认带 opcache 扩展，JIT 待启用） | 2/4/6 |

应用层允许每栈做「预计算 + 最小请求路径」级别的合法优化（flask/slim 已
预计算字符串；hono 本阶段把 bytes/stream 的 Uint8Array 构造移到模块加载
时——只读复用，不改变每请求语义）。

## 方法

1. **sweep**：128 并发、json16k（中等 payload 代表格）、1 轮/配置、
   warmup 3s + measured 8s，取每栈最高均值 QPS 的配置。
2. **全矩阵**：sweep 胜出配置 × 12 workloads × 3 轮，协议与第一阶段相同
   （轮转、correctness 门、CV≤7% 结论门槛）。
3. **对比**：调优矩阵 vs 调优矩阵；并与第一阶段双进程矩阵并列报告。

## 明确不做

- 不换服务器实现（python 不换 uvicorn/uvloop，php 不换 swoole/roadrunner，
  capsid 不换 single-worker）——「调优」不是「换栈」。
- 不调内核/网卡参数（sysctl、REUSEPORT 策略等），不超 SUT 六核预算。
- 不做 64k 档与 conns=1 腿（与第一阶段一致）。
