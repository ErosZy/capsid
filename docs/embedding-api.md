# 宿主嵌入接口

公开接口以 [`include/capsid/runtime.h`](../include/capsid/runtime.h) 为权威。
当前 `CAPSID_ABI_VERSION == 7`；所有配置结构必须先调用对应的 `*_init()`，
不要手工清零或猜测结构大小。

第三方 server/pool 的线程模型、event loop、view 生命周期、SSE/no-buffer、
取消、shutdown 和 ABI 升级约束见
[`host-integration.md`](host-integration.md)。

## 生命周期

典型宿主流程如下：

1. `capsid_worker_config_init()` 后填写 `worker_path`、资源和安全策略；
2. `capsid_worker_spawn()` 创建 worker；
3. `capsid_worker_load_bundle()` 或 `capsid_worker_load_bundle_named()` 加载源码 bundle；
4. 通过 `capsid_worker_fd()` 接入宿主 event loop；
5. 写入请求头/body/end，并按事件补充双向 credit；
6. 持续调用 `capsid_worker_next_event()` 消费响应、日志、审计和退出事件；
7. `capsid_worker_shutdown()` 后继续排空，最终调用 `capsid_worker_destroy()`。

`capsid_worker_spawn()` 会同步校验并深拷贝所有嵌套配置字符串、规则和策略。
调用返回后，宿主可以释放原配置内存；要变更策略必须新建 worker。

### 可信字节码

`capsid_worker_load_trusted_bytecode_named()` 可跳过源码解析，加载由完全相同的
Capsid/QuickJS 构建生成的模块字节码。它是面向宿主构建流水线的可选优化，不是
应用上传格式。

QuickJS 字节码不保证跨版本、编译选项或架构兼容，反序列化器也不构成安全边界。
损坏、不匹配或攻击者控制的 bytes 可能导致内存破坏；因此只能加载宿主生成并
校验过摘要的可信产物，不能加载租户输入。`source_name` 还必须与编译时嵌入的
模块名完全相同。常规应用继续使用源码 bundle API。

## 非阻塞与事件循环

大部分 I/O API 可能返回 `CAPSID_WOULD_BLOCK`。调用者应等待
`capsid_worker_fd()` 可读/可写，再调用 `capsid_worker_flush()` 或继续取事件。
返回 `WOULD_BLOCK` 的操作不会提交半个逻辑帧。

当前 handle 不承诺跨线程并发调用。最稳妥的集成方式是由一个 event-loop
线程拥有每个 `capsid_worker`，其他线程通过宿主自己的队列投递工作。

## 请求与流控

每个请求使用宿主分配的非零 `request_id`：

```text
begin_request
  → write_request（可多次）
  → end_request
  → RESPONSE_HEAD / RESPONSE_BODY / RESPONSE_END
```

请求 body 只能在收到 `CAPSID_EVENT_REQUEST_CREDIT` 后写入，不能超过授予的
credit。响应 body 由宿主通过 `capsid_worker_grant_response_credit()` 放行。
credit 按 request ID 隔离，全局 queue budget 不能代替逐请求背压。

取消使用 `capsid_worker_cancel()`，重复取消是幂等的。取消会传播到 handler、
上传 body、响应背压和出站 fetch。旧请求已在途的合法帧可能仍需排空，宿主
不应立即复用 request ID。

## 事件与内存生命周期

`capsid_event.payload` 以及从以下函数取得的 view 都指向运行时拥有的事件缓冲：

- `capsid_response_header_at()`；
- `capsid_response_status_text()`；
- `capsid_audit_record_decode()`。

这些 view 只保证有效到同一 worker 的下一次 `capsid_worker_next_event()` 调用。
需要异步保存时必须先复制。响应 header 应通过 count/iterator 解码，以保留
多个 `Set-Cookie` 的独立值和顺序。

宿主必须持续排空 `CAPSID_EVENT_LOG` 和 `CAPSID_EVENT_AUDIT`，不能把它们当成
无界日志通道。worker 出现 `CAPSID_EVENT_EXIT`、协议错误或同步 CPU timeout
后，应销毁并替换。

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

## 最小配置示例

```c
capsid_worker_config config;
capsid_worker_config_init(&config);
config.worker_path = "/path/to/capsid-worker";
config.request_timeout_ms = 1000;

capsid_worker *worker = NULL;
capsid_result result = capsid_worker_spawn(&config, &worker);
if (result != CAPSID_OK) {
    /* 处理启动失败 */
}
```

生产集成还必须处理 `WOULD_BLOCK`、所有事件类型、stream credit、取消、
worker 替换和宿主进程 shutdown。完整符号与字段注释以 public header 为准。
