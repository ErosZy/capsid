# 宿主嵌入与集成规范

本文面向直接链接 `libcapsid_runtime.a` 的网关、server、worker pool 和其他
embedder，合并了嵌入接口（C ABI）与集成规范（线程/事件循环/背压/ABI 升级）。

- 公开接口以 [`include/capsid/runtime.h`](../include/capsid/runtime.h) 为权威；
  完整符号与字段注释以 public header 为准。
- 当前 `CAPSID_ABI_VERSION == 7`；所有配置结构必须先调用对应的 `*_init()`，
  不要手工清零或猜测结构大小。
- Capsid Runtime 只提供隔离 worker 与 C ABI，不替宿主实现 HTTP 监听、调度、
  重试或应用发布。

## 生命周期

典型宿主流程如下：

1. `capsid_worker_config_init()` 后填写 `worker_path`、资源和安全策略；
2. `capsid_worker_spawn()` 创建 worker（同步校验并深拷贝所有嵌套配置字符串、
   规则和策略；返回后宿主可以释放原配置内存，要变更策略必须新建 worker）；
3. `capsid_worker_load_bundle()` 或 `capsid_worker_load_bundle_named()` 加载源码 bundle；
4. 在当前 POSIX ABI 上通过 `capsid_worker_fd()` 接入宿主 event loop；
5. 写入请求头/body/end，并按事件补充双向 credit；
6. 持续调用 `capsid_worker_next_event()` 消费响应、日志、审计和退出事件；
7. `capsid_worker_shutdown()` 后继续排空，最终调用 `capsid_worker_destroy()`。

创建后必须先发送 bundle，再等待 `CAPSID_EVENT_READY`；READY 前不得发请求。

### 可信字节码

`capsid_worker_load_trusted_bytecode_named()` 可跳过源码解析，加载由完全相同的
Capsid/QuickJS 构建生成的模块字节码。它是面向宿主构建流水线的可选优化，不是
应用上传格式。

QuickJS 字节码不保证跨版本、编译选项或架构兼容，反序列化器也不构成安全边界。
损坏、不匹配或攻击者控制的 bytes 可能导致内存破坏；因此只能加载宿主生成并
校验过摘要的可信产物，不能加载租户输入。`source_name` 还必须与编译时嵌入的
模块名完全相同。常规应用继续使用源码 bundle API。

## 线程与事件循环

一个 `capsid_worker` 同一时刻只能由一个宿主线程驱动。创建、写入请求、读取事件、
取消和销毁都必须串行；若网关有多个 I/O 线程，应把每个 worker 固定到一个
owner loop，通过线程安全队列把命令投递给 owner。

当前 POSIX ABI 上，`capsid_worker_fd()` 返回非阻塞 Unix socket。宿主应：

1. 有待发送数据时监听 writable，任何时候监听 readable；
2. writable 时反复调用 `capsid_worker_flush()`，直到 `CAPSID_OK` 或
   `CAPSID_WOULD_BLOCK`；
3. readable 时反复调用 `capsid_worker_next_event()`，直到
   `CAPSID_WOULD_BLOCK`；
4. 收到 `CAPSID_CLOSED`、`CAPSID_PROTOCOL_ERROR` 或 `CAPSID_EVENT_EXIT` 后停止复用
   该 worker，并按应用发布策略替换它。

大部分 I/O API 可能返回 `CAPSID_WOULD_BLOCK`，这是正常背压信号，不是 worker
故障或忙循环重试的理由。返回 `WOULD_BLOCK` 的操作不会提交半个逻辑帧。

不要在共享 reactor 上阻塞等待单个 worker。启动、请求和 shutdown 都应有宿主
deadline；worker 内置 deadline 是第二道边界，不替代网关 deadline。

`capsid_worker_fd()` 是 ABI v7 的 POSIX 集成面，不应被解释为永久的跨平台抽象。
跨平台宿主应使用自己的 worker-event-source adapter，不要让业务层依赖 Unix fd。
Capsid 将保留 ABI v7 的 fd 路径，并以加法接口建立 Windows 原生 event source；
在该接口与 Windows 构建测试交付前，不得声称 Runtime 已提供 Windows 嵌入支持。
平台支持层级见[架构与产品边界](architecture.md#平台契约)。

## 请求、credit 与背压

request ID 在一个 worker 内必须非零且唯一，直到 response end、error、timeout
或 cancel 完成。请求顺序为：

```text
begin_request
  → 收到 REQUEST_CREDIT 后 write_request（可多次）
  → end_request
  → RESPONSE_HEAD / RESPONSE_BODY / RESPONSE_END
```

1. `capsid_worker_begin_request()`；
2. 只在 `CAPSID_EVENT_REQUEST_CREDIT` 授予的额度内调用
   `capsid_worker_write_request()`；
3. `capsid_worker_end_request()`。

响应 body 同样受 credit 控制。宿主消费或成功转发一段
`CAPSID_EVENT_RESPONSE_BODY` 后，才用
`capsid_worker_grant_response_credit()` 归还对应字节。不能提前按预计消费量授信，
否则下游慢客户端会把内存压力转移到宿主。credit 按 request ID 隔离，全局 queue
budget 不能代替逐请求背压。

每个 request ID 的事件顺序是 response head → 零个或多个 body → response end，
或 error/timeout。不同 request ID 可以交错，宿主必须按 ID 保存独立状态。

取消使用 `capsid_worker_cancel()`，重复取消是幂等的。取消会传播到 handler、
上传 body、响应背压和出站 fetch。旧请求已在途的合法帧可能仍需排空，宿主
不应立即复用 request ID。

## 事件与内存生命周期

`capsid_event.payload` 以及从以下函数取得的 view 都指向运行时拥有的事件缓冲：

- `capsid_response_header_at()`；
- `capsid_response_status_text()`；
- `capsid_audit_record_decode()`。

这些 view 只保证有效到同一 worker 的下一次 `capsid_worker_next_event()` 调用。
跨回调、跨线程、异步写入或排队前必须复制；不得保存裸指针。响应 header 应通过
count/iterator 解码，以保留多个 `Set-Cookie` 的独立值和顺序。memory metrics
decode 复制数值，不保留 payload view。

宿主自己提供给 `begin_request`、bundle 和 policy 的输入除文档明确说明同步复制
外，也应按调用边界管理；最安全的规则是调用返回前保持有效。

宿主必须持续排空 `CAPSID_EVENT_LOG` 和 `CAPSID_EVENT_AUDIT`。它们是有界通道，
不能替代无界日志队列。worker 出现 `CAPSID_EVENT_EXIT`、协议错误或同步 CPU
timeout 后，应销毁并替换。

## SSE、streaming 与 no-buffer

Capsid 不把 SSE 当作特殊协议：它是长生命周期 response body stream。宿主应在
收到 response head 后立刻转发 headers，并逐 body frame 写下游；不要等待
response end 聚合。对 `text/event-stream`、无 `Content-Length` 或显式
streaming route：

- 禁止代理层自动 response buffering 和整体压缩缓冲；
- 下游 write 完成后再归还 worker credit；
- 客户端断开立即 cancel；
- 为连接总数、每连接 idle time 和未确认字节设置宿主上限；
- 若网关协议需要 flush，按事件边界或产品定义的节流策略 flush，不能依靠空帧。

Capsid 不生成 `X-Accel-Buffering` 等特定代理 header。是否设置、删除或解释这些
header 属于宿主/部署策略。

## 错误、取消与 shutdown

`CAPSID_EVENT_ERROR` 是请求级错误（request ID 非零）或 worker 级错误（ID 为零）；
`CAPSID_EVENT_REQUEST_TIMEOUT` 表示 worker soft deadline；`CAPSID_EVENT_EXIT` 表示子
进程终止。宿主必须把三者与应用 HTTP 5xx 区分，不能把协议/隔离故障伪装成应用
响应。

下游取消、宿主 deadline 或请求体读取失败时调用 `capsid_worker_cancel()`。cancel
后丢弃该 ID 的迟到事件，但继续驱动 socket，直到运行库完成内部清理。不要复用
已取消的 ID。

正常关闭先从调度摘除，等待 inflight 清零，再调用 `capsid_worker_shutdown()` 并
继续 flush/read。到宿主 shutdown deadline 仍未退出时调用
`capsid_worker_terminate()`，最后始终 `capsid_worker_destroy()`。destroy 是
abortive cleanup，不应在持有 payload view 的异步回调中发生。

## 超时与销毁

`request_timeout_ms` 产生 `CAPSID_EVENT_REQUEST_TIMEOUT`：

- 异步悬挂只取消对应请求，worker 可以继续使用；
- QuickJS interrupt 打断同步 CPU 死循环后，worker 视为 disposable。

`capsid_worker_destroy()` 是有界回收路径，会从正常 shutdown 升级到 SIGTERM
和 SIGKILL。宿主不应依赖应用 bundle 主动退出。

## 网络与安全配置

`egress_policy == NULL` 表示所有出站 Fetch 默认拒绝。若同时配置
`capsid_capability_policy.net_policy`，两者取交集。hostname、DNS 解析后的实际
地址和每次 redirect 都会重新检查。

`strict_sandbox`、`sandbox_required_features`、`resource_limits`、
`sandbox_cgroup_path` 和 `sandbox_network_namespace_fd` 都是宿主接口，
不会暴露给 JavaScript。详情见：

- [宿主能力策略](capability-policy.md)
- [Linux 严格沙箱](linux-sandbox.md)

## sandbox 与资源职责

JavaScript 不能扩大权限。宿主负责构造 capability、egress、cgroup、namespace
和文件路径策略，并在 READY 的 flags 中验证实际安装的 sandbox feature。
生产 Linux 应使用 strict sandbox 和受控 cgroup；无 strict 的 benchmark 数据
不得作为生产隔离证明。

`capsid_worker_request_memory_metrics()` 是诊断接口，会遍历 QuickJS heap。它不进入
常规 telemetry 采样，不进入请求热路径，也不向 JavaScript 暴露。进程 RSS/PSS
应读取 worker PID，而不是只测网关或把整个网关重复分摊给每个 worker。

## ABI 版本策略

当前 ABI 为 v7。兼容规则如下：

- 同一 ABI version 不改变已有结构体字段、大小、对齐、枚举数值和函数签名；
- 可以追加独立函数和新的事件枚举值，但旧宿主必须把未知事件当作明确错误，
  不能静默解释；
- 需要扩展已有结构体时升级 ABI version，除非该结构从一开始就定义了双方验证的
  size negotiation；
- 同一 ABI 版本内不删除或改名既有导出符号；
- library initializer 只能写入调用方 `struct_size` 允许的范围；
- library 必须通过冻结 v7 header 编译并链接的
  `abi_v7_current_header_current_library` 测试。

升级宿主时先部署能识别新事件的网关，再启用会产生该事件的新功能。诊断类新事件
只有宿主主动请求才会出现，因此不会影响未调用新函数的 v7 宿主。

## 上线清单

- 固定 worker、library、bundle、capability manifest 和 build metadata 的 hash；
- 验证 READY、sandbox flags、应用健康请求和最大 body/headers；
- 验证 request/response credit、慢客户端、SSE、取消和 gateway shutdown；
- 验证 worker crash、协议错误、soft/hard timeout 和替换策略；
- 将 worker PID 的 cgroup memory/CPU/PID 限制与网关资源分开观测；
- 保存旧 header/新 library、Release 全测和 delegated sandbox 的发布证据。

## 最小配置示例

```c
capsid_worker_config config;
capsid_worker_config_init(&config);
config.worker_path = "/path/to/capsid-worker";
config.request_timeout_ms = 1000;

capsid_worker *worker = NULL;
capsid_result result = capsid_worker_spawn(&config, &worker);
if (result != CAPSID_OK) {
    /* 记录 capsid_result_string(result)，本次启动失败 */
}
```

生产集成还必须处理 `WOULD_BLOCK`、所有事件类型、stream credit、取消、
worker 替换和宿主进程 shutdown。完整符号与字段注释以 public header 为准。
