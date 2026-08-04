# 响应队列饱和：活性修复设计（queue-saturation activity fix）

状态：设计稿（RED 阶段）
关联缺陷：`64 并发 × >64KiB 响应` 触发 `max_queued_bytes` 上限 →
`capsidResponseWrite` 抛 RangeError → 响应不完整 → 请求挂死 20s+ →
吞吐崩塌（见 bench 排查记录，json64k 46 QPS 实为 4MiB 边界问题，
与 JSON.stringify 无关）。

## 1. 缺陷定义

这是**正确性/活性缺陷**，不是性能优化：

- 合法响应因并发组合失败（`pending_response_bytes_` 全局累计超 4 MiB）
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
    bool coalescing_started;
    std::deque<PendingWrite> pending;      // 元数据有界，数据在 JS heap
    std::optional<TerminalPending> terminal; // 新：延迟 terminal
    bool pump_advanced;                    // 新：本轮 pump 已推进（round-robin）
};
```

**关键：`PendingWrite` 保存 JSValue 引用而不是复制数据。** QuickJS 的
ArrayBuffer 不移动（非移动 GC），`JS_DupValue` 防回收；pump 时
`JS_GetUint8Array` 重新取指针、从 `offset` 处逐段写入 wire。native
pending 只占元数据（约 64 字节/段），不会随数据大小增长 ——
"不在压力出现时复制整块数据进入无界 native pending"。

### 3.3 write 路径（js_response_write）

```
EnqueueResult queue_response_bytes(id, bytes, size, state):
    if size == 0: return kQueued
    # 不再有 size > max_queued_bytes 的整体拒绝
    if state.credit > 0 && wire 空间够:
        尝试直接入 wire（分段 ≤ kMaxPayloadSize，累计 ≤ max_queued_bytes）
        发走 min(credit, 空间) 字节；credit 记账
    # 剩余字节 → PendingWrite（JS 引用 + offset）
    pending.push_back({dup(chunk), offset=已发, size, resolve, reject})
    pump_response_output()
    return kWouldBlock   # promise 挂起；数据被引用保活
```

- 大块（>4 MiB 单块）不特殊处理：pump 逐段推进（每段
  min(credit, kMaxPayloadSize, wire 剩余空间)），promise 在
  `offset == size` 时 resolve。
- 整块数据全部进入 wire（output_ / coalesced）后 resolve
  （契约 #4 的"有界接纳"）。
- 正常压力下永远不抛 RangeError（契约 #3）。

### 3.4 统一推进：pump_response_output()

**触发点**（任一发生即调用）：
1. `flush_output()` 后 output 清空（socket 释放空间）
2. `add_response_credit()`（credit 到达）
3. 新 write 入队 / terminal 设置
4. cancel / timeout 清理后

**行为**（round-robin，契约 #7）：
```
pump_response_output():
    start = 上次轮转位置
    for 每个 response（从 start 起，环形）:
        if state.pump_advanced: continue
        # 1) coalesced 缓冲 → wire（空间够时）
        # 2) pending 推进一个 quantum（≤ kMaxPayloadSize）：
        #    分段写入 wire，逐段扣 credit；全部入 wire 则 resolve promise
        # 3) pending 空 && terminal 存在：
        #    - kResponseEnd: 发 ResponseEnd 帧 → erase + remember_terminal
        #    - kResponseError: 丢弃未发 body → 发 Error 帧 → erase
        # 4) credit==0 || wire 满 → 标记停止（等下一次 pump 触发）
        state.pump_advanced = true
    重置 pump_advanced
```

- 每轮每个 request 最多推进一个 quantum —— 大响应不能占住整个
  wire 队列（契约 #7）。
- 单请求字节顺序由 `offset` + 单段内顺序保证。
- wire 高水位：`has_output_capacity()` 保持按
  `output_ + coalesced ≤ max_queued_bytes` 检查（契约 #8）。

### 3.5 terminal 保障（契约 #5、#6）

| 入口 | 行为 |
|---|---|
| `js_response_end` | pending 空且 coalesced 清空 → 立即发帧；否则 `terminal = kResponseEnd`（有界元数据），pump 时发。**不再抛 "response is not ready to end"。** |
| `js_response_error` | 丢弃 pending（reject_pending）+ 丢弃 coalesced → `terminal = kResponseError(msg, flags)`，pump 时发 Error 帧。**不再依赖 flush_coalesced 成功。** |
| `send_error`（native 超时等） | **不得忽略 enqueue 结果**：失败 → 存 terminal 元数据 → pump 重试。 |

- 每请求精确一个 terminal：response 在 terminal 帧进入 wire 后才
  `erase` + `remember_terminal`。
- 同一请求的 PendingWrite JSValue（chunk/resolve/reject）在
  `cleanup_response()` 统一释放，且只释放一次（erase 时调用，
  terminal 路径与 cancel/timeout/shutdown 路径共用）。

### 3.6 cancel / timeout / shutdown 清理（契约 #6）

- cancel/timeout：`cleanup_response()` 释放所有 PendingWrite 的
  JSValue（reject 后 free）、terminal 元数据、coalesced 缓冲，
  然后 erase response。
- shutdown：`reject_pending`（现有语义）+ 同一 cleanup。
- 所有路径幂等：response 已 erase 则不再触碰。

## 4. 影响面

- `src/worker_runtime.cc`（write/end/error/send_error/pump 核心）
- `js/bootstrap.js`（无需改动 —— 已 `await capsidResponseWrite`，
  promise pending 天然形成背压；仅验证 catch 路径仍正确）
- `tests/test_response_queue_saturation.cc`（新 RED binary）
- `tests/fixtures/queue-saturation.js`（新 fixture）
- `tests/test_p0_boundaries.cc`（`test_configured_limits`：queue
  limit 不再解释为 response-size limit，large-chunk 改为成功断言）
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
