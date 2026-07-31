# 第三方宿主集成规范

本规范面向直接链接 `libcapsid_runtime.a` 的网关、server、worker pool 和其他
embedder。Capsid Runtime 只提供隔离 worker 与 C ABI，不替宿主实现 HTTP
监听、调度、重试或应用发布。

## 线程与事件循环

一个 `capsid_worker` 同一时刻只能由一个宿主线程驱动。创建、写入请求、读取事件、
取消和销毁都必须串行；若网关有多个 I/O 线程，应把每个 worker 固定到一个
owner loop，通过线程安全队列把命令投递给 owner。

`capsid_worker_fd()` 返回非阻塞 Unix socket。宿主应：

1. 有待发送数据时监听 writable，任何时候监听 readable；
2. writable 时反复调用 `capsid_worker_flush()`，直到 `CAPSID_OK` 或
   `CAPSID_WOULD_BLOCK`；
3. readable 时反复调用 `capsid_worker_next_event()`，直到
   `CAPSID_WOULD_BLOCK`；
4. 收到 `CAPSID_CLOSED`、`CAPSID_PROTOCOL_ERROR` 或 `CAPSID_EVENT_EXIT` 后停止复用
   该 worker，并按应用发布策略替换它。

不要在共享 reactor 上阻塞等待单个 worker。启动、请求和 shutdown 都应有宿主
deadline；worker 内置 deadline 是第二道边界，不替代网关 deadline。

## worker 与应用生命周期

一个 worker 只加载一个自包含 ESM 应用。创建后必须先发送 bundle，再等待
`CAPSID_EVENT_READY`；READY 前不得发请求。源码使用
`capsid_worker_load_bundle_named()`。只有由完全相同且可信的 Capsid/QuickJS 构建
生成的字节码才能使用 `capsid_worker_load_trusted_bytecode_named()`；字节码不是
安全输入格式，也不兼容其他构建。

推荐的发布切换顺序是：创建新 worker → 加载并等待 READY → 加入调度 →
从调度摘除旧 worker → 等待旧请求结束 → deadline 到期则 cancel → shutdown →
destroy。应用 bundle、policy 和 sandbox 配置在 worker 生命周期内不可变；变更
时替换 worker，不做原地热补丁。

## 请求、credit 与背压

request ID 在一个 worker 内必须非零且唯一，直到 response end、error、timeout
或 cancel 完成。请求顺序为：

1. `capsid_worker_begin_request()`；
2. 只在 `CAPSID_EVENT_REQUEST_CREDIT` 授予的额度内调用
   `capsid_worker_write_request()`；
3. `capsid_worker_end_request()`。

响应 body 同样受 credit 控制。宿主消费或成功转发一段
`CAPSID_EVENT_RESPONSE_BODY` 后，才用
`capsid_worker_grant_response_credit()` 归还对应字节。不能提前按预计消费量授信，
否则下游慢客户端会把内存压力转移到宿主。`CAPSID_WOULD_BLOCK` 是正常背压信号，
不得当作 worker 故障或忙循环重试。

每个 request ID 的事件顺序是 response head → 零个或多个 body → response end，
或 error/timeout。不同 request ID 可以交错，宿主必须按 ID 保存独立状态。

## payload view 生命周期

`capsid_event.payload`、响应 header、status text 和 audit decode 所引用的 view，
只保证有效到同一个 worker 的下一次 `capsid_worker_next_event()`。跨回调、跨线程、
异步写入或排队前必须复制。不得保存裸指针，也不得在读下一事件后继续使用
header view。memory metrics decode 复制数值，不保留 payload view。

宿主自己提供给 `begin_request`、bundle 和 policy 的输入除文档明确说明同步复制
外，也应按调用边界管理；最安全的规则是调用返回前保持有效。

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
`capsid_worker_terminate()`，最后始终 `capsid_worker_destroy()`。destroy 不应在持有
payload view 的异步回调中发生。

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
