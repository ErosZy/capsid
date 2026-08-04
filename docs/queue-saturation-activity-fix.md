# 响应队列饱和：活性修复设计（queue-saturation activity fix）

状态：已实现（commit 3f7e8b2 主修复 + 返修批次 2026-08-04）
关联缺陷：`64 并发 × >64KiB 响应` 触发 `max_queued_bytes` 上限 →
`capsidResponseWrite` 抛 RangeError → 响应不完整 → 请求挂死 20s+ →
吞吐崩塌（见 bench 排查记录，json64k 46 QPS 实为 4MiB 边界问题，
与 JSON.stringify 无关）。

## 1. 缺陷定义

这是**正确性/活性缺陷**，不是性能优化：

- 合法响应因并发组合失败（wire 队列累计超 4 MiB，64 并发 × 65 KiB 触发）
- 失败后响应不产生 terminal event，连接被长期占用（hang 到 client
  30s timeout）
- 错误链：`capsidResponseWrite` RangeError → bootstrap catch →
  `capsidResponseError`；但 `js_response_error` 在队列满时也失败、
  `send_error` 忽略 enqueue 失败、`js_response_end` 在队列满时抛错
  —— 饱和时无法保证请求产生 terminal。

## 2. 冻结契约

1. `max_queued_bytes` 是 worker 本地 IPC/wire 队列硬上限，不是单响应
   或所有响应总长度上限。
2. 任意合法响应可以大于该上限，通过 credit 分段传输。
3. 正常队列压力只能让 `capsidResponseWrite()` Promise pending，
   不能抛 RangeError。
4. Promise 在整块数据被有界接纳后 resolve；cancel、timeout、shutdown
   才 reject。
5. 每个请求必须精确产生一个 terminal：ResponseEnd、Error 或 Timeout。
6. terminal frame 暂时放不下时延迟发送，绝不能丢弃或递归抛错。
7. 多个等待响应采用 round-robin；大响应不能饿死小响应。
8. native outbound/coalesced 高水位始终不超过 `max_queued_bytes`。
9. 不修改 wire protocol，不提高 4 MiB 默认值来掩盖问题。

## 3. 状态机

### 3.1 三态返回

`queue_response_bytes()` 从含糊的 bool 改为三态：

```cpp
enum class EnqueueResult {
    kQueued,     // 数据进入 wire 队列（全部或部分）
    kWouldBlock, // 无空间/无 credit：promise 挂起，等 pump 推进
    kFatal,      // 协议/状态不一致：fail-closed，终结请求
};
```

### 3.2 数据结构

```cpp
struct PendingWrite {
    JSValue chunk;   // 持有 Uint8Array 引用（JS_DupValue，JS heap 保活）
    size_t offset;   // 已推进偏移
    size_t size;
    JSValue resolve;
    JSValue reject;
};

struct TerminalPending {
    enum class Kind { kResponseEnd, kResponseError };
    Kind kind;
    std::string message;    // kResponseError：错误文本
    uint32_t error_flags;   // kResponseError：kErrorFlagTimeout 等
};

struct ResponseState {
    uint64_t credit;
    uint64_t request_credit;
    uint64_t response_body_bytes_accepted;
    uint64_t deadline_ns;
    bool request_ended;
    std::deque<PendingWrite> pending;      // 元数据有界，数据是调用时快照
    TerminalPending terminal;              // 延迟 terminal（flag 标记）
    bool terminal_pending;
    ResponsePhase phase;                   // kOpen / kEndPending / kFailurePending
};
```

**已实现：`PendingWrite` 保存调用时快照。** `js_response_write` 在
`kWouldBlock` 路径用 `JS_NewUint8ArrayCopy` 在 JS heap 复制字节；
且 stream 层 enqueue 即 detach 原 ArrayBuffer —— 应用在 promise
pending 期间修改源数组不可能改变响应字节（冻结"调用时快照 +
所有权转移"语义，见测试 #13）。native pending 只占元数据。

### 3.3 write 路径（js_response_write）

```
EnqueueResult queue_response_bytes(id, bytes, size, state):
    if size == 0: return kQueued
    # 不再有 size > max_queued_bytes 的整体拒绝
    if state.credit > 0 && wire 空间够:
        尝试直接入 wire（分段 ≤ kMaxPayloadSize，累计 ≤ max_queued_bytes）
        发走 min(credit, 空间) 字节；credit 记账
    # 剩余字节 → PendingWrite（调用时快照 + offset）
    pending.push_back({snapshot(chunk), offset=已发, size, resolve, reject})
    pump_response_output()
    return kWouldBlock   # promise 挂起；数据被引用保活
```

- 大块（>4 MiB 单块）不特殊处理：pump 逐段推进（每段
  min(credit, kMaxPayloadSize, wire 剩余空间)），promise 在
  `offset == size` 时 resolve。
- 整块数据全部进入 wire（output_ / coalesced）后 resolve
  （契约 #4 的"有界接纳"）。
- 正常压力下永远不抛 RangeError（契约 #3）。

### 3.4 统一推进：pump_response_output()（已实现）

**触发点**（任一发生即调用）：
1. `flush_output()` 后 output 清空（socket 释放空间）
2. `add_response_credit()`（credit 到达）
3. 新 write 入队 / terminal 设置
4. cancel / timeout 清理后

**行为**（持久轮转，契约 #7）：
```
pump_response_output():
    rounds = pump_order_.size()          # 跨 pump 保留的 deque
    for i in 0..rounds:
        id = pump_order_.front(); pump_order_.pop_front()
        if responses_.find(id) 不存在: continue   # 完成/取消：drop
        advance_pending(state, id, kMaxPayloadSize)
        if pending 空 && terminal_pending:
            try_send_terminal → 成功: done（erase + remember）
            continue                            # 不回队
        pump_order_.push_back(id)       # 回队尾：真正的轮转
```

- **跨 pump 持久轮转**：每轮从队头处理一圈，处理完回队尾 ——
  下一轮起点即本轮结束处。低 ID 大响应不可能每轮抢占首位，
  高 ID 小响应在固定 quantum 数内完成且先于大响应（测试 #12）。
- 每请求每轮一个 quantum（≤ kMaxPayloadSize）。
- 单请求字节顺序由 `offset` + 单段内顺序保证。
- wire 高水位：`has_output_capacity()` 保持按
  `output_ ≤ max_queued_bytes` 检查（契约 #8）。

**可注入 OutboundBuffer（返修 #4）：** `src/outbound_buffer.h` 提供
帧粒度的发送缓冲（逻辑队列 = storage - write_offset_），writer 可注入
（生产为 socket，单测注入 short-write/EAGAIN）。flush 逐帧发送，
`frame_start_` 保留当前帧已发送的头（partial write 可停在帧头中间），
compact 删除 `frame_start_` 之前的完整帧 —— 物理存储 ≤ 一帧 +
wire 上限（`tests/test_outbound_buffer.cc` 精确断言 logical 与
storage 高水位）。

### 3.5 terminal 保障 + 生命周期阶段（契约 #5、#6，返修 #1/#3）

| 入口 | 行为 |
|---|---|
| `js_response_end` | pending 空 → 立即发帧；否则 `terminal = kResponseEnd`（有界元数据），pump 时发。**不再抛 "response is not ready to end"。** |
| `js_response_error` | 丢弃 pending → `terminal = kResponseError(msg, flags)`，pump 时发 Error 帧。 |
| `send_error`（native 超时等） | **不得忽略 enqueue 结果**：失败 → 存 terminal 元数据 → pump 重试。 |

**错误消息截断（返修 #1）：** error payload 上限 =
`min(kMaxPayloadSize, max_queued_bytes - kHeaderSize)` —— 长错误消息
在 4 KiB 队列下截断为单帧可容纳，绝不永久挂起（测试 #9）。

**显式阶段（返修 #3）：** `ResponsePhase { kOpen, kTerminalPending }`。
expire 只处理 `kOpen` 且首次超时即清除 deadline + 转换阶段 ——
队列饱和时每个 timer tick 不再重复执行 cancel bridge / send_error /
drain_jobs；cancel 一次、terminal 一次（测试 #11 冻结）。

- 每请求精确一个 terminal：response 在 terminal 帧进入 wire 后才
  `erase` + `remember_terminal`。
- PendingWrite 的 JSValue（快照/resolve/reject）在 `resolve_pending`
  （完成）或 `reject_pending`（cancel/timeout/shutdown/error）释放，
  且只释放一次：terminal 发送或请求终结时 erase response，同一请求
  的 JS 值不会再被触碰。

### 3.6 cancel / timeout / shutdown 清理（契约 #6）

- cancel：`reject_pending`（释放所有 PendingWrite 的 JSValue）→
  cancel bridge → erase → `remember_terminal` → pump（清理轮转）。
- timeout：先清 deadline + 转 kFailurePending → `reject_pending` →
  cancel bridge → `send_error`（terminal 保障）→ 后续 tick 跳过。
- shutdown：`reject_pending`（现有语义）。
- 所有路径幂等：response 已 erase 则不再触碰。

## 4. 影响面

- `src/worker_runtime.cc`（write/end/error/send_error/pump/flush/compact）
- `js/bootstrap.js`（无需改动 —— 已 `await capsidResponseWrite`）
- `tests/test_response_queue_saturation.cc`（14 场景 RED binary）
- `tests/fixtures/queue-saturation.js`（fixture）
- `tests/test_p0_boundaries.cc`（`test_configured_limits` 语义）
- 不修改公共 ABI / 协议版本 / 4 MiB 默认值。

## 5. RED 测试清单

| # | 场景 | 断言 |
|---|---|---|
| 1 | max_queued=4096，单响应 20000 B | 100% 完成 + 内容一致 |
| 2 | max_queued=4096，单 chunk 8193 B | 分段完成（8193 > queue） |
| 3 | max_queued=4 MiB，64 并发 × 65537 | 全部完成 + 内容一致 |
| 4 | 对照：64 并发 × 65536 | 全部完成（不回归） |
| 5 | 饱和后应用抛错 | 每请求 1s 内收到 terminal |
| 6 | 大响应 + 小响应并发 | 小响应不被饿死 |
| 7 | pending 中 cancel / timeout | 无泄漏 / UAF（ASan 验证），worker 可复用 |
| 8 | 压力场景后 /small | worker 仍可服务 |

## 6. 验收门

- Release / ASan / UBSan / TSan 全绿
- 64c / 65537：零错误、零超时、零排空挂死
- 65537 QPS ≥ 65536 的 90%；63c 与 64c 差异 ≤ 10%
- 1 KiB / 16 KiB workload 不回退超过 5%；p99 无秒级尾巴
- 三轮交错 A/B，保留 worker/host profile 与队列高水位
- profile 证明时间由正常 credit/IPC 推进构成，无异常重试/重建

## 7. 明确禁止的"修复"

- 把 4 MiB 改大
- 把默认并发降到 63
- 超限后快速返回 500/503
