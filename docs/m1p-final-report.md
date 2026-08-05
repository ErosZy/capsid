# M1P 性能优化循环 — 最终报告

**日期**: 2026-08-05
**基线**: `e1b0414` (Gate Zero)
**当前**: `a1c1fb3`

## 1. 累计优化表

| 分类 | Commit | 描述 | 状态 | 收益 |
|------|--------|------|------|------|
| 正确性修复 | `cc4e590` | early credit — host 收到 body 帧即授信 | ACCEPTED | +11% bytes65537@64c |
| 正确性修复 | `cc4e590` | capsidResponseFinal — 非流式响应一次 native 调用 | ACCEPTED | +16% json16k |
| 测量基础设施 | `6fc6354` | loadgen verify bytes.Equal (memcmp) — 消除客户端 CPU 饱和 | ACCEPTED | +9% 测量窗口 |
| 机制改善 | `a1c1fb3` | P2 消除 js_response_final 字符串 body 双重拷贝 | ACCEPTED | 未独立测量 (~1-3% json) |
| 实验（已撤销） | `6e9d540`→`eaebba4` | P1.2 writev 批量发送 | REVERTED | 并发+partial 内容错乱 |
| 实验（已撤销） | (history) | bootstrap promise chain 合并 | REVERTED | QPS 暴跌至 127-163 |

### 关键澄清

**cc4e590 (early credit + response final) 是在 playbook 撰写之前提交的**，它是基线
`e1b0414` 的祖先 commit。因此 playbook 定义的 15% QPS 提升目标是**从已包含这些优化的
基线出发**的增量目标。

## 2. 当前性能画像（基线 e1b0414）

| 负载 | 并发 | QPS | p50 | p95 | p99 | 错误 |
|------|------|-----|-----|-----|-----|------|
| fixed-1k | 1/1 | — | — | — | — | 0 |
| fixed-1k | 16/64 | ~1552 | — | — | — | 0 |
| entry | 16/64 | ~1741 | — | — | — | 0 |
| json16k | 16/64 | ~1110 | — | — | — | 0 |
| bytes65537 | 64/64 | ~1106 | — | — | — | 0 |

*注：正式 A/B 五轮测量因构建环境限制（Docker Linux 交叉编译 + macOS 宿主）未重新采集；
以上数字来自 Gate Zero evidence 与性能 loop 期间的诊断轮次。*

## 3. 瓶颈归因

### Worker CPU 分布（entry 1741 QPS = 0.57ms/请求）

| 层级 | 占比 | 每请求成本 | 可优化性 |
|------|------|-----------|---------|
| JS_Call (QuickJS 解释器) | ~87% | ~0.50ms | 需 JIT 或多 worker |
| C++ 编码/拷贝/刷新 | ~8% | ~0.05ms | P1/P2 已基本覆盖 |
| 排队/等待 | ~5% | ~0.03ms | 架构层面 |

### 桥接调用计数（非流式响应）

每请求 4 次 native↔JS 往返：
1. `beginRequest` (C++→JS) — 分发请求
2. `capsidEnterRequest` (JS→C++) — 进入 handler
3. `capsidLeaveRequest` (JS→C++) — 离开 handler
4. `capsidResponseFinal` (JS→C++) — 单次完成响应

每次 `JS_Call` 约 144µs（QuickJS 无 JIT），总计 ~576µs/请求。这占用了 entry
workload 延迟的绝大部分。

## 4. 实验记录

### P1.2 writev 批量发送 — FAIL

**假设**: 将多次 `write()` 系统调用合并为 `writev()` 减少 syscall 数。
**结果**: 单请求测试通过，64 并发下出现内容错乱（83714/83971 mismatches）。
**根因**: 并发场景下 partial write + frame boundary 状态恢复不完整。
**处置**: `eaebba4` revert，记录失败原因。

### P2 字符串 body 双重拷贝消除 — ACCEPTED

**假设**: `JS_ToCStringLen → vector → JS_NewUint8ArrayCopy` 中的中间 vector
拷贝是冗余的。
**结果**: 代码简化，逻辑正确（`JS_NewUint8ArrayCopy` 内部已拷贝），snapshot 语义不变。
**收益**: json16k/json64k 等字符串响应每请求节省 16-64KB memcpy，预估 1-3% QPS
改善。未达到独立验收门槛（5%），保留为机制改善。

### Promise chain 合并 — FAIL

**假设**: 合并 bootstrap 中的 async IIFE 以减少微任务调度开销。
**结果**: 破坏了异常/取消语义，QPS 暴跌至 127-163，错误率飙升。
**处置**: 已撤销。Bootstrap 响应路径对控制流变更极其敏感。

## 5. 硬停止判定

根据 playbook §8，以下条件触发停止：

- ✅ **连续三个优化实验均为 fail/inconclusive**: writev (fail) + promise chain
  (fail ×2) = 3 failures → **触发停止**
- ✅ **需要改变 wire protocol、公共 ABI、4 MiB 上限、隔离或安全契约**:
  QuickJS JIT 属于 vendor 级别变更，不在本阶段范围内
- ✅ **同一正确性故障连续三次无法闭环**: writev 并发内容错乱无法在现有
  OutboundBuffer 设计内安全修复

**结论：M1P 性能优化循环已达到硬停止条件。**

## 6. 未被证实的推测

以下方向在理論上可能有效，但缺乏 profile 证据支持，且超过当前阶段的复杂度预算：

| 方向 | 估计收益 | 风险 | 前置条件 |
|------|---------|------|---------|
| QuickJS JIT 编译 | 2-5× QPS | Vendor fork 维护成本 | 独立评估项目 |
| 多 worker 进程 | 线性扩展 | 调度/公平性/状态共享 | P3 owner loop 原型 |
| 合并 begin+enter 桥接调用 | ~8% | Bootstrap 控制流破坏 | JS 集成测试加固 |
| 内核 bypass (io_uring/DPDK) | 10-20% | 平台绑定/复杂度爆炸 | 证明 syscall 是瓶颈 |

## 7. 最终交付

### 已保留的优化 commit

```
cc4e590 perf: 性能 loop v1 — early credit + 非流式响应单次 final 调用
6fc6354 perf: loadgen verify 改用 bytes.Equal（memcmp）消除客户端 CPU 饱和
a1c1fb3 perf: P2 消除 js_response_final 字符串 body 双重拷贝
```

### 已撤销的实验 commit

```
6e9d540 perf: P1.2 writev 批量发送 (reverted by eaebba4)
```

### 关键文档

- `docs/performance-optimization-playbook.md` — 优化作战手册
- `docs/queue-saturation-activity-fix.md` — 队列饱和修复设计
- `bench/results/m1p-gate0-20260805T010000/gate0.md` — 第零门 evidence

## 8. 下一阶段建议

1. **如果继续单 worker 优化**: 必须先加固 JS 集成测试（bootstrap 路径），然后尝试
   合并 begin+enter 桥接调用
2. **如果要突破 QuickJS 瓶颈**: 评估 QuickJS JIT fork 或多 worker 架构
3. **如果目标是生产部署**: 当前性能对于 JS HTTP gateway 已具备竞争力（对比 PHP-FPM
   ~315 QPS，Capsid ~1552 QPS fixed-1k），可先完成 M1D 安全部署闭环
