# Capsid Host 技术评审与 v1 详细方案

> 状态：关键决策已确认；实施尚未开始。
> 基线：2026-08-01 的 `main` 工作树。
> 上游规划：[Capsid Host 架构规划](host-architecture-plan.md)。
> Runtime 权威接口：[runtime.h](../include/capsid/runtime.h)；集成约束见
> [第三方宿主集成规范](host-integration.md)。

## 1. 结论

现有 Host 规划的大方向可以通过，但建议在开始实现前修改若干关键契约。

保留的核心决定：

- Host 是独立的一方宿主进程，Runtime 继续只负责单 worker 和 FetchRPC；
- 一份 Host 上限与一份 App 申请做交集，App 不能扩大权限；
- 每个 worker 一生只属于一个不可变 App Version；
- 新版本预热成功后再切换，任何失败保持旧版本；
- worker 固定归属一个事件循环 owner，完整执行 credit、cancel 和 drain；
- v1 使用 HTTP/1.1，TLS/HTTP/2 先交给成熟反向代理；
- 不因为猜测性能而引入 io_uring、共享内存 IPC 或自定义 HTTP parser。

需要先修正的部分：

1. v1 同时支持源码和可信字节码；`bundle.qjsb` 只有通过签名 provenance、摘要、精确
   source name 和 Runtime compatibility ID 校验后，才能进入 trusted bytecode API；
2. `env.valueFrom` 读取的 secret value 作为不可变 `capsid:env` 快照进入 worker；这是
   v1 的明确安全契约，而不是实现泄漏；
3. `host.json` 缺少 listener、管理 socket、全局容量和队列硬边界；
4. 权限交集不是简单字符串集合交集，需要类型化、规范化的 Policy Compiler；
5. 部署 API 在第一阶段就承诺蓝绿语义，因此 staging、预热、原子切换和 drain 也
   必须在第一阶段形成垂直闭环；
6. `active.json` 需要明确原子落盘与恢复语义，但 v1 **不需要数据库**；
7. Runtime 尚缺字节码 compatibility ID 和结构化启动错误；这两项必须在可信字节码
   和第一方 Host 之前补齐，worker 回收也不能阻塞 reactor；
8. Host 必须补齐 HTTP request smuggling、hop-by-hop header、流式 body、慢客户端和
   自动重试的精确规则。

推荐的 v1 技术栈：

| 领域 | 选择 | 原因 |
| --- | --- | --- |
| Host 语言 | C++20，仅 Host target 使用 | 直接调用现有 C ABI；复用 CMake 与 C++ 工程经验 |
| 事件循环与 HTTP/1 | Boost.Asio + Boost.Beast | Linux 下使用 epoll；能接管 Unix fd；提供增量 HTTP/1 parser/serializer |
| 配置 JSON | Jansson | API 小；能显式 `JSON_REJECT_DUPLICATES`，适合安全配置 |
| 摘要与验签 | OpenSSL 3.5 LTS 的 `EVP` SHA-256 / Ed25519 | 不自写哈希或签名实现；后续如需 TLS 仍可复用 |
| 持久状态 | 普通文件 + `fsync` + 原子 `rename` | 单进程、单写者足够；不引入 SQLite |
| 指标 | 内建固定指标 + `/metrics` 文本端点 | v1 不引入完整 telemetry SDK；避免高基数与 exporter 故障 |
| TLS/H2 | 外部 nginx/Caddy/Envoy | 先收敛发布、调度和隔离，不同时维护边缘协议栈 |

没有选择的方案：

| 方案 | 本轮不选的原因 |
| --- | --- |
| raw epoll + 自写 HTTP 状态机 | 重复实现 parser、timer、半包和生命周期；安全收益为零 |
| Rust/Tokio/Hyper | 内存安全优势真实存在，但当前仓库没有 Rust 基础，会增加第二套构建、FFI 和 CI；若团队主要能力在 Rust，可重新评估 |
| 继续 Go/cgo | 保留为 A/B 基线；第一方 Host 规划的目标是直接拥有 worker fd 和 ABI，是否更快仍由数据证明 |
| SQLite/其他数据库 | v1 单进程单写者，只需要一个活动版本指针，没有数据库查询或事务需求 |
| Boost.JSON 作为安全配置 parser | 重复 key 采用 last-wins，不能直接满足 fail-closed 配置要求 |

## 2. 评审范围和当前项目事实

本评审阅读了顶层构建、公开 ABI、Runtime client、worker、sandbox、capability、egress、
协议、集成测试、框架兼容、性能和 Host 规划材料。项目当前结构可以概括为：

```text
HTTP 宿主（尚不存在于本仓库）
    │
    │ libcapsid_runtime.a / C ABI v7
    │ FetchRPC over Unix socketpair
    ▼
capsid-worker
    ├── QuickJS-ng + libuv
    ├── Web API bootstrap
    ├── capability / egress policy
    └── seccomp + Landlock + namespace + cgroup
```

当前 ABI 已能支持：

- spawn、源码 bundle 加载和 READY 事件；
- 多 request ID、请求/响应 credit、取消和超时；
- response head/body/end、日志、审计和 worker exit；
- JS heap、进程地址空间、fd 和 cgroup 限制；
- strict sandbox、额外 required feature 和预打开 network namespace fd；
- capability、environment snapshot 和双层 egress policy；
- worker PID、CPU 拓扑和可选 affinity。

当前 ABI 没有提供：

- HTTP listener、路由、队列、worker pool、发布和持久状态；
- Runtime/QuickJS 字节码 compatibility ID；
- spawn 失败的结构化阶段、字段和 `errno`；
- Host 配置到 Runtime descriptor 的编译器；
- cgroup 目录的创建与生命周期；network namespace 创建有意留给部署环境，不作为
  第一方 Host 或 Runtime 的待补能力；
- 非阻塞的 child reap/destroy API。

`capsid_worker_destroy()` 当前会执行 shutdown、等待、SIGTERM、SIGKILL 和 wait，最坏会
同步等待数百毫秒。因此它不能直接在数据面 reactor 回调里执行。过渡方案是先在
owner shard 摘除并关闭 worker，再把唯一 handle 移交给有界 reaper executor；长期可
增加拆分式非阻塞回收 ABI。

另有一个项目一致性问题：当前 Git tree 没有 `bench/`，但 README、性能文档和
`tests/audit-current-docs.mjs` 都引用其中的报告。因此这些性能数字只能作为文档中的
既有陈述，本次 checkout 无法从原始报告重新核验。第一方 Host 的性能验收不能依赖这
一缺失证据，必须重新生成可提交或可追溯的 A/B 结果。

## 3. 对现有 Host 规划的逐项评审

### 3.1 产品边界：通过

“Runtime 管单 worker，Host 管 HTTP、路由、池、发布和过载”的边界是正确的。
`capsid-host` 应是新 executable，Host 内部组件可以形成 `capsid_host_core`，但 v1
不承诺第二套公共稳定 ABI。

需要同步修正文档中的一句冻结决策：“Capsid 只提供 runtime”应改为“Runtime target
不内置 HTTP server；产品可另行交付第一方 Host”。否则 `project-status.md` 与 Host
规划在产品语义上互相冲突。

### 3.2 `host.json` / `capsid.json` 两层模型：有条件通过

两层而不是 realm/tenant/policy directory 多层模型，有利于 v1 收敛。但“交集”必须
定义为类型化偏序：

- module：精确名称集合包含；
- env：精确键或 Host 允许的尾部通配规则；
- fs：按规范化 path component 判断祖先关系，不能做字符串前缀；
- fetch：按 hostname/IP/CIDR、端口范围和 deny 优先级编译；
- storage/stdio：精确资源集合；
- 数值限制：App 值小于等于 Host 最大值；
- sandbox feature：只允许 Host 决定，App 没有覆盖字段。

Policy Compiler 必须输出规范化 `effective.json`、Runtime descriptor 和稳定 rule ID
反查表。部署失败应返回 JSON pointer，例如：

```json
{
  "code": "PERMISSION_EXCEEDS_HOST",
  "field": "/permissions/fs/read/allow/0",
  "requested": "/srv/capsid/data/orders",
  "allowedBy": "/permissions/fsReadRoots"
}
```

### 3.3 权限样例中的 Fetch scheme：需要修改

规划中的 Host 上限写成 `*.internal.example.com:443`，App 申请却写成
`https://orders-api.internal.example.com:443`。当前 Runtime egress 判定只接收
host/IP/CIDR 和 port，不能区分 `http` 与 `https`。

v1 应二选一：

- 简单方案：配置语法统一为 `host:port`，明确它不保证 URI scheme；
- 若必须保证 HTTPS-only：先扩展 worker/ABI，使 policy 判定包含 scheme。

推荐 v1 使用第一种，不在配置里表达当前 Runtime 无法执行的安全承诺。

### 3.4 Secret 语义：必须修正

v1 明确接受 `API_TOKEN` 通过 `capsid:env` 进入 worker：Host 把 token value 放入 HELLO
的 environment snapshot，应用只通过获准的 `capsid:env` key 读取。边界固定为：

- 不把 secret 文件路径传给 worker；
- 不把 secret 作为进程环境变量；
- 不写入 `effective.json`、日志、错误或指标；
- worker 只收到 App 明确申请、Host 明确允许的键值快照；
- 单值最长 16 KiB、最多 256 项、全部 name + value 最多 48 KiB，与现有 Runtime 校验
  保持一致；值不能包含 NUL；
- secret 变化不原地修改 READY worker，而是生成新 generation、预热并原子替换；
- Host 在 spawn 返回后尽快清除临时读取 buffer，但不宣称能够抹除 allocator、IPC 和
  worker 内已产生的全部副本。

这是有意选择的能力模型：获得该 key 权限的应用代码可以读到值。v1 不再同时承诺
“通过 `capsid:env` 使用 secret”和“secret 内容不进入 worker”这两个互斥目标。

### 3.5 可信字节码：纳入 v1，但必须先建立信任链

公开头文件明确说明，攻击者控制、损坏或不兼容的 QuickJS bytecode 可能造成内存
破坏。compatibility ID 只解决“格式是否匹配”，不能把不可信 bytes 变可信。

因此 v1 同时保留源码和字节码，但字节码必须满足完整的 provenance 契约：

- `bundle.mjs` 始终是必需的语义源和兼容回退；
- `bundle.qjsb`、`bytecode.json` 和 `bytecode.sig` 必须成组出现；
- `capsid-bytecode-compile` 必须来自目标 Host 同一发布物、链接同一 QuickJS 配置；工具
  从源码直接生成 bytecode 和待签名 attestation，不提供“为任意现有 qjsb 补签”模式；
- 构建流水线使用离线 Ed25519 私钥签名，Host 只配置 key ID 到公钥的信任根；
- attestation 固定包含 schema、App、Version、精确 `sourceName`、源码 SHA-256、字节码
  SHA-256、Runtime/QuickJS compatibility ID 和 key ID；
- 签名覆盖带 domain separator、固定字段顺序和长度前缀的二进制消息，不依赖 JSON
  object key 顺序或临时 canonicalization 规则；
- Host 安全复制三个文件后，先拒绝重复/未知字段，再验证 key、签名、两个摘要、App、
  Version 和 `sourceName`；任何 provenance 或摘要失败都使部署失败；
- 签名有效但 compatibility ID 与当前 worker 不匹配时，明确记录原因并回退到同
  Version 的 `bundle.mjs`；匹配时才调用
  `capsid_worker_load_trusted_bytecode_named()`；
- key 撤销影响后续部署和重启恢复；已 READY worker 不原地改变，按 generation
  替换规则处理。

`bytecode.json` 的 v1 形状固定为：

```json
{
  "schema": "capsid-bytecode-v1",
  "application": "orders",
  "version": "2026-07-31-002",
  "sourceName": "bundle.mjs",
  "sourceSha256": "sha256:...",
  "bytecodeSha256": "sha256:...",
  "compatibilityId": "sha256:...",
  "keyId": "release-2026"
}
```

`bytecode.sig` 是恰好 64 bytes 的原始 Ed25519 signature。被签消息是
`"capsid-bytecode-attestation-v1\0"`，随后按上述顺序串联每个字段的 32-bit big-endian
长度和 UTF-8 bytes。所有字符串都有独立上限；Host 重建消息后使用 OpenSSL EVP 验签。

选择结果只有四种：

| 版本目录状态 | 结果 |
| --- | --- |
| 三个 bytecode 文件都不存在 | 加载源码 |
| 三者齐全、provenance 有效、identity 匹配 | 加载可信字节码 |
| 三者齐全、provenance 有效、仅 identity 失配 | 记录原因并加载源码 |
| 文件不齐或任一 provenance 校验失败 | 部署失败，旧版本保持 active |

当前仓库已有 trusted bytecode Runtime API 和 worker 加载路径，但编译器只在 benchmark
构建中声明，且本 checkout 缺少其 `bench/` 源码。v1 的第一个字节码切片应把该工具
恢复并提升为正式、可测试、随发布物交付的 target；在兼容身份和验签闭环完成前，Host
不能调用 trusted API。

### 3.6 部署阶段：当前拆分不闭环

规划第 4 节已承诺“预热、原子切换、失败保留旧版本”，但实施顺序把可靠蓝绿放到
第二阶段。这样第一阶段的 `/v1/deploy` 无法满足它自己的公开语义。

建议第一阶段做最小垂直闭环：

```text
安全读取 → 校验/编译 → 内部快照 → spawn/load/READY
       → 可选健康检查 → 原子 active 切换 → 旧池 drain
```

可以推迟 autoscaling、完整 reload、TLS/H2 和多种 transport，但不能推迟原子切换和
失败保持旧版本。

### 3.7 listener 配置：需要补回

当前 `host.json` 示例没有 listener，但后文同时支持 subdomain、path 和 trusted
header，无法决定绑定地址、路由模式和信任边界。每个 listener 必须只配置一种主要
路由模式，避免隐含优先级。

Header routing 仅允许 Unix socket，或具备 mTLS/source allowlist 的独立内部 TCP
listener。公网 listener 必须删除客户端提供的同名控制 header。

### 3.8 资源上限：不能只看单 worker

`memoryMax`、`cpuQuota` 是每 worker 限制；`maxWorkers` 会放大为 App 和整机资源。
Host 还需要：

- 全局 worker 数和启动并发；
- 全局可承诺内存；
- 单 App READY/starting/draining worker 总数；
- 蓝绿期间双份 pool 的临时容量；
- App/全局 queued request 与 queued header bytes；
- 每 listener connection、header、body 和 timeout 限制。

启动 worker 前必须先获得 memory/startup permit；失败就拒绝预热，不能先过量 spawn
再等待 cgroup OOM。

### 3.9 默认值：冻结前必须用证据校准

规划示例中的 `maxInflight=32`、`maxWorkers=16` 等可以作为讨论值，不能直接冻结为
`host-v1` 默认。现有性能文档也强调不同 workload 的最优 worker/inflight 不同。

v1 alpha 阶段应先要求显式 pool size，完成固定 workload 的容量扫描后再冻结默认值。
Runtime 自身 ABI 默认值是底层兜底，不应自动成为 Host 产品默认值。

### 3.10 无配置启动：需要定义得更保守

“没有 `host.json` 也能运行”不能等同于“自动开放一个公网 listener”。推荐默认行为：

- strict sandbox 和 deny-all capability/egress；
- 不绑定任何 TCP data listener；
- 只创建 mode `0600` 的本机 Unix admin socket；
- state/app root 不存在或 ownership 不安全时明确失败；
- 目标 Linux 缺少 required isolation feature 时失败，不降级；
- 运维必须显式配置 listener 后才对外服务。

这仍允许自包含 App 做零权限部署，同时不会因为缺配置意外暴露网络入口。

### 3.11 `capsid:storage` 不是共享持久存储

当前 `capsid:storage` 只存在于单 worker 内存。App 开启 storage 且 pool 大于一个 worker
时，Host 校验必须给出明确警告；扩缩容、worker crash 和版本切换都会丢失或分叉状态。
v1 不提供 affinity 来掩盖这一事实，也不能把 namespace 命名解释为跨 worker 数据库。

### 3.12 Host 配置 reload 后置

安全 ceiling 收紧时，旧 READY worker 仍持有旧快照；“逐 App 平滑 reload”会产生新旧
安全策略并存窗口。v1 不做 in-process security reload，只提供 config validate/plan，
变更通过受控 Host 重启或外部双实例切换完成。以后若实现 reload，必须明确收紧时是
立即 cancel 旧请求，还是允许一个有界 drain 窗口，不能隐式选择。

## 4. 目标架构

```text
                         Unix 管理 socket
                                │
                         Admin HTTP/1 API
                                │
                  ┌──────── Control Plane ────────┐
                  │ config / policy / deploy      │
                  │ artifact snapshot / registry  │
                  └──────────────┬────────────────┘
                                 │ immutable snapshot
        ┌────────────────────────┼────────────────────────┐
        ▼                        ▼                        ▼
  Reactor shard 0          Reactor shard 1          Reactor shard N
  listener/client fd       listener/client fd       listener/client fd
  local worker pool        local worker pool        local worker pool
  request state            request state            request state
        │                        │                        │
        └──── capsid_worker_fd / FetchRPC / credit ─────┘
                                 │
                bounded bootstrap + reaper executors
                                 │
                         isolation boundary
              delegated cgroup / Host network environment
```

### 4.1 进程和线程

`capsid-host` 使用：

- 1 个 control thread：配置、部署状态机、active pointer 和 registry 发布；
- `N` 个 reactor thread：每个线程一个 `boost::asio::io_context`；
- 小型有界 bootstrap executor：执行安全文件复制、spawn、load 和等待 READY；
- 小型有界 reaper executor：执行可能等待 child 的 destroy/wait；
- 1 个有界日志输出线程；队列满时按事件类别计数并执行明确丢弃策略。

`N` 初始取 `capsid_recommended_worker_count()` 与 Host 配置上限的较小值，但最终默认
必须经第一方 Host A/B 校准。

### 4.2 ownership 规则

每个 worker 的唯一 ownership 变化如下：

```text
bootstrap executor
    → READY 后一次性 post 到目标 shard
    → shard 独占所有 Runtime API 调用
    → 摘除、停止读写并清空 view
    → reaper executor 独占 destroy
```

任何时刻只有一个线程调用同一个 `capsid_worker`。payload、header 和 audit view 在
下一次 `capsid_worker_next_event()` 前复制到 Host 自有的有界对象。

### 4.3 为什么选 Asio/Beast，而不是 raw epoll

Asio 在 Linux 使用 epoll，并能用 `posix::stream_descriptor` 管理现有
`capsid_worker_fd()`；Beast 提供增量 HTTP/1 解析和序列化。Host 仍然保持 owner
shard 模型，但无需自己重写 fd 注册、timer、HTTP framing 和半包状态机。

Beast 不是完整 Web server：Host 仍需实现路由、header 策略、body credit、超时和
错误映射。这恰好是 Capsid 产品逻辑，而不是重复实现通用协议 parser。

Runtime target 继续保持 C++11；只给 `capsid-host` 和 `capsid_host_core` 设置
`CXX_STANDARD 20`，不借 Host 项目升级破坏现有 ABI 或兼容测试。

## 5. 配置方案

### 5.1 修订后的 `host.json` 轮廓

以下字段是结构建议，不代表默认数值已经冻结：

```json
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/apps",
  "stateRoot": "/var/lib/capsid",
  "admin": {
    "unix": "/run/capsid/admin.sock",
    "mode": "0600"
  },
  "listeners": [
    {
      "name": "public",
      "tcp": "127.0.0.1:8080",
      "routing": {
        "mode": "subdomain",
        "suffix": ".apps.example.com"
      },
      "limits": {
        "connections": 4096,
        "headerBytes": "64KiB",
        "headerTimeout": "5s",
        "bodyIdleTimeout": "30s"
      }
    }
  ],
  "permissions": {
    "modules": ["capsid:permissions", "capsid:stdio"],
    "environmentNames": [],
    "fsReadRoots": [],
    "fetchTargets": [],
    "storageNamespaces": [],
    "stdioStreams": ["stdout", "stderr"]
  },
  "isolation": {
    "mode": "strict",
    "required": [
      "no_new_privs",
      "landlock",
      "seccomp",
      "user_namespace",
      "mount_namespace"
    ],
    "cgroupRoot": "/sys/fs/cgroup/capsid-host"
  },
  "trustedBytecodeKeys": {
    "release-2026": "/etc/capsid/bytecode-keys/release-2026.pub"
  },
  "defaults": {
    "worker": {},
    "request": {},
    "pool": {}
  },
  "maximums": {
    "worker": {},
    "request": {},
    "pool": {}
  },
  "capacity": {
    "workersTotal": 128,
    "startupsConcurrent": 4,
    "queuedRequestsTotal": 4096,
    "queuedHeaderBytesTotal": "64MiB",
    "workerMemoryCommitTotal": "24GiB"
  }
}
```

相比现有样例，主要变化是：

- 显式定义 admin 与 listener；
- 把 per-worker、per-request、pool 和 host capacity 分开；
- listener 自己拥有连接、header 和 timeout 上限；
- Host 级全局预算不能由 App 覆盖。

### 5.2 App 配置

App 侧也建议分清边界：

```json
{
  "apiVersion": "capsid/app-v1",
  "entry": "bundle.mjs",
  "permissions": {},
  "worker": {
    "jsHeap": "64MiB",
    "processAddressSpace": "256MiB",
    "memoryMax": "256MiB",
    "fileDescriptors": 64,
    "pidsMax": 8
  },
  "request": {
    "timeout": "3s",
    "maxInflightPerWorker": 8
  },
  "pool": {
    "minReady": 4,
    "maxWorkers": 4,
    "queueRequests": 128,
    "queueHeaderBytes": "2MiB",
    "queueTimeout": "250ms"
  },
  "healthCheck": {
    "path": "/_capsid/health",
    "timeout": "1s"
  }
}
```

静态池阶段要求 `minReady == maxWorkers`。等有界扩缩容实现并通过压力测试后，再允许
两者不同，避免 v1 配置暴露尚不存在的语义。

### 5.3 解析和校验

配置处理固定顺序：

1. 限制文件大小和 JSON nesting；
2. 使用标准 JSON 模式，拒绝 comment、trailing comma、NaN/Infinity；
3. 拒绝任何重复 key；
4. 拒绝未知字段；
5. 检查类型、单位、范围和跨字段关系；
6. 展开 `{application}` 模板；
7. 规范化路径、host、CIDR、port 和资源单位；
8. 编译有效策略并生成稳定摘要；
9. 只把规范化结果交给后续阶段。

选择 Jansson 的唯一关键理由，是它能用公开 flag 直接拒绝重复 key。Boost.JSON 的 DOM
parser 对重复 key 保留最后一个值，不适合作为安全配置的唯一 parser。配置不在热路径，
解析性能不是选型指标。

## 6. 版本快照与持久状态

### 6.1 为什么 `active.json` 足够

`active.json` 不是用户配置，它只是 Host 内部的活动版本指针。例如：

```json
{
  "schema": "capsid-active-v1",
  "app": "orders",
  "version": "2026-07-31-002",
  "generation": "sha256:8f3a9c..."
}
```

v1 的约束是：

- 一个 Host 进程；
- 一个 control plane writer；
- 每个 App 同时最多一个 deploy 操作；
- request 热路径只读内存 Registry，不读取状态文件；
- 没有跨 App 原子事务或复杂历史查询。

在这些条件下，数据库不会增强线上请求正确性，只会增加依赖、schema migration、备份
和损坏恢复面。普通文件已经足够。

### 6.2 状态目录

建议使用：

```text
/var/lib/capsid/
├── apps/
│   └── orders/
│       ├── active.json
│       ├── versions/
│       │   ├── 2026-07-31-001.json
│       │   └── 2026-07-31-002.json
│       └── generations/
│           ├── 8f3a9c.../
│           │   ├── COMPLETE
│           │   ├── capsid.json
│           │   ├── effective.json
│           │   ├── bundle.mjs
│           │   ├── bundle.qjsb
│           │   ├── bytecode.json
│           │   └── bytecode.sig
│           └── 729abe.../
│               └── ...
└── staging/
    └── <operation-id>/
```

`versions/<version>.json` 记录外部 Version ID 到 generation digest 的不可变映射。同一
App/Version 再次部署：

- 内容和有效配置摘要相同：返回当前或既有结果，属于幂等；
- 摘要不同：返回 `VERSION_IMMUTABILITY_CONFLICT`，不能覆盖旧映射。

### 6.3 generation identity

不能只用 bundle bytes 作为 generation identity。相同源码在权限、资源、Host 配置或
secret revision 改变后必须生成不同 worker pool。

```text
generationDigest = SHA-256(
    "capsid-generation-v1\0" +
    appId +
    sourceDigest +
    bytecodeAttestationDigestOrEmpty +
    selectedArtifactKind +
    normalizedAppConfigDigest +
    effectivePolicyDigest +
    effectiveResourceDigest +
    hostConfigDigest +
    secretRevision +
    runtimeCompatibilityId
)
```

即使本次因 compatibility mismatch 回退源码，也要记录已验证 attestation、回退原因和
实际 selected artifact。重启不得在不改变 generation identity 的情况下从源码静默切到
另一份字节码，或反向切换。

secret revision 不保存明文或 secret value 的裸摘要。secret provider 应返回不含秘密的
opaque revision；最简单的文件 backend 可使用已打开文件的 device、inode、size 和
`ctime` 组成 revision，并在读取前后复核。即使相同内容重写后生成新 generation 也可
接受，比再引入一套 Host 密钥管理更简单。

### 6.4 安全复制

从 `applicationsRoot` 读取版本时：

1. 先验证 App/Version ID 的字符集和长度；
2. 从预打开的 root dirfd 使用 `openat2`，带
   `RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS`；
3. 只接受 regular file，拒绝 symlink、device、FIFO、socket 和越界路径；
4. 对配置、源码和总版本大小设置硬上限；
5. 从已经打开的 fd 读取并计算 SHA-256；
6. 比较复制前后的 `fstat` 身份、size、mtime/ctime，变化即失败；
7. 写入 `stateRoot/staging/<operation-id>`，逐文件 `fdatasync`；
8. 写入最后的 `COMPLETE` marker 并同步目录；
9. 原子 rename 到 generation 目录并 `fsync` 父目录。

请求处理阶段只访问内存中的 bundle/metadata 或 Host 内部 generation，绝不重新读取上传
目录。

### 6.5 `active.json` 原子切换

切换必须在同一文件系统、同一目录完成：

1. 获取 per-App deploy mutex；
2. 确认 generation 已有 `COMPLETE` 且新 pool 全部 READY；
3. 以 `O_CREAT|O_EXCL` 创建 `active.json.tmp.<operation-id>`；
4. 写入规范化、完整的 JSON；
5. `fdatasync` 临时文件；
6. `renameat` 覆盖 `active.json`；
7. `fsync` App 状态目录；
8. 发布新的内存 Registry snapshot；
9. 返回部署成功，并开始旧 pool drain。

顺序必须是“持久指针成功后，再发布内存指针”。若进程在两者之间崩溃，重启会从新的
`active.json` 恢复；若在 rename 前崩溃，旧版本仍 active。临时文件在启动恢复时清理。

启动恢复只信任完整且引用存在 generation 的 `active.json`。无效指针应让该 App
fail closed 并报告明确错误，不能扫描目录后擅自选择“最新版本”。

### 6.6 Secret snapshot 读取

`value` 与 `valueFrom` 互斥。前者是普通配置值；后者只接受一个受限 secret key ID，
不能含 `/`、`..` 或空 component。文件 backend 的固定流程是：

1. Host 启动时安全打开 `secretRootTemplate` 的静态根，验证目录类型、owner 和 mode；
2. 使用已经校验的 App ID 打开 App 子目录，再用 `openat2` 的 `RESOLVE_BENEATH`、
   `RESOLVE_NO_SYMLINKS` 和 `RESOLVE_NO_MAGICLINKS` 打开 key；
3. 使用 `O_RDONLY|O_CLOEXEC|O_NONBLOCK`，随后只接受 regular file，拒绝 group/world
   writable、symlink、FIFO、device 和 socket；
4. 读取最多 16 KiB + 1 byte，超限即失败；拒绝 NUL 和非法 UTF-8，不自动 trim 换行；
   二进制 secret 必须由发布方先编码成文本；
5. 读取前后比较 fd 的 device、inode、size、mtime/ctime；变化即重试一次，仍变化则部署
   失败；
6. 只有同时通过 Host environment allowlist、App env 申请和 `capsid:env` module gate 的
   key 才生成 `capsid_env_entry`；总项数和总 bytes 再按 Runtime 上限复核；
7. Runtime 完成 descriptor 深复制后，Host 立即清除该 bootstrap task 的临时 value
   buffer；持久 metadata 只记录 opaque revision，不记录 value 或其裸摘要。

文件 mode 不等于完整 secret 管理系统；挂载、备份、节点 swap/core dump 和 secret root
的生命周期仍由部署环境负责。Host 的责任是最小读取、最小传递和不把明文扩散到控制面。

## 7. 部署 API 和状态机

### 7.1 管理面

默认管理入口是独立 Unix socket：

```text
/run/capsid/admin.sock
```

要求：

- mode 默认 `0600`，可选固定管理 group；
- 使用 `SO_PEERCRED` 校验本机 UID/GID；
- 不与公网 data listener 复用；
- request body、header 和处理时间有小而固定的上限；
- 不把 secret、bundle bytes 或完整环境写入响应或日志。

唯一能改变 active version 的操作仍是：

```http
POST /v1/deploy
Content-Type: application/json

{"app":"orders","version":"2026-07-31-002"}
```

部署通常超过普通 HTTP handler 延迟，建议返回 `202 Accepted` 和 operation ID：

```json
{
  "operation": "01J...",
  "app": "orders",
  "version": "2026-07-31-002",
  "status": "warming"
}
```

只读的 `GET /v1/operations/{id}`、`GET /v1/apps/{app}` 和健康/指标端点不违反“唯一改变
版本的接口”原则。

### 7.2 状态机

```text
RECEIVED
  → VALIDATING
  → STAGING
  → COMPILING_POLICY
  → WARMING
  → HEALTH_CHECKING
  → ACTIVATING
  → ACTIVE

任意切换前状态 → FAILED（旧版本不变）
旧 active pool → DRAINING → RETIRED
```

operation 记录可以是内存对象和有界 JSONL 运维日志，不需要为了查询它引入数据库。
Host 重启后只需准确恢复 active generation；中断的 deploy 可报告 `ABORTED_BY_RESTART`
并由调用方幂等重试。

### 7.3 并发和幂等

- 同一 App deploy 串行；
- 不同 App 可以并发，但共享全局 startup/memory permit；
- 相同 App/Version/generation 已 active：直接返回 active；
- 相同 Version 已映射到不同 generation：`409`；
- warming 中的完全相同请求加入同一个 singleflight；
- 新请求不会为每个调用分别 spawn 一套 pool。

### 7.4 健康检查

若配置 health check：

- 对每个新 worker 至少执行一次，而不是只检查 pool 中任意一个；
- 只允许 `GET`、无 body、固定小 response body 上限；
- 期望 `200..299`，完整消费或丢弃 body 并正确归还 credit；
- 使用独立 timeout 和 request ID；
- 任意 `minReady` worker 失败都不切换；
- health check 执行真实应用代码，文档要提醒不要产生业务副作用。

### 7.5 drain

激活新 generation 后：

1. Registry 不再把新请求发给旧 pool；
2. 旧 pool 保持处理 inflight；
3. inflight 清零则 `shutdown`、继续 flush/read 到 EXIT；
4. drain deadline 到期，cancel 所有 request；
5. 短暂 cancel grace 后 terminate；
6. handle 移交 reaper executor 执行 destroy；
7. 记录总 drain 时间和被强制取消数。

## 8. 路由和 HTTP 边界

### 8.1 一个 listener 一种路由模式

支持：

- `subdomain`：精确 DNS label 后缀；
- `path`：固定 `/@capsid/{app}/` 前缀；
- `header`：只用于受信内部 listener。

规则：

- App ID 只使用规划中的 ASCII 规范；
- Host header 去掉合法端口后按 label 边界匹配，不能做裸字符串 suffix；
- path 模式在原始 path segment 上识别 App，拒绝 encoded slash、backslash、dot segment
  和非法 percent encoding；
- request 参数只能查询 Registry，永远不拼磁盘路径；
- 不能通过 URL/header 选择 Version、generation 或 worker；
- 控制 header 在构造 FetchRPC headers 前删除。

### 8.2 HTTP/1 安全规则

Host 需要在进入应用前统一处理：

- 拒绝冲突或重复的 `Content-Length`；
- 拒绝 `Transfer-Encoding` 与 `Content-Length` 冲突；
- 只接受 Beast 明确认可的 HTTP/1 framing；
- 删除 connection-nominated header 及所有 hop-by-hop header；
- 禁止把 `Connection`、`Keep-Alive`、`Proxy-Connection`、`TE`、`Trailer`、
  `Transfer-Encoding`、`Upgrade` 原样交给 worker；
- 校验 header name/value、总字节和字段数；
- v1 禁止 WebSocket upgrade 和 CONNECT；
- v1 每连接只允许一个应用 request 正在处理，暂不实现 HTTP pipelining 并发；
- header、body idle、queue、Host request 和 response idle timeout 分开计时。

Host 不能假设 Runtime 的 response header decoder 已执行 HTTP 语义过滤；当前 decoder
主要验证 FetchRPC 二进制结构。因此 response 也必须经过 hop-by-hop、长度和非法值检查。

### 8.3 `Expect: 100-continue`

只有完成路由、admission、worker 分配并成功 begin request 后，才向客户端发送
`100 Continue`。被拒绝的请求不读取完整 body。

没有 `Expect` 的客户端可能提前发送 body；Beast 在读 header 时读过界进入 buffer 的
body bytes 必须计入 queued bytes，且 buffer 本身有硬上限。

## 9. Request/response credit 映射

### 9.1 请求方向

```text
client readable
  → parse header
  → route + admission + choose local worker
  → capsid_worker_begin_request
  → 等 REQUEST_CREDIT
  → 每次最多读取 remaining credit 的 body
  → capsid_worker_write_request
  → body 完成后 capsid_worker_end_request
```

没有 request credit 时不继续 application-level socket read。若 Runtime global write queue
使 `write_request` 返回 `WOULD_BLOCK`，保留当前有界 chunk，监听 worker fd writable，
flush 后再继续。

### 9.2 响应方向

```text
CAPSID_EVENT_RESPONSE_BODY
  → 在 next_event 前复制 payload
  → async_write 到 client
  → write completion 成功
  → grant_response_credit(实际写出字节)
```

不能在 socket write 提交时提前归还 credit。客户端慢、断开或 write timeout 时立即
cancel request；迟到事件按现有 ABI 要求继续排空但不转发。

每个 request 的 Host 缓冲上限应不大于授予的 Runtime response window，并同时计入
App 与 Host 的未确认字节预算。

### 9.3 SSE 和 streaming

- 收到 response head 后立即发出；
- body frame 逐段写，不聚合到 response end；
- `text/event-stream` 禁止整体压缩和代理 buffering；
- 成功写下游后才归还 credit；
- 设置连接数、idle timeout、最大持续时间或产品定义的例外；
- 客户端断开立即 cancel。

### 9.4 request ID

每个 shard 使用单调递增的 64-bit 非零 ID，按 worker 跟踪未完成集合。发生 wrap 时只有
不在该 worker 活跃集合中的 ID 才可使用；实现上可在接近上限时轮换 worker，避免复杂
复用逻辑。

## 10. Worker pool 和调度

### 10.1 shard-local pool

连接和 worker 都固定在 shard。新 pool 创建时按 shard 分配 worker，调度优先只选本
shard worker，从而避免 request body 和 response body 跨线程搬运。

当某 shard 暂时无 worker capacity：

- 请求进入该 App 在本 shard 的有界队列；
- 不把同一 worker 临时转交另一 shard；
- 后续若需要跨 shard request handoff，必须以 profiling 为依据并单独设计。

listener 可使用 `SO_REUSEPORT` 让各 shard 自己 accept；不支持时由 acceptor 做一次性
connection handoff。

### 10.2 选择 worker

静态 pool v1 使用简单的 Power of Two Choices：随机选两个本 shard READY worker，
比较：

```text
inflight
+ response bytes awaiting client
+ unhealthy penalty
```

pool 很小时直接取最小值也可以。不要只按 round-robin，因为一个 SSE 或慢客户端会让
worker 长期负载不对称。

### 10.3 admission control

固定顺序：

1. listener connection/header gate；
2. Host 全局 inflight/queue gate；
3. App inflight/queue gate；
4. 本 shard pool capacity；
5. worker `max_inflight_requests` 硬边界。

错误映射：

| 场景 | HTTP |
| --- | --- |
| App 不存在 | 404 |
| App 自己的 queue/quota 满 | 429 |
| Host 全局过载、pool 无 READY worker | 503 |
| queue 或 Host deadline 到期 | 504 |
| 应用正常返回 5xx | 原样应用响应 |
| worker/IPC 在 response head 前失败 | 503 或受限重试 |

### 10.4 自动重试

v1 只允许一次重试，并同时满足：

- 尚未向客户端发送 response head；
- 故障来自 worker crash/IPC/protocol，不是应用 HTTP 5xx；
- 方法为 GET 或 HEAD；
- 请求没有 body；
- Host deadline 仍有足够预算；
- 新 worker 属于同一 active generation。

PUT/DELETE 虽有协议幂等含义，但已流式发送的 body 未被 Host 保存，不能自动重放。
POST 即使带 Idempotency-Key，v1 也不自动重试，除非以后增加明确的 App opt-in 契约。

## 11. 权限编译与隔离

### 11.1 编译产物

Policy Compiler 对每个 generation 生成：

- `allowed_modules`；
- `capsid_permission_rule[]`；
- `capsid_env_entry[]`；
- direct `capsid_egress_policy`；
- capability `net_policy`；
- `capsid_resource_limits`；
- sandbox required feature bits；
- Landlock 所需只读 path rules；
- 稳定 rule ID 到 JSON pointer 的映射；
- 不含 secret 明文的 `effective.json`。

rule ID 推荐对规范化 rule 按 `(stage, capability, resource, action)` 排序后从 1 连续编号，
比截断 hash 更容易保证无碰撞和可复现。

### 11.2 文件路径

- Host root 和 App path 都先按绝对 path component 规范化；
- `/a/b` 是 `/a/b/c` 的祖先，但不是 `/a/bad` 的前缀；
- `.`、`..`、空 component、NUL 和非绝对路径被拒绝；
- deny 优先，App allow 必须完全落在 Host allow root 内；
- Runtime operation rule、Landlock 和实际 `openat2` 三层从同一个有效规则生成；
- 所需 root 不存在或不能安全打开时，部署失败，不静默忽略。

### 11.3 网络

Runtime 检查 hostname、DNS 结果和 redirect。Host 不配置或切换 network namespace，
worker 自然使用与 Host 相同的网络环境。v1 的职责是：

- Runtime policy 执行精确 hostname/IP/CIDR + port；
- Runtime 默认拒绝 loopback、link-local、metadata 和 RFC private ranges，除非 Host
  policy 用精确 CIDR 显式开放；
- Host/App schema 不暴露 network namespace、veth、route 或 firewall 配置；
- 如果运维把整个 Host 放进 systemd、容器或 Kubernetes 提供的额外网络边界，worker
  自然随 Host 使用该边界；这是可选部署措施，不是 Capsid 前置要求。

现有 Runtime 的预打开 netns fd 能力继续保留给其他 embedding host，但第一方 Host
不使用它，也不提供对应控制面字段。

### 11.4 cgroup 层级和容量

建议层级：

```text
capsid-host/
└── apps/<app>/<generation>/
    └── workers/<worker-id>/
```

- App/generation parent 控制聚合 CPU、memory 和 PID；
- worker leaf 使用当前 Runtime `sandbox_cgroup_path` 和 resource limits；
- Host 创建和删除目录，Runtime 只负责写 leaf limit、回读并 attach child；
- parent controller 和 `cgroup.subtree_control` 由部署环境预先委派；
- worker spawn 前先取得 Host memory/startup permit；
- 蓝绿预热要同时计入旧、新 pool，容量不足则部署失败，旧版本不受影响。

### 11.5 外部隔离边界

Host 主进程使用专用非 root 用户，通过 systemd `Delegate=yes` 或等价容器配置获得受限
cgroup subtree。Host 只在已委派且验证过的 root 下创建 App/generation/worker 子目录。

第一方 Host **不实现、也不规划** privileged supervisor：仓库中没有 root helper target、
supervisor socket、netns 创建协议或 nftables 管理逻辑。目标环境若要求独立 netns、veth
或防火墙，必须在启动 Host 前由 systemd、容器 runtime、Kubernetes CNI 或运维系统
完成；无法提供该边界时就更换部署形态，而不是让 Host 临时提权。

## 12. 可观测性

### 12.1 指标

v1 内建固定、低基数指标，使用 `app`、`generation`、`listener`、`result` 等受控 label；
禁止 request ID、URL、Version 自由文本、hostname 或错误消息成为 label。

至少包括：

- worker：starting/ready/busy/draining/crash；
- request：inflight/queued/rejected/cancel/timeout/retry；
- latency：queue、startup、worker、time-to-head、total；
- stream：request/response credit、未确认 bytes、slow-client cancel；
- deploy：validate/stage/spawn/load/health/activate/drain 时间和结果；
- isolation：required/applied feature、delegated cgroup failure、外部网络边界校验结果；
- log/audit queue drop；
- process 与 child RSS/PSS/cgroup memory/CPU。

Prometheus 文本端点默认只绑定管理 Unix socket或 loopback。OpenTelemetry C++ 的 signals
虽然已稳定，但 v1 没必要为了一个本地 Host 引入完整 SDK/exporter；需要 OTLP 时可让
sidecar scrape，或以后增加可选 adapter。

### 12.2 结构化日志

所有日志使用一行一个 JSON object，固定字段：

```text
timestamp, level, event, app, version, generation,
worker_id, request_id, operation_id, stage, result, duration_ms
```

禁止记录：

- secret value；
- Authorization/Cookie 等敏感 header；
- 原始 request/response body；
- 未清洗的应用错误作为结构字段；
- 高基数路径进入指标。

Runtime 的 LOG 和 AUDIT 必须持续排空。日志 sink 变慢不能阻塞 reactor：使用有界队列，
应用日志可丢弃并计数；部署、安全和进程生命周期事件进入独立高优先级 lane。若未来
要求完整合规审计，需要单独设计本地持久 spool，不能假装普通 stderr 提供 exactly-once。

## 13. Runtime 前置改动

### 13.1 P0：结构化 build/compatibility identity

可信字节码进入 v1，因此必须先新增只读 build info，至少包含：

```text
Capsid runtime version
ABI version
FetchRPC version
QuickJS commit
txiki overlay key/manifest
compile flags relevant to bytecode
architecture/endianness/pointer width
bytecode format identity
capability manifest hash
```

先提供 library 侧 `capsid_runtime_build_info()`，同时让实际 worker HELLO/READY 返回
同一 identity。Host 必须比较 library、compiler attestation 和 worker 三者，不能只信
链接到 Host 的 library。

### 13.2 P1：结构化错误

当前 spawn 只能返回 `INVALID_ARGUMENT`、`SYSTEM_ERROR` 等粗粒度结果。第一方 Host 需要
区分：

- config validation；
- socketpair/posix_spawn；
- cgroup 写入、回读和 attach；
- child exec；
- HELLO/sandbox；
- bundle parse/evaluate；
- required feature mismatch。

建议新增 size-negotiated `capsid_error_info` 和 `capsid_worker_spawn_ex()`，包含稳定 code、
stage、可选 `errno` 和安全消息。不要依赖 thread-local “last error”，它在多 shard/多
bootstrap thread 下难以正确使用。

### 13.3 P1：非阻塞生命周期

短期用 ownership handoff 到 reaper executor。长期考虑把：

```text
request_shutdown → poll EXIT → send_signal → reap → free handle
```

拆成不会等待的 API，使 Host 能完全在事件循环中表达生命周期。ABI 设计前应先用第一方
Host 实现验证确有必要，避免先扩 ABI 后发现 executor 已足够。

## 14. 构建与依赖治理

新增 CMake option：

```text
CAPSID_BUILD_HOST=ON|OFF
```

建议 targets：

```text
capsid_host_core      C++20 internal library
capsid-host           executable
capsid-host-tests     unit/integration targets
```

依赖原则：

- Runtime 和 public header 不依赖 Boost/Jansson/OpenSSL；
- Host dependencies 只链接 Host targets；
- 固定经过审查的 source release 和 SHA-256，构建时不隐式抓取浮动 branch；
- 生产镜像锁 OS、compiler、Boost、Jansson 和 OpenSSL patch version；
- 生成 SPDX SBOM，保存 license 与 source provenance；
- 开启现有 `-Wall -Wextra -Wpedantic -Werror`、LTO、ASan、UBSan；
- Host 并发核心增加 TSan job；
- TLS 即使由外部代理终止，OpenSSL 也只用于 SHA-256 与 Ed25519 验签，保持小的 EVP
  API 面。

版本不应写死在架构契约中。当前评审可采用 Boost 1.91、Jansson 2.15 和 OpenSSL 3.5
LTS 作为初始验证基线，但最终 manifest 固定的是实际审查过的 patch release。

## 15. 测试与验收

### 15.1 TDD 是全局交付规则

Host、Runtime 前置改动、编译工具和运维脚本全部遵循同一循环：

1. 先提交一个因缺少目标行为而失败的自动化测试；安全 gate 先写拒绝用例，再写允许
   用例；
2. 只实现让当前切片转绿的最小生产代码，不先铺未被测试驱动的通用框架；
3. 在测试保持全绿时重构，并把新发现的边界条件变成回归测试；
4. 一个切片同时包含测试、实现、必要文档和可观察错误，不接受“功能先合入、以后补
   测试”；
5. 单元测试使用 fake clock、fake filesystem/adapter 和确定性 scheduler；真实 Linux
   kernel、真实 worker 与 crash 测试另设 integration suite，不能用 mock 结果替代；
6. coverage 只是提示，合入门以契约、负控、状态机不变量和故障注入是否被执行为准。

每个 PR/commit 描述都要写出 `RED` 测试名、它最初如何失败，以及 `GREEN` 后证明了
什么。修 bug 时，复现测试必须先在未修代码上失败。

### 15.2 单元和属性测试

- JSON 重复 key、未知字段、深度、超限和单位解析；
- App 申请不可能扩大 Host 权限的单调性 property；
- path ancestor、deny、wildcard hostname、CIDR 和 port 交集；
- generation digest 和 rule ID 可复现；
- subdomain/path/header 路由正负控；
- pool 选择、queue、permit 和错误映射；
- deploy/worker 状态机所有非法转移；
- active pointer 原子恢复。

### 15.3 HTTP 和流控集成

- chunked、Content-Length、TE/CL 冲突和 smuggling corpus；
- header 数量/字节、慢头、慢 body 和 early body；
- `Expect: 100-continue` 的接受与拒绝；
- 大 request body 在 credit=0 时不继续读取；
- response credit 只在 client write completion 后归还；
- 慢客户端、SSE、断开、cancel 和迟到事件；
- 多 request ID 交错；
- worker crash 在 response head 前后不同语义；
- shutdown/drain/terminate 不阻塞 reactor。

### 15.4 部署故障注入

在每个步骤后强制 kill Host 并重启：

- source copy 中途；
- generation fsync 前后；
- COMPLETE 前后；
- pool READY 前后；
- active temp write、fsync、rename 和 parent fsync 前后；
- Registry publish 前后；
- 旧 pool drain 中。

验收不变量：重启后只可能得到旧 active 或完整的新 active，绝不能指向半个 generation。

另测：symlink/magic link/device/FIFO、并发原地修改、digest mismatch、ENOSPC、只读目录、
相同 Version 不同内容、并发 deploy 和 secret 变化。

### 15.5 可信字节码与 secret

可信字节码按以下顺序驱动实现：

1. compatibility identity golden 先失败，再实现 library/worker/compiler 三方一致性；
2. compiler round-trip 先失败，再证明同一源码、同一 `sourceName` 的 bytecode 能被真实
   worker 加载并与源码行为一致；
3. attestation verifier table test 逐字段篡改、摘要不符、重复/未知字段、未知/撤销 key、
   非法签名和错误 App/Version/sourceName；然后才实现 verifier；
4. 真实部署测试覆盖可信字节码路径、无字节码源码路径、兼容性失配源码回退，以及
   provenance 失败绝不回退；
5. fuzz attestation parser 和签名消息重建；ASan/UBSan 下把随机 bytes 隔绝在 trusted
   API 之前。

secret 按以下顺序驱动实现：

1. schema 负控覆盖未知 key、越权 env 名、路径字符、重复 key、超长值、NUL 和总量超限；
2. safe-read 测试先构造 symlink、FIFO、device、换 inode/size、越界 path 和读取中修改，
   再实现基于 dirfd/openat2 的读取；
3. Policy Compiler golden 证明只有 `Host allow ∩ App request` 的键值进入 descriptor，
   `effective.json` 只有 key 与 opaque revision；
4. 真实 worker 集成证明授权代码读到精确 value，未授权/重复 key 启动失败，不同 worker
   和 App 不串值，也不存在 ambient environment fallback；
5. rotation 测试证明旧 READY worker 保持旧快照，新 generation 预热后原子切换，且捕获
   的 admin response、日志和 metrics 中均找不到 secret canary。

### 15.6 隔离测试

- READY flags 必须覆盖 effective required bits；
- cgroup parent/leaf、limit 写回和 child membership；
- worker 自然处于与 Host 相同的 network namespace，Host 不打开或传递 netns fd；
- worker 无 ambient env/fd；
- App path 权限与 Landlock/openat2 一致；
- 构建产物和运行文件中不存在 supervisor socket、root helper 或 netns 创建入口；
- delegated 环境 skip 继续视为非证据。

### 15.7 性能验收

#### 基线推演，不是实测结论

现有 Vue SSR + SQLite 文档记录了 Capsid-16w 的 C32 均值 495.8 QPS、600 QPS 可持续
开放负载和约 762.5 QPS closed-loop 平台。若同一轮分层 profile 重新确认 Go gateway
约占 20 CPU units、workers 占 60–65 units，则：

```text
系统吞吐倍率 =
    (gatewayCost + workerCost)
    / (optimizedGatewayCost + workerCost)
```

| Gateway 自身 CPU 成本下降 | 推演的系统吞吐倍率 | 495.8 QPS | 600 QPS | 762.5 QPS |
| ---: | ---: | ---: | ---: | ---: |
| 30% | 约 1.08x | 约 535 | 约 648 | 约 824 |
| 50% | 约 1.13–1.14x | 约 560–565 | 约 680–684 | 约 862–869 |
| 67% | 约 1.19–1.20x | 约 590–595 | 约 714–720 | 约 907–915 |

v1 的合理目标假设是整体提升 8%–15%，20% 只作为优秀结果。该表是 Amdahl 推演，不是
承诺：C8/C16 还受每请求 50 ms 人工等待形成的 160/320 QPS 上限约束；固定 16 workers
若已在 Runtime 内饱和，释放 Gateway CPU 也不会自动转化成 QPS。

当前 tree 缺少被文档引用的 `bench/` 原始报告，而且已提交文档尚未保存上述 20/60–65
分层 profile。M1 开始前必须恢复可运行的 benchmark harness，重新生成 Go baseline 和
原始 profile；不能把回忆值写成实测基线。

#### 每个性能切片的 profile gate

每个 Host 数据面里程碑和每个声称改善性能的 PR，都必须同时完成函数级 TDD 与以下
before/after 证据：

1. 完全相同的 bundle、Runtime/worker build、worker 数、inflight、connection、response
   size、cgroup CPU/memory、CPU affinity、loadgen、warmup、时长和到达模型；
2. 至少 3 个 measured run，保留全部原始输出，报告 median、离散度、完成率、QPS、
   p50/p95/p99 和 loadgen schedule lag；
3. Gateway 与 workers 使用独立 cgroup 或等价 process grouping，记录各自
   `usage_usec`、CPU/response、RSS/PSS、context switch 和 page fault，不能只给整机 CPU；
4. 优化前后各保存一次采样 profile：Go baseline 使用 pprof，C++ Host/worker 使用
   `perf record`/flamegraph 或目标平台等价工具；同时保存 `perf stat` 的 cycles、instructions、
   IPC、branches、branch-misses、cache-misses、task-clock 和 migrations；
5. Host trace 同步记录 queue wait、worker execution、time-to-head、IPC read/write wakeup、
   bytes/frame、credit stall、跨 shard 投递和 allocator 次数；profile instrumentation 的
   headline benchmark 与诊断 run 分开，避免探针开销污染主结果；
6. 报告必须指出优化前的 dominant stack/counter、代码为何针对它，以及优化后该成本
   是否下降；没有 profile 指向目标路径，不进入实现。

原始命令、环境 manifest、commit、构建 flags、数据和报告必须从当前 tree 可追溯。
profile 只证明“时间花在哪里”，A/B benchmark 才证明“用户结果是否改善”；两者缺一，
不能合入性能结论。

比较 Go gateway 与第一方 Host 时，必须使用完全相同的 bundle、Runtime build、worker
数、inflight、connection、response size、cgroup、loadgen、warmup、时长和到达模型。
结果至少包含 QPS、完成率、p50/p95/p99、CPU/response、Host RSS、worker PSS、queue
wait、time-to-head、IPC bytes/syscalls 和 cancel/error。

先记录 baseline，再冻结 regression threshold；不能先写“C++ 必然更快”。只有 profile
持续指向 event loop/HTTP 层，才继续优化该层；io_uring、zero-copy 或共享内存仍需要
各自独立的 before/after profile。

### 15.8 发布门

- Release/LTO、ASan、UBSan、TSan 和 fuzz 全绿；
- Host HTTP/部署/故障注入矩阵全绿；
- delegated cgroup 与 Runtime egress policy 的正向证据；
- 配置 schema、示例、Policy Compiler 和 Runtime descriptor golden 一致；
- SBOM、依赖 hash、worker/library/Host/build identity 固定；
- A/B 报告含原始数据并可从当前 tree 追溯；
- 升级旧版本 Host 时，active state 和 App Version 可恢复；
- 运维文档覆盖 backup、rollback、drain、磁盘满、外部网络边界和 cgroup 委派故障。

## 16. 实施顺序

以下都是 v1 内部切片，不是把测试、可信字节码或 secret 推迟到 v2。每个切片先落一个
可观察失败的测试，再写最小实现。

### M0：可执行契约

1. `host_config_rejects_network_namespace_field` 先失败；修订 Host/App schema，补 listener、
   capacity、queue 和 trusted bytecode keys，并拒绝 netns 配置字段；
2. `runtime_worker_compiler_identity_matches` 先失败；增加 library/worker/compiler 三方
   compatibility identity；
3. `bytecode_attestation_rejects_one_bit_tamper` 先失败；冻结 attestation 签名消息、
   Ed25519 verifier 和结构化错误；
4. `secret_value_never_appears_in_effective_config` 先失败；冻结 secret snapshot、revision、
   redaction 和 generation digest；
5. `active_recovery_never_selects_incomplete_generation` 先失败；冻结 `active.json`、fsync、
   crash recovery 与 fake filesystem 接口；
6. 建立 Host test target、fake worker、fake clock、sanitizer job 和依赖锁。

完成条件：所有 v1 公共契约都有 golden 和负控，compiler 成为正式 target；生产 Host 代码
仍可很少，但不能存在未被测试表达的安全分支。

### M1：artifact、secret 与单 worker 闭环

1. 从 compiler round-trip 失败测试开始，实现源码 → bytecode → 真实 worker 的同构执行；
2. 从 attestation 篡改 table test 开始，实现安全复制、验签、摘要、sourceName 与兼容性
   选择；
3. 从 secret symlink/FIFO/NUL/越权测试开始，实现安全读取、Policy Compiler 和
   `capsid_env_entry[]` 快照；
4. 从 `one_request_waits_for_credit` 开始，实现 Unix admin socket、单一 path listener、
   一个 worker 的 begin/write/end、response credit、cancel 与 timeout；
5. 同时覆盖四条真实路径：源码、可信字节码、兼容失配回退源码、secret 进入 worker。

完成条件：单 worker 端到端测试证明 bytecode 与 secret 的 v1 契约；任一签名/摘要错误
fail closed，secret canary 不出现在 Host 输出。

### M2：静态池和可靠部署闭环

1. 从 pool/queue 状态机失败测试开始，实现固定 `minReady == maxWorkers`、shard owner、
   admission、慢客户端和 SSE；
2. 从每个持久化边界的 crash test 开始，实现 stage → prewarm → health → active rename
   → drain；
3. 从 rotation test 开始，实现 secret revision 变化生成新 pool；
4. 从 bytecode key rotation/restart test 开始，实现 provenance 随 generation 固化；
5. 增加结构化日志、固定指标和明确回退原因。

完成条件：失败永远保留旧版本，重启只恢复完整 generation，请求全程有界；字节码和
secret 在蓝绿、回滚和重启中保持相同语义。

### M3：生产 v1 发布门

1. subdomain 与 trusted-header listener；
2. delegated cgroup hierarchy；验证 Host 不包含 netns 配置、supervisor 或网络管理代码；
3. Host/global/App 完整 admission control；
4. 幂等操作查询、显式 rollback、generation retention/GC；
5. 结构化 Runtime 错误；
6. 全量安全、fuzz、sanitizer、soak、性能 A/B 和 crash matrix；
7. systemd unit、外部网络边界、权限、key rotation、secret rotation、升级和运维文档。

完成条件：在目标 Linux 环境完成 strict isolation、可信字节码与 secret 的正向和负向
证明，并通过生产流量/发布故障门。至此才称为 v1。

### M4：数据驱动的后续能力

- `minReady < maxWorkers` 的有界扩缩容；
- startup fairness、circuit breaker；
- 安全 ceiling reload；
- 可选 OTLP adapter；
- HTTP/2、内置 TLS 或第三方 transport adapter；
- 只有 profile 证明后才考虑 io_uring/zero-copy。

## 17. 已确认的决定

1. **源码 + 可信字节码都进 v1**：字节码必须通过签名 provenance、摘要、sourceName
   和 compatibility identity 校验；源码始终保留用于兼容回退；
2. **secret 通过 `capsid:env` 进入 worker**：按权限交集生成不可变快照，轮换生成新
   generation，任何 Host 输出不含明文；
3. **C++20 + Asio/Beast**：保留 C++ owner-shard，但不手写 epoll/HTTP parser；
4. **保持简单**：`active.json` 原子指针，不引入 SQLite；静态池先行，autoscaling 后置；
5. **不做 Host netns supervisor**：Host/App 没有 netns 配置；worker 自然使用 Host 的
   网络环境，额外网络隔离完全属于可选的部署环境措施。

## 18. 外部选型依据

- [Boost.Beast HTTP 文档](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_http.html)：
  HTTP/1 增量解析、序列化和 buffer-oriented 接口；
- [Boost.Asio POSIX stream descriptor](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview/posix/stream_descriptor.html)：
  接管现有 POSIX fd，并执行异步 read/write/wait；
- [Jansson 解码 API](https://jansson.readthedocs.io/en/latest/apiref.html)：
  `JSON_REJECT_DUPLICATES` 可直接拒绝安全配置中的重复 key；
- [OpenSSL release strategy](https://www.openssl-library.org/policies/releasestrat/)：
  OpenSSL 3.5 是支持到 2030 年的 LTS 系列；
- [systemd cgroup delegation](https://systemd.io/CGROUP_DELEGATION/)：
  非 root service 管理受委派 cgroup subtree 的边界。

## 19. 最终建议

Host 规划不需要推倒重来。最重要的调整是把 v1 从“功能列表”改成一个可证明的垂直
闭环：

> 源码目录安全快照，类型化权限编译，固定池预热，原子 active 指针，旧池有界排空，
> 可信字节码经完整信任链进入 worker，secret 经最小权限 `capsid:env` 快照进入 worker，
> 请求和响应始终受 credit、queue、deadline 与 Linux isolation 共同约束。

按 M0 到 M3 的 TDD 切片逐步把这个闭环做小、做严；v1 完成后，再用真实 profile 决定
扩缩容、HTTP/2 和更复杂控制面。这样既保持当前计划的产品简洁性，也与现有 Runtime
的真实能力边界一致。
