# Capsid Host v1 详细设计

> 状态：v1 权威设计；M1 数据面与安全部署闭环已交付（以当前源码和测试为准），
> static-pool/managed 属可运行的 benchmark/integration 模式，非生产部署接口。
> Runtime 权威接口：[runtime.h](../include/capsid/runtime.h)；集成约束见
> [第三方宿主集成规范](host-integration.md)。

## 1. 文档定位和冻结结论

本文是第一方 Host 的唯一权威设计。旧的规划、评审过程和每日状态页已经删除；某项能力
是否完成，以当前源码和测试为准，不能从本设计的存在推断实现已经交付。

冻结的核心决定：

- Host 是独立的一方宿主进程，Runtime 继续只负责单 worker 和 FetchRPC；
- 一份 Host 上限与一份 App 申请做交集，App 不能扩大权限；
- 每个 worker 一生只属于一个不可变 App Version；
- 新版本预热成功后再切换，任何失败保持旧版本；
- worker 固定归属一个事件循环 owner，完整执行 credit、cancel 和 drain；
- v1 使用 HTTP/1.1，TLS/HTTP/2 先交给成熟反向代理；
- 不因为猜测性能而引入 io_uring、共享内存 IPC 或自定义 HTTP parser。

v1 同时冻结以下约束：

1. v1 同时支持源码和可信字节码；`bundle.qjsb` 只有通过签名 provenance、摘要、精确
   source name 和 Runtime compatibility ID 校验后，才能进入 trusted bytecode API；
2. `env.valueFrom` 读取的 secret value 作为不可变 `capsid:env` 快照进入 worker；这是
   v1 的明确安全契约，而不是实现泄漏；
3. `host.json` 包含 listener、管理 socket、全局容量和队列硬边界；
4. 权限交集不是简单字符串集合交集，需要类型化、规范化的 Policy Compiler；
5. 部署 API 在第一阶段就承诺蓝绿语义，因此 staging、预热、原子切换和 drain 也
   必须在第一阶段形成垂直闭环；
6. `active.json` 需要明确原子落盘与恢复语义，但 v1 **不需要数据库**；
7. compatibility identity 与 attestation verifier 由 M0 核心提供；结构化启动错误和
   非阻塞 worker 回收必须在数据面接入前完成，回收不能阻塞 reactor；
8. Host 使用单一 HTTP framing authority，并固定 hop-by-hop header、流式 body、
   慢客户端和自动重试规则；
9. active generation 使用 worker crash replacement、指数退避、跨 App 公平和 crash
   budget，超限后 fail closed；
10. 显式下线使用 retire 管理动作和 crash-safe tombstone，不以删除目录表达；
11. worker 可观察的绝对 URL、path rewrite 和 Forwarded/X-Forwarded 信任边界必须由
    8.2 的单一规范化契约冻结，listener 不得自行派生另一套规则；
12. 部署后使用最小持续健康探针，SSE 长连接有独立容量保护；
13. 公网 C++ Host 与全局 Admin socket 的残余权限边界明确写入 threat model。
14. Linux 是 v1 生产目标，macOS/Windows 原生开发是独立产品契约；
    Host 只决定和验证隔离能力，Runtime 负责平台 process/transport/sandbox，
    不支持的生产隔离必须 fail closed。

v1 技术栈：

| 领域 | 选择 | 原因 |
| --- | --- | --- |
| Host 语言 | C++20，仅 Host target 使用 | 直接调用现有 C ABI；复用 CMake 与 C++ 工程经验 |
| 事件循环与 HTTP/1 | Boost.Asio + Boost.Beast | 覆盖 epoll/kqueue/IOCP；平台 adapter 接管 worker event source；提供增量 HTTP/1 parser/serializer |
| 配置 JSON | Jansson | API 小；能显式 `JSON_REJECT_DUPLICATES`，适合安全配置 |
| 摘要与验签 | OpenSSL `EVP` SHA-256 / Ed25519 | 不自写哈希或签名实现；后续如需 TLS 仍可复用 |
| 持久状态 | 普通文件 + `fsync` + 原子 `rename` | 单进程、单写者足够；不引入 SQLite |
| 指标 | 内建固定指标 + `/metrics` 文本端点 | v1 不引入完整 telemetry SDK；避免高基数与 exporter 故障 |
| TLS/H2 | 外部 nginx/Caddy/Envoy | 先收敛发布、调度和隔离，不同时维护边缘协议栈 |

没有选择的方案：

| 方案 | v1 不选的原因 |
| --- | --- |
| raw epoll + 自写 HTTP 状态机 | 重复实现 parser、timer、半包和生命周期；安全收益为零 |
| Rust/Tokio/Hyper | 内存安全优势真实存在，但当前仓库没有 Rust 基础，会增加第二套构建、FFI 和 CI；若团队主要能力在 Rust，可重新评估 |
| 继续 Go/cgo | 保留为 A/B 基线；第一方 Host 直接拥有 worker fd 和 ABI，是否更快仍由数据证明 |
| SQLite/其他数据库 | v1 单进程单写者，只需要一个活动版本指针，没有数据库查询或事务需求 |
| Boost.JSON 作为安全配置 parser | 重复 key 采用 last-wins，不能直接满足 fail-closed 配置要求 |

当前仍有效的集成约束：`capsid_worker_destroy()` 是同步的有界回收路径，最坏会
等待数百毫秒，因此不能在数据面 reactor 回调里执行；宿主应先在 owner shard 摘除
并关闭 worker，再把唯一 handle 移交给有界 reaper executor。

## 3. 关键契约补充

### 3.1 产品边界

“Runtime 管单 worker，Host 管 HTTP、路由、池、发布和过载”的边界是正确的。
`capsid-host` 是独立 executable，Host 内部组件使用 `capsid_host_core`，但 v1
不承诺第二套公共稳定 ABI。

### 3.2 `host.json` / `capsid.json` 两层模型

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

### 3.3 Fetch scheme

早期配置样例的 Host 上限写成 `*.internal.example.com:443`，App 申请却写成
`https://orders-api.internal.example.com:443`。当前 Runtime egress 判定只接收
host/IP/CIDR 和 port，不能区分 `http` 与 `https`。

v1 配置语法统一为 `host:port`，明确它不保证 URI scheme。若以后需要
HTTPS-only，必须先扩展 worker/ABI，使 policy 判定包含 scheme；配置不能先表达 Runtime
无法执行的安全承诺。

### 3.4 Secret 语义

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

M0.3 冻结的纯 Policy Compiler 边界不接收路径或 ambient environment，只接收 App env
申请、Host environment allowlist，以及后续 safe-read provider 产生的
`(keyId, value, opaqueRevision)`。App env name 使用 Runtime 的 ASCII 标识符语法，Host
allowlist 额外允许单个末尾 `*`；`value`/`valueFrom` 必须且只能出现一个。secret key ID
最长 128 bytes，只允许 ASCII 字母、数字、`_`、`-`、`.`，不能包含 `..`；opaque revision
最长 256 bytes，只允许 ASCII 字母、数字和 `.`、`_`、`:`、`@`、`+`、`-`。provider
输出必须与申请的 distinct key 集合精确相等，缺失、重复或多余 material 都 fail closed。
所有配置字符串都拒绝 NUL；object member name 含 NUL 时沿用 Jansson 上游的严格解析
行为，作为 `kInvalidJson` 定位到文档根，不为获得更细路径而放宽 vendored parser。

这是有意选择的能力模型：获得该 key 权限的应用代码可以读到值。v1 不再同时承诺
“通过 `capsid:env` 使用 secret”和“secret 内容不进入 worker”这两个互斥目标。

### 3.5 可信字节码信任链

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
长度和 UTF-8 bytes。普通 claim 各自不得超过 1024 UTF-8 bytes；schema 必须精确为
`capsid-bytecode-v1`，三个 SHA-256 字段必须精确为 `sha256:` 加 64 位小写十六进制。
这些结构/语法检查先于验签；验签后才比较部署期 expected claims、实际源码/字节码摘要和
当前 compatibility ID。未知 JSON key 的诊断路径必须做 RFC 6901 转义。Host 重建消息后
使用 OpenSSL EVP 验签。

M0.2 的选择器输入中，源码始终存在；`bundle.qjsb`、`bytecode.json`、`bytecode.sig`
使用 all-or-none optional 表达，三者全无选择源码，部分存在直接拒绝。完整且 provenance
有效、identity 相同才选择 trusted bytecode；只有签名、claims 和两个摘要均有效而
identity 不同，才返回带 `/compatibilityId` 原因的源码回退。任何未签名的 identity 篡改
必须是签名失败，不能伪装成兼容回退。验证结果同时保留安全的 key ID 与原始 attestation
SHA-256，供 generation identity 使用。

选择结果只有四种：

| 版本目录状态 | 结果 |
| --- | --- |
| 三个 bytecode 文件都不存在 | 加载源码 |
| 三者齐全、provenance 有效、identity 匹配 | 加载可信字节码 |
| 三者齐全、provenance 有效、仅 identity 失配 | 记录原因并加载源码 |
| 文件不齐或任一 provenance 校验失败 | 部署失败，旧版本保持 active |

当前仓库已有 trusted bytecode Runtime API、worker 加载路径、正式 compiler target、
compatibility identity 和 attestation verifier。后续 Host 数据面只能把完整通过本节
信任链的产物交给 trusted API。

### 3.6 部署垂直闭环

`/v1/deploy` 的最小垂直闭环固定为：

```text
安全读取 → 校验/编译 → 内部快照 → spawn/load/READY
       → 可选健康检查 → 原子 active 切换 → 旧池 drain
```

可以推迟 autoscaling、完整 reload、TLS/H2 和多种 transport，但不能推迟原子切换和
失败保持旧版本。

### 3.7 listener 配置

当前 `host.json` 示例没有 listener，但后文同时支持 subdomain、path 和 trusted
header，无法决定绑定地址、路由模式和信任边界。每个 listener 必须只配置一种主要
路由模式，避免隐含优先级。

Header routing 仅允许 Unix socket，或具备 mTLS/source allowlist 的独立内部 TCP
listener。公网 listener 必须删除客户端提供的同名控制 header。

### 3.8 整机资源上限

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

### 3.9 默认值必须用证据校准

示例中的 `maxInflight=32`、`maxWorkers=16` 等只能作为讨论值，不能直接冻结为
`host-v1` 默认。现有性能文档也强调不同 workload 的最优 worker/inflight 不同。

v1 alpha 阶段应先要求显式 pool size，完成固定 workload 的容量扫描后再冻结默认值。
Runtime 自身 ABI 默认值是底层兜底，不应自动成为 Host 产品默认值。

### 3.10 无配置启动

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
                    本机 Admin endpoint
                   (v1 生产为 Unix socket)
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
        └── WorkerEventSource adapter / FetchRPC / credit ──┘
                                 │
                bounded bootstrap + reaper executors
                                 │
                         isolation boundary
              delegated cgroup / Host network environment
```

### 4.1 进程和线程

`capsid-host` 使用：

- 1 个 control thread：配置、部署/恢复状态机、`active.json` 和 registry 发布；
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
`capsid_worker_fd()`；macOS 使用 kqueue，Windows 后端可使用 IOCP。Beast 提供
跨平台的增量 HTTP/1 解析和序列化。Host 仍然保持 owner shard 模型，
但无需自己重写 fd/HANDLE 注册、timer、HTTP framing 和半包状态机。

为防止当前 ABI 把 Host 锁死在 POSIX，只有 `WorkerEventSource` 平台 adapter
可以直接调用 `capsid_worker_fd()`。Pool、routing、request、credit 和 lifecycle
只观察“可读/可写/已关闭”语义，不包含 `_WIN32` 或 POSIX 分支。Windows
实现时，Runtime 内部把进程创建、worker transport 和 sandbox 拆成平台后端；
新的 waitable/event-source C ABI 只能加法扩展 ABI v7，其具体类型、ownership 和
wakeup 语义先由 Windows RED 测试冻结。

Beast 不是完整 Web server：Host 仍需实现路由、header 策略、body credit、超时和
错误映射。这恰好是 Capsid 产品逻辑，而不是重复实现通用协议 parser。

Runtime target 继续保持 C++11；只给 `capsid-host` 和 `capsid_host_core` 设置
`CXX_STANDARD 20`，不借 Host 项目升级破坏现有 ABI 或兼容测试。

## 5. 配置方案

### 5.1 修订后的 `host.json` 轮廓

以下字段是结构建议；普通容量值仍需 profile 校准。`recovery` 和 streaming 数值是 v1
候选值，必须通过 fake-clock、crash-loop、SSE soak 和故障注入验证后才能冻结：

```json
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/apps",
  "stateRoot": "/var/lib/capsid",
  "secretRootTemplate": "/run/capsid/secrets/{application}",
  "admin": {
    "unix": "/run/capsid/admin.sock",
    "mode": "0600"
  },
  "listeners": [
    {
      "name": "public",
      "tcp": "127.0.0.1:8080",
      "publicScheme": "https",
      "routing": {
        "mode": "subdomain",
        "suffix": ".apps.example.com"
      },
      "limits": {
        "connections": 4096,
        "headerBytes": "64KiB",
        "headerTimeout": "5s",
        "bodyIdleTimeout": "30s",
        "streamIdleTimeout": "60s"
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
    "request": {
      "maxStreamingInflightPerWorker": 2
    },
    "pool": {}
  },
  "maximums": {
    "worker": {},
    "request": {
      "maxStreamingInflightPerWorker": 2
    },
    "pool": {}
  },
  "capacity": {
    "workersTotal": 128,
    "startupsConcurrent": 4,
    "queuedRequestsTotal": 4096,
    "queuedHeaderBytesTotal": "64MiB",
    "workerMemoryCommitTotal": "24GiB"
  },
  "recovery": {
    "crashBudget": {
      "maxEvents": 5,
      "window": "60s"
    },
    "restartBackoff": {
      "initial": "250ms",
      "maximum": "30s",
      "jitter": "20%"
    },
    "replacementsConcurrentPerApp": 1,
    "activeHealthInterval": "30s",
    "activeHealthFailures": 2
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
    "maxInflightPerWorker": 8,
    "maxStreamingInflightPerWorker": 2
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

v1 对 `host.json` 和 `capsid.json` 使用同一组解析资源上限：原始输入最多 1 MiB（含），
且必须在创建 JSON DOM 前检查；JSON value nesting 最多 64 层，根 value 计作第 1 层。
任一上限超出都返回稳定的 `kResourceLimit` 和根 JSON Pointer `""`，不再进入未知字段或
值校验。nesting 由 Jansson parser 自身的深度限制执行，不能在其前面再写一个按括号计数
的简化 JSON 扫描器；字符串中的 `{`、`[` 和转义内容不得影响深度判断。

M0.1 一次完成 5.1/5.2 所示 Host/App 结构、权限容器、数组和动态 key map 的递归类型检查；
单位语法与规范化、Host/App 上限交集、listener 路由条件、secret 的 `value`/`valueFrom`
互斥和 attestation 语义分别由后续对应契约处理，不再拆成逐字段的 M0.1 子里程碑。

选择 Jansson 的唯一关键理由，是它能用公开 flag 直接拒绝重复 key。Boost.JSON 的 DOM
parser 对重复 key 保留最后一个值，不适合作为安全配置的唯一 parser。配置不在热路径，
解析性能不是选型指标。

## 6. 版本快照与持久状态

### 6.1 为什么 `active.json` 足够

`active.json` 不是用户配置，它是 Host 内部单写的 App 服务状态记录。active 形态例如：

```json
{
  "schema": "capsid-active-v1",
  "app": "orders",
  "state": "active",
  "version": "2026-07-31-002",
  "generation": "sha256:8f3a9c..."
}
```

显式下线使用 retired tombstone，而不是删除文件：

```json
{
  "schema": "capsid-active-v1",
  "app": "orders",
  "state": "retired",
  "previousVersion": "2026-07-31-002",
  "previousGeneration": "sha256:8f3a9c..."
}
```

crash budget 超限时保存 `state: "quarantined"`，并保留对应 Version/generation 和稳定
reason code。三种状态共享同一个原子替换协议，不增加第二份状态文件。

```json
{
  "schema": "capsid-active-v1",
  "app": "orders",
  "state": "quarantined",
  "version": "2026-07-31-002",
  "generation": "sha256:8f3a9c...",
  "reason": "CRASH_BUDGET_EXCEEDED"
}
```

M0.4 把该内部文件冻结为最大 16 KiB 的严格 JSON object：拒绝重复/未知字段、NUL、错误
类型和尾随输入；`schema` 必须精确为 `capsid-active-v1`，`app` 必须与正在恢复的 App
精确相等，App/Version ID 与本设计 5.2 的 ASCII 规范一致，generation 必须是
`sha256:` 加 64
位小写十六进制。active 只允许并要求 `version/generation`；retired 只允许并要求
`previousVersion/previousGeneration`；quarantined 只允许并要求
`version/generation/reason`，v1 reason 固定为 `CRASH_BUDGET_EXCEEDED`。规范化输出是无换行
单行 JSON，字段顺序固定为 `schema/app/state` 再接状态专属字段。

v1 的约束是：

- 一个 Host 进程；
- 一个 control plane writer；
- 每个 App 同时最多一个 deploy/retire 状态变更操作；
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

- 内容和有效配置摘要相同：Version 映射幂等并复用既有 generation；deploy 是否可以短路
  仍按 7.3 的服务状态判断——已 active 才直接返回，retired/quarantined 必须重新预热；
- 摘要不同：返回 `VERSION_IMMUTABILITY_CONFLICT`，不能覆盖旧映射。

### 6.3 generation identity

不能只用 bundle bytes 作为 generation identity。相同源码在权限、资源、Host 配置或
secret revision 改变后必须生成不同 worker pool。

```text
generationDigest = SHA-256(binaryRecord)
```

`binaryRecord` 固定从包含末尾 NUL 的 `"capsid-generation-v1\0"` 开始，随后按上述顺序
编码十个字段；每个字段都是 32-bit big-endian byte length 加原始 UTF-8 bytes。
`selectedArtifactKind` 只允许稳定 ASCII 值 `source` 或 `trusted-bytecode`，无 attestation
时第三个字段编码为空字符串。最终公开形式是 `sha256:` 加 64 位小写十六进制。长度前缀
消除拼接歧义，任何字段（包括 secret revision 和实际选择的 artifact）变化都生成不同
generation。

即使本次因 compatibility mismatch 回退源码，也要记录已验证 attestation、回退原因和
实际 selected artifact。重启不得在不改变 generation identity 的情况下从源码静默切到
另一份字节码，或反向切换。

secret revision 不保存明文或 secret value 的裸摘要。secret provider 应返回不含秘密的
opaque revision；最简单的文件 backend 可使用已打开文件的 device、inode、size 和
`ctime` 组成 revision，并在读取前后复核。即使相同内容重写后生成新 generation 也可
接受，比再引入一套 Host 密钥管理更简单。

多个 secret 的聚合 revision 固定为 `sha256:` 加以下 binary record 的 SHA-256：包含末尾
NUL 的 `"capsid-secret-revision-v1\0"`，先编码 App ID，再按 environment name 排序，
对每个 secret env 依次编码 env name、key ID、provider opaque revision；每项同样使用
32-bit big-endian length prefix。literal env 不进入 secret revision，因为其值已由
normalized App config digest 覆盖。请求 JSON 顺序不得改变聚合 revision。

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

### 6.5 `active.json` 原子状态切换

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

retire 和 quarantine 复用步骤 3–8：先在内存中阻止新路由，再写入对应 tombstone/state。
它们不需要新 generation READY，但同样必须 `fdatasync`、原子 rename 和 `fsync` 父目录。
两者的写失败语义不同：retire 在 rename 前失败时恢复原 Registry，让旧版本继续服务；
quarantine 写失败时仍保持内存 fail closed、停止 replacement，并持续重试落盘及发出不可
丢弃告警，绝不能为了报告写失败而恢复 crash-loop 流量。存储恢复前不得主动重启 Host；
若进程仍因节点故障消失，旧 active 状态可能在下次启动再次触发 replacement，因此磁盘
不可写属于需要运维介入的 durability incident，而不是可静默降级的正常路径。

顺序必须是“持久指针成功后，再发布内存指针”。若进程在两者之间崩溃，重启会从新的
`active.json` 恢复；若在 rename 前崩溃，旧版本仍 active。临时文件在启动恢复时清理。

rename 成功但父目录 `fsync` 失败不能伪装成普通的“未提交”错误：此时内存 Registry 不
发布新状态，操作进入 `DURABILITY_UNCERTAIN` 并阻止同 App 后续状态写，直到控制面完成
对账或进程按 durability incident 退出。节点若在对账前崩溃，文件系统可能恢复旧或新的
完整原子目录项；两者都允许，但恢复仍必须验证 active 所指 generation 的 COMPLETE，
绝不能把临时文件或半 generation 激活。只有父目录同步完成后才能返回提交成功。

启动恢复只信任完整的 `active.json`：`active` 必须引用存在且完整的 generation；
`retired` 不恢复 pool；`quarantined` 不自动重启 worker。无效状态让该 App fail closed
并报告明确错误，不能扫描目录后擅自选择“最新版本”。
缺少 `active.json` 表示该 App 没有服务状态，不扫描 generations；retired/quarantined
恢复也不要求旧 generation 仍保留。启动时清理 `active.json.tmp.*` 是 best effort：失败
产生运维告警但不改变从有效 `active.json` 得出的状态。

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

纯编译阶段输出按 env name 排序的 owning snapshot 和临时 `capsid_env_entry[]` view。
`effective.json` 的环境片段固定为单行 canonical JSON，只记录 name/source；secret 项再
记录 key ID 与 opaque revision。例如：

```json
{"environment":[{"name":"API_TOKEN","source":"secret","keyId":"orders-api-token","revision":"file-v1:11:22:41:1700000000"},{"name":"APP_MODE","source":"literal"}]}
```

literal 与 secret value 都不进入该 JSON、revision、错误 path/message、管理响应、日志或
指标。编译失败必须原子返回空 snapshot，不能保留此前已处理的部分 entry。

文件 mode 不等于完整 secret 管理系统；挂载、备份、节点 swap/core dump 和 secret root
的生命周期仍由部署环境负责。Host 的责任是最小读取、最小传递和不把明文扩散到控制面。

## 7. 部署、下线 API 和状态机

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

Admin socket 是刻意的全局 trust boundary：通过 `SO_PEERCRED` 获准的 UID 或管理 group
可以部署、回滚和下线该 Host 中的所有 App。v1 不声称提供 per-App 管理授权；互不信任
的运维主体必须使用不同 Host 实例和管理 socket。

唯一能激活某个 Version 的操作仍是：

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

显式下线使用 `POST /v1/apps/{app}/retire`，但它不能选择另一个 Version。只读的
`GET /v1/operations/{id}`、`GET /v1/apps/{app}` 和健康/指标端点也不能改变版本。

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

ACTIVE → RETIRING → RETIRED
ACTIVE → DEGRADED → QUARANTINED
QUARANTINED → RETIRING → RETIRED
QUARANTINED/RETIRED → 显式 deploy → WARMING → ACTIVE
```

operation 记录可以是内存对象和有界 JSONL 运维日志，不需要为了查询它引入数据库。
Host 重启后只需准确恢复 active generation；中断的 deploy 可报告 `ABORTED_BY_RESTART`
并由调用方幂等重试。

M0.6 把 durable state 与内存 phase 明确分开。`active.json` 仍只有 M0.4 冻结的
`active|retired|quarantined`，不会把 `RETIRING`、`QUARANTINING` 或
`DURABILITY_UNCERTAIN` 当成新磁盘 schema。内存 phase 冻结为：

```text
ABSENT | ACTIVE | RETIRING | RETIRED | QUARANTINING | QUARANTINED
       | DURABILITY_UNCERTAIN | FAILED_CLOSED
```

路由派生必须 fail closed：只有结构完整的 `ACTIVE` 可以服务并允许 automatic
replacement；`ABSENT`、`RETIRED`，以及已经提交 retired tombstone 但仍在 drain 的
`RETIRING` 返回 404；其他 phase 一律 503。开始 retire 后、tombstone 提交前也先返回
503，不继续接收新流量；提交成功后即使旧 pool 尚在 drain 也改为 404。未知 phase、phase
与 document 类型不匹配或恢复 action/document 不匹配都进入 `FAILED_CLOSED`，绝不能因
默认 `else` 分支变成 active。

M0.4 的 persist 结果进入内存状态机时固定按下表解释：

| 操作 | persist 结果 | 内存结果 | 路由/后续动作 |
| --- | --- | --- | --- |
| retire | rename 前失败 | 恢复来源 `ACTIVE` 或 `QUARANTINED` | active 来源恢复服务；quarantined 仍 503；操作失败 |
| retire | rename + directory sync 成功 | 保留 `RETIRING`，document 换成 retired | 立即 404 并开始 drain；drain 完成后 `RETIRED` |
| quarantine | rename 前失败 | 保持 `QUARANTINING` | 503、禁止 replacement，并重试同一 quarantined document 的落盘 |
| quarantine | rename + directory sync 成功 | `QUARANTINED` | 503、禁止 replacement，并开始有界 drain |
| 任一操作 | rename 成功但 durability uncertain | `DURABILITY_UNCERTAIN` | 503、阻止后续状态写，等待对账/进程级处置 |
| 任一操作 | persist result 内部自相矛盾 | `DURABILITY_UNCERTAIN` | 按实现/适配器缺陷 fail closed，不猜测提交状态 |

重启恢复计划只允许 M0.4 的 `kActivate` 启动 pool，而且必须等恢复 pool READY/healthy 后
才发布 active route；`kNone`、`kKeepRetired`、`kKeepQuarantined` 和任何恢复错误都不
spawn worker。这样 retired/quarantined 的安全性不依赖易失的 operation 状态。

### 7.3 并发和幂等

- 同一 App 的 deploy/retire 串行；
- 不同 App 可以并发，但共享全局 startup/memory permit；
- 相同 App/Version/generation 已 active：直接返回 active；
- 相同 generation 已 quarantined：显式 deploy 重新走预热并在成功后重置 instability
  budget，不按 active 幂等短路；
- App 已 retired：显式 deploy 正常创建/恢复 pool 并原子写回 active；
- 相同 Version 已映射到不同 generation：`409`；
- warming 中的完全相同请求加入同一个 singleflight；
- 新请求不会为每个调用分别 spawn 一套 pool。

显式 deploy 的纯决策表也在 M0.6 冻结：active 的同一 Version/generation 才能直接返回
already-active；同一 Version 指向不同 generation 始终是 immutability conflict；absent、
retired 和 quarantined（包括显式重部署完全相同的 quarantined generation）都必须走
`WARMING → active.json commit → ACTIVE`。quarantine budget 只能在新的 active pointer
已经提交并发布后重置，不能在收到 deploy 或开始 warming 时提前清除。`RETIRING`、
`QUARANTINING` 返回 busy；`DURABILITY_UNCERTAIN` 阻止 deploy。

### 7.4 健康检查

若配置 health check：

- 对每个新 worker 至少执行一次，而不是只检查 pool 中任意一个；
- 只允许 `GET`、无 body、固定小 response body 上限；
- 期望 `200..299`，完整消费或丢弃 body 并正确归还 credit；
- 使用独立 timeout 和 request ID；
- 任意 `minReady` worker 失败都不切换；
- health check 执行真实应用代码，文档要提醒不要产生业务副作用。

激活后继续复用同一探针做低成本主动健康感知：

- 按 pool round-robin 并加 jitter，使每个有空闲 inflight 槽位的 worker 约每 30 秒被检查
  一次；同一 App 最多一个健康检查并发；
- busy 或 streaming 已满的 worker 本轮跳过，不让健康检查抢占业务 slot，并记录 skip；
  skip 不算健康成功，也不重置连续失败计数；其 inflight 仍由 request deadline、stream
  idle timeout、同步 CPU timeout、IPC/protocol failure 和进程 EXIT 等被动信号兜底，
  任一被动失败按本节规则立即摘除；
- 连续 2 次 timeout、非 `2xx`、协议错误或异常退出后，worker 进入 `UNHEALTHY`，先从
  scheduler 摘除，再交给 reaper；
- 同步 CPU timeout、IPC/protocol failure 和意外 EXIT 不等待第二次探针，立即摘除；
- 健康摘除和 crash 都进入同一个 generation instability budget，防止“探针失败 → 无限
  recycle”绕过 crash budget；
- 未配置 `healthCheck` 的 App 只有 Runtime/IPC/timeout 等被动健康信号，不伪造业务探针。

### 7.5 drain

激活新 generation 后：

1. Registry 不再把新请求发给旧 pool；
2. 旧 pool 保持处理 inflight；
3. inflight 清零则 `shutdown`、继续 flush/read 到 EXIT；
4. drain deadline 到期，cancel 所有 request；
5. 短暂 cancel grace 后 terminate；
6. handle 移交 reaper executor 执行 destroy；
7. 记录总 drain 时间和被强制取消数。

### 7.6 显式下线

```http
POST /v1/apps/orders/retire
```

该操作无策略 body，返回 `202 Accepted` 和 operation ID；重复 retire 是幂等成功。固定
顺序为：

1. 获取 per-App operation mutex，把内存状态置为 `RETIRING`，立即停止接收新请求；
2. 用 6.5 的协议把 `active.json` 原子替换为 `state: "retired"` tombstone；rename 前
   失败就恢复旧 Registry，旧版本继续服务；
3. 发布无 active pool 的 Registry snapshot；普通数据请求统一返回 `404`，管理 API 仍
   显示 retired 和 previous generation；
4. 按 7.5 drain 全部 pool；操作在所有 worker 退出后变为 `RETIRED`；
5. generation 和 Version 映射按 retention/GC 规则保留，retire 本身不删除可回滚产物；
6. 后续显式 deploy 任一已有或新 Version 都可以重新激活 App。

Host crash 在 tombstone rename 前恢复旧 active，在 rename 后恢复 retired；不会根据上传
目录是否存在推断下线，也不会出现删除 `active.json` 后擅自选择最新 Version 的行为。

## 8. 路由和 HTTP 边界

### 8.1 一个 listener 一种路由模式

支持：

- `subdomain`：精确 DNS label 后缀；
- `path`：固定 `/@capsid/{app}/` 前缀；
- `header`：只用于受信内部 listener。

规则：

- App ID 只使用本设计 5.2 的 ASCII 规范；
- Host header 去掉合法端口后按 label 边界匹配，不能做裸字符串 suffix；
- path 模式在原始 path segment 上识别 App，拒绝 encoded slash、backslash、dot segment
  和非法 percent encoding；
- request 参数只能查询 Registry，永远不拼磁盘路径；
- 不能通过 URL/header 选择 Version、generation 或 worker；
- 控制 header 在构造 FetchRPC headers 前删除。

v1 的 `header` 模式固定使用单个 `Capsid-App` header。它只允许在已经由 Unix socket、
mTLS 或 source allowlist 证明为受信的内部 listener 上启用；“配置成 header mode”本身
不构成信任证明。缺少、重复或不符合 App ID 文法的 `Capsid-App` 都 fail closed。其他
路由模式即使收到该 header 也只删除、不使用。

### 8.2 Worker 可观察的 URL 与代理头

`Request.url` 是公共 App 契约，v1 固定由 Host 构造，绝不根据转发头猜测：

```text
request.url = publicScheme + "://" + validatedAuthority + rewrittenTarget
```

- 每个 TCP data listener 必须显式配置 `publicScheme: "http"|"https"`；这表示用户看到
  的外部 scheme，不要求 Host 自己终止 TLS；
- subdomain 路由的 authority 是按 suffix 规则验证后的请求 `Host`；path/header 路由
  必须配置固定 `publicAuthority`，不能让任意 Host 改变应用可见 origin；
- 只接受 HTTP origin-form request-target；absolute-form、authority-form 和 `*` 在 v1
  数据 listener 上拒绝；
- `Host` 只参与 authority 校验和 URL 构造，不作为普通 Fetch header 交给 worker；
- query bytes 原样保留，不 decode 后重编码；非法 percent encoding 在路由前拒绝。

authority 的 v1 文法刻意保持窄而可审计：只接受 ASCII DNS/IPv4 风格 host，加可选的
非空十进制端口 `1..65535`；host 转成小写，端口转成无前导零的十进制形式。DNS label
最多 63 bytes，完整 host 最多 253 bytes。禁止 userinfo、空 label、尾点、label 首尾
连字符、bracketed IPv6 literal 和超长 authority。subdomain
的 `suffix` 必须以 `.` 开头、不得带端口，并且请求 Host 必须恰好是“一个 App DNS
label + suffix”；suffix 自身、额外前置 label 和裸字符串后缀匹配都拒绝。App ID 中合法
但不是 DNS label 的名字（例如含 `_`）使用 path/header 模式。path/header 模式仍要求
请求中恰好一个语法合法的 Host，但 worker origin 只使用规范化后的固定
`publicAuthority`，请求 Host 无权改变它。v1 若需要 IPv6 public origin，先由外部代理
暴露 DNS authority，不在实现中临时放宽文法。

request-target 的 v1 文法同样固定：只能以 `/` 开头的 ASCII origin-form，最大 16 KiB，
禁止 raw control、space、backslash、fragment 和非 ASCII byte；每个 `%` 必须跟两个 hex
digit。path 部分拒绝 percent-encoded `/`、`\\`，也拒绝解码后恰为 `.` 或 `..` 的 segment；
query 中的 `%2F` 等合法 escape 可以保留。path 模式只从原始
`/@capsid/{app}` segment 取 App，不对 App decode；`/@capsid/{app}`、尾随 `/` 都重写为
`/`，query 原字节附回。规范化器不做 Unicode、dot-segment 或 percent canonicalization，
所以不会出现不同输入静默折叠成同一路由键。

路由重写表：

| 模式 | 客户端 target | worker `Request.url` 的 path/query |
| --- | --- | --- |
| subdomain | `/api/orders?x=1` | `/api/orders?x=1` |
| path | `/@capsid/orders` | `/` |
| path | `/@capsid/orders/api?x=1` | `/api?x=1` |
| header | `/api/orders?x=1` | `/api/orders?x=1` |

v1 对所有 data listener 使用同一 fail-closed 代理头规则：进入 Host 后总是删除
`Forwarded`、所有 `X-Forwarded-*` 和 `X-Real-IP`，既不用于路由/URL，也不交给 worker。
因此公网客户端不能伪造 scheme、authority 或 client IP；v1 也不向 App 暴露真实 client
IP。外部 TLS proxy 必须覆盖合法 `Host`，Host 的 `publicScheme` 配成 `https`。未来若确有
需求，再单独设计带 peer CIDR/mTLS 证明的 trusted-forwarding 契约，不在 v1 暗示信任。

进入 worker 前还要完成一个原子的 owning snapshot：header name 按 ASCII token 校验并
转成小写，value 只接受 HTAB 与可见 ASCII，保留输入顺序和值的原字节；最多 128 个
字段，原始 name/value 字节和最多 64 KiB。除 `Host`、`Capsid-App` 和上述 proxy header
外，还删除标准 hop-by-hop 字段，以及所有 `Connection` header 中逗号分隔、ASCII
case-insensitive 指名的字段；空或非法 Connection token 直接拒绝。`X-Forwardedness`
不是 `X-Forwarded-*`，不应误删。成功结果不引用 Beast buffer，失败结果不发布部分 App、
URL 或 header。

### 8.3 HTTP/1 安全规则

Host 需要在进入应用前统一处理：

- 拒绝冲突或重复的 `Content-Length`；
- 拒绝 `Transfer-Encoding` 与 `Content-Length` 冲突；
- 只接受 Beast 明确认可的 HTTP/1 framing；
- 删除 connection-nominated header 及所有 hop-by-hop header；
- 按 8.2 删除所有代理转发头；
- 禁止把 `Connection`、`Keep-Alive`、`Proxy-Connection`、`TE`、`Trailer`、
  `Transfer-Encoding`、`Upgrade` 原样交给 worker；
- 校验 header name/value、总字节和字段数；
- v1 禁止 WebSocket upgrade 和 CONNECT；
- v1 每连接只允许一个应用 request 正在处理，暂不实现 HTTP pipelining 并发；
- header、body idle、queue、Host request 和 response idle timeout 分开计时。

8.2 的纯规范化器接收 Beast 已经解析出的 target/header view，只负责语义验证、路由、URL
构造和清洗；它不得重新解释 `Content-Length`/`Transfer-Encoding` 或判断 message framing。
framing 冲突、重复 Content-Length 和 chunked 合法性始终只由同一个 Beast parser 权威
决定。`Transfer-Encoding` 之后仍作为 hop-by-hop 字段从 worker header snapshot 删除，
这不等于第二次 framing 判定。

Host 不能假设 Runtime 的 response header decoder 已执行 HTTP 语义过滤；当前 decoder
主要验证 FetchRPC 二进制结构。因此 response 也必须经过 hop-by-hop、长度和非法值检查。

### 8.4 `Expect: 100-continue`

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

- 收到 response head 后先检查 `Content-Type`；`text/event-stream` 必须先取得该 worker 的
  streaming permit，再向客户端发出 head；
- v1 默认 `maxStreamingInflightPerWorker=2`，App 只能申请更低值；该值必须小于
  `maxInflightPerWorker` 以至少保留一个普通请求槽位，只有
  `maxInflightPerWorker == 1` 时允许两者都为 1 并明确失去并发保留；
- permit 已满且尚未向客户端发出 head 时，Host cancel worker request 并合成 `503`，
  不能先发 `200 text/event-stream` 再断开；
- body frame 逐段写，不聚合到 response end；
- `text/event-stream` 禁止整体压缩和代理 buffering；
- 成功写下游后才归还 credit；
- 默认 stream idle timeout 为 60 秒；应用必须用注释/heartbeat 保持活跃，超时即 cancel；
- v1 不设强制最大持续时间：streaming permit 已形成确定容量边界，部署者可另行设置
  `maxStreamDuration`；
- 客户端断开立即 cancel。

streaming permit 同时计入 App 和 Host connection/inflight 预算，worker 退出、cancel 或
response end 时必须恰好归还一次。普通 chunked/大响应仍受 credit、idle timeout 和普通
inflight 约束，不因为没有 `Content-Length` 就自动占用 SSE permit。

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
| App 已 retired | 404 |
| active generation 已 quarantined | 503 |
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

### 10.5 worker 崩溃替换与 generation quarantine

active worker 出现意外 EXIT、cgroup OOM、同步 CPU timeout、IPC/protocol failure，或被
7.4 的持续健康检查摘除时，固定执行：

1. owner shard 立即从调度集合移除 worker，并把 handle 交给 reaper executor；
2. 先把本次事件计入 generation 的滚动 instability budget，再决定任何 retry 或
   replacement：60 秒内最多 5 次；意外退出、主动健康连续失败导致的 recycle，以及
   replacement spawn/load/READY 失败都计数，正常 drain、Host shutdown 和运维 retire
   不计数；
3. 若本次计数使 budget 超限，立即跳到下述 `QUARANTINING` 流程，不执行后续 retry 或
   replacement；只有 generation 仍为 active 时，失败 inflight 才可按 10.4 决定是否重试；
4. pool 低于目标 READY 数时创建 replacement singleflight；每个 App 同时最多一个替换
   spawn，且必须重新取得全局 startup/memory permit；
5. active replacement 使用指数退避：250 ms 起步、每次翻倍、最大 30 秒，并加 ±20%
   jitter；成功 worker 连续稳定 60 秒后重置该 App/generation 的 backoff；
6. 全局 startup permit 使用按 App 公平的队列，replacement 与 deploy 分 lane 计数；一个
   crash-loop App 不能持续排在其他 App deploy 前面。

M0.7–M0.9 的纯控制器进一步冻结以下边界，避免实现时再选择语义：

- fake clock 使用单调毫秒；时间倒退 fail closed。滚动窗口是
  `(now - window, now]`，年龄恰好等于 window 的事件已经过期；`maxEvents: 5` 表示第
  6 个仍在窗口内的计数事件触发 quarantine；
- backoff attempt 只在实际安排新 replacement 时递增；已有 per-App replacement
  singleflight、无需补 worker 或 generation 已非 active 时不递增；`maximum` 先约束指数
  base，再施加有界 signed jitter，因此 `30s + 20%` 的最终上界是 36s；
- replacement worker 连续 READY 达稳定期只重置 backoff attempt，不清空滚动 crash
  budget；
- instability controller 的单个结果先计数并决定 `BEGIN_QUARANTINE`，再交给 request retry
  决策；quarantine 结果类型本身禁止 retry 与 replacement，不能依赖调用方记住额外顺序；
- startup permit 在 App 之间避免连续授予同一 App（只要另一个 App 正在等待），选中 App
  内保持 FIFO；deploy/replacement 共用 permit 但分别计数，不设置隐含 lane 优先级；相同
  App/generation 的 replacement 排队请求加入 singleflight，不新增队列项。

budget 超限时：

- 内存 Registry 先进入 `QUARANTINING` 并停止新流量；从该状态生效起明确禁用 10.4
  自动重试，尚未取得 response head 的 inflight 一律 cancel 并合成 `503`，不能重新指派
  给残余 READY/draining worker，也不能为它启动 replacement；已经发出 response head 的
  inflight 只能有界 drain，超时后 cancel/断开；
- control plane 用 6.5 的原子协议把 `active.json` 写成 `state: "quarantined"`，包含
  Version、generation 和稳定 reason code `CRASH_BUDGET_EXCEEDED`；
- 停止全部自动 replacement，数据请求返回 `503`，并产生不可丢弃的高优先级事件；
- Host 重启只恢复 quarantine，不因计数器丢失而重新进入 crash loop；
- 运维可显式 deploy 旧 Version，或显式重新 deploy 同一不可变 Version 来清除 quarantine
  并开始全新预算；该动作经过完整预热，不是隐式 resume。

v1 不默认自动回滚。旧 generation 在 drain 完成后可能已经销毁，应用也可能对外部系统
产生与旧代码不兼容的状态；未经 App opt-in 自动切回不是普遍安全操作。以后若增加
activation-guard rollback，必须单独定义旧池保留窗口、双池容量和外部状态兼容契约。

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

v1 的 memory permit 按每 worker `memoryMax` 全额记账。这会比实测约 6 MiB 的 READY PSS
保守很多，但它保证承诺总量不会依赖历史平均值，也不会因 workload 改变突然超卖。
M4 可评估“硬上限承诺 + measured working-set/PSS 软预算”的两级 admission：必须保留
cgroup `memoryMax` 和 Host hard ceiling，使用按 workload/profile 更新的高分位 working
set、增长余量和 OOM 负控；不能直接用一次 benchmark 的平均 PSS 替代硬记账。

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

- worker：starting/ready/busy/unhealthy/draining/crash/replacement；
- recovery：instability budget、backoff、replacement permit、quarantine 和 retire；
- request：inflight/queued/rejected/cancel/timeout/retry；
- latency：queue、startup、worker、time-to-head、total；
- stream：request/response credit、未确认 bytes、SSE permit、slow-client cancel；
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

`CRASH_BUDGET_EXCEEDED`、active state 进入 quarantine/retired、retire drain 超时和管理
授权失败属于不可丢弃的 control-plane 事件；日志不得包含未清洗 URL、forwarded header
或 health response body。

## 13. Runtime 集成要求

### 13.1 已实现：结构化 build/compatibility identity

可信字节码使用只读 build info，至少包含：

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

library 侧 `capsid_runtime_build_info()`、compiler identity target 与 attestation verifier
已经由 M0.2 冻结。实际 worker HELLO/READY 还必须返回同一 identity；Host 必须比较
library、compiler attestation 和 worker 三者，不能只信链接到 Host 的 library。

v1 build info 是 ABI v7 的追加接口，不改动已有结构体布局。它固定公开 runtime/ABI/
FetchRPC 版本、QuickJS commit、txiki overlay key/manifest、bytecode 相关编译 flags、目标
architecture、endianness、pointer width、bytecode format identity、capability manifest
hash 和最终 compatibility ID。最终 ID 是 `sha256:` 加小写十六进制；hash 输入为公共头
文件注释中固定字段顺序的 `key=value\n` UTF-8 record，包含末尾换行，不使用 JSON
canonicalization 或 locale formatting。

其中 `quickjsCommit` 必须是 `vendor/txiki.js/deps/quickjs` 的锁定 gitlink commit，不是
外层 `vendor/txiki.js` commit；外层 vendor、全部 submodule、补丁和 overlay 内容由
`txikiOverlayKey`/`txikiOverlayManifest` 覆盖。配置 worker 构建时必须把锁定 QuickJS
gitlink 与实际 checkout 再比较，避免字段名与真实输入不一致。

实际 worker 的 READY payload 必须携带同一 ASCII compatibility ID；正式
`capsid-bytecode-compile --print-compatibility-id` 只输出该 ID 和换行。三者任一不同即禁止
trusted bytecode。真实 source → bytecode → worker round-trip 仍由 M1 的集成测试完成。

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

现有 CMake option：

```text
CAPSID_BUILD_HOST=ON|OFF
```

当前与计划 targets：

```text
capsid_host_core      C++20 internal library
capsid-host-tests     unit/integration targets
capsid-host           executable（后续数据面切片）
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

版本不应写死在架构契约中。最终 manifest 固定实际审查过的 patch release；文档只记录
选择条件，不把某次开发环境的依赖版本变成永久契约。

### 14.1 公网 C++ Host 的残余风险

第一方 Host 使用 C++20 解析攻击者可控 HTTP，是本方案最大的单进程内存安全残余风险。
Beast 减少自写 parser 面，但不能把该风险归零。若 data listener 被远程利用，攻击者将
获得 Host 服务账号可读的 App/state/secret 面、内存 Registry 和进程内全局部署权限；
没有 privileged supervisor 意味着这不直接等于 root，但已经等于该 Host 安全边界失守。

v1 固定以下缓解，不把它们描述成形式证明：

- Beast 是唯一 HTTP framing authority；Host 只在解析结果上做语义 gate，不再实现第二
  套 Content-Length/chunked parser，避免两个 parser 对边界产生分歧；
- 配置规范化、path/authority、CIDR、attestation 签名消息和 header 清洗写成无副作用
  纯函数，配 table/property test 与独立 fuzz target；
- HTTP parser/serializer、URL 重写和生命周期状态机持续跑 ASan/UBSan，owner-shard 与
  handoff 跑 TSan，smuggling corpus 和随机分片进入 release gate；
- Host 使用专用非 root 账号、最小文件权限和独立管理 socket；不同信任域使用不同 Host
  进程，限制一次利用的横向范围。

如果持续 fuzz 仍暴露不可接受的 parser/lifetime 缺陷，保留把公网 HTTP frontend 拆成
更低权限 transport 进程的选项，通过有界、版本化 IPC 连接 control/worker Host。仅把
配置 parser 移进 helper 不能隔离公网 HTTP exploit，不作为该风险的主要缓解。

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
- 三种路由模式的 URL rewrite golden，以及所有 Forwarded/X-Forwarded 头剥离 property；
- pool 选择、queue、permit 和错误映射；
- fake clock 下的 crash rolling window、指数退避/jitter 边界、per-App replacement
  singleflight 和 startup fairness；
- SSE permit 获取/归还恰好一次，stream cap 始终为普通请求保留槽位；
- deploy/worker 状态机所有非法转移；
- active/retired/quarantined 三种状态的原子恢复。

### 15.3 HTTP 和流控集成

- chunked、Content-Length、TE/CL 冲突和 smuggling corpus；
- header 数量/字节、慢头、慢 body 和 early body；
- 客户端伪造 `Forwarded`/`X-Forwarded-*` 不影响 URL 或 worker headers，proxy 后的
  `publicScheme=https` 生成稳定绝对 URL；
- Admin socket 非授权 peer 被所有端点拒绝；获准 UID/group 对所有 App 具有相同全局
  deploy/retire 权限，不误测成 per-App ACL；
- `Expect: 100-continue` 的接受与拒绝；
- 大 request body 在 credit=0 时不继续读取；
- response credit 只在 client write completion 后归还；
- 慢客户端、SSE、stream permit 满时在 head 前返回 503、断开、cancel 和迟到事件；
- 多 request ID 交错；
- worker crash 在 response head 前后不同语义；
- active health 连续失败摘除、replacement backoff、budget quarantine，以及 crash-loop
  App 不阻塞另一 App deploy；
- 达到 crash budget 的那个事件先切入 `QUARANTINING`，不重试到残余 READY worker，也不
  启动 replacement；
- worker 持续 busy 时主动探针可 skip，但 request/stream deadline 或 IPC/EXIT 被动失败仍
  会摘除并计入 budget；
- shutdown/drain/terminate 不阻塞 reactor。

### 15.4 部署故障注入

在每个步骤后强制 kill Host 并重启：

- source copy 中途；
- generation fsync 前后；
- COMPLETE 前后；
- pool READY 前后；
- active temp write、fsync、rename 和 parent fsync 前后；
- Registry publish 前后；
- retire tombstone rename 和 Registry remove 前后；
- quarantined App 执行 retire 的 tombstone rename 和幂等恢复；
- quarantine state rename 和 replacement 停止前后；
- 旧 pool drain 中。

验收不变量：重启后只可能得到旧 active、完整的新 active、retired 或 quarantined，绝不
能指向半个 generation，也不能复活 retired/quarantined pool。

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

#### 当前证据边界

当前 tree 没有 benchmark runner、原始 A/B 或可核验的 gateway/worker 分层 profile，
因此本设计不保存历史 QPS，也不为 C++ Host 预设提升百分比。M1 必须先恢复 runner，
重新生成 Go baseline 与双侧 profile；任何容量推演只能放在带输入参数的运行报告里，
不能成为产品承诺。通用规则见[性能证据规则](performance-benchmarks.md)。

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

#### M1-perf：最小单 worker Host A/B 检查点

不等完整部署、蓝绿、静态池，也不等 request body、streaming、cancel 和
timeout 做完。M1 的单 worker path listener、GET/HEAD 无 body、URL/header 清洗、
response credit、keep-alive 和内容正确性闭环通过后，立即执行第一轮：

```text
同一 loadgen ─┬─ Go capsid-http-gw ─┬─ 同一 capsid-worker
              └─ C++ capsid-host ────┘
```

第一轮只测已经实现的公共交集，不用未实现能力污染数据：

- 一个预先 READY 的 worker，固定 `workerCount=1`，不测 cold start、deploy 或 autoscaling；
- 一个 path route，先测 GET/HEAD、无 request body、固定 1 KiB response，再增加一个真实
  CPU/模板 workload；
- baseline/candidate 使用完全相同的 bundle digest、Runtime/worker binary、connection、
  inflight、CPU set、cgroup、warmup、测量时长和到达模型；
- 至少 3 轮 headline run，另做同条件 diagnostic run；headline 关闭 profile 探针；
- 同时保存 Host/gateway 与 worker 分组 CPU、CPU/response、QPS、完成率、p50/p95/p99、
  schedule lag、RSS/PSS、context switch、queue wait、time-to-head 和 IPC bytes/syscalls；
- Go 保存 pprof，C++ Host 保存 `perf record`/等价 profile；缺任一侧 profile 或原始 A/B
  输出时，报告只能标记 `INCOMPLETE_EVIDENCE`，不能形成性能结论。

第一个报告用于建立可重复 baseline，不预设胜负阈值。只有至少两次独立重复得到稳定
离散度后，才为后续 Host PR 冻结 regression threshold。若 C++ Host 变慢，先按 profile
定位，不通过放宽 workload、减少校验或关闭 credit 来制造胜出。

完成 request body 双向 credit、streaming、disconnect cancel 和 timeout 后，使用
同一 runner 增加第二个数据面检查点。两个检查点的 workload 不混合；早期
GET/HEAD baseline 保留为回归线，不被后续更复杂的场景覆盖。

当前 tree 没有 `bench/`，因此 M1-perf 的第一条测试固定为
`host_single_worker_ab_emits_complete_evidence`：先用 fake baseline/candidate/loadgen 验证
runner 能强制同条件、三轮原始结果和双侧 profile，再接真实进程。恢复 runner 时不得从
文档中的历史汇总反推或生成原始数据。

### 15.8 发布门

- Release/LTO、ASan、UBSan、TSan 和 fuzz 全绿；
- Host HTTP/部署/故障注入矩阵全绿；
- crash-loop quarantine、retire tombstone、持续健康和 SSE permit 矩阵全绿；
- delegated cgroup 与 Runtime egress policy 的正向证据；
- 配置 schema、示例、Policy Compiler 和 Runtime descriptor golden 一致；
- SBOM、依赖 hash、worker/library/Host/build identity 固定；
- A/B 报告含原始数据并可从当前 tree 追溯；
- 升级旧版本 Host 时，active state 和 App Version 可恢复；
- 运维文档覆盖 backup、rollback、drain、磁盘满、外部网络边界和 cgroup 委派故障；
- threat model 明确公网 C++ Host 与全局 Admin socket 的残余权限边界。

## 16. 实施顺序

以下都是 v1 内部切片，不是把测试、可信字节码或 secret 推迟到 v2。每个切片先落一个
可观察失败的测试，再写最小实现。

### M0：可执行契约

1. `host_config_rejects_network_namespace_field` 先失败；修订 Host/App schema，补 listener、
   capacity、queue 和 trusted bytecode keys，并拒绝 netns 配置字段；
2. M0.2 合并执行：`runtime_worker_compiler_identity_matches` 与
   `bytecode_attestation_rejects_one_bit_tamper` 同时先失败；一次增加 library/worker/compiler
   三方 compatibility identity、attestation 签名消息和 Ed25519 verifier；
3. `secret_value_never_appears_in_effective_config` 与 rotation/generation golden 同时先失败；
   一次冻结 env schema、Host/App 权限交集、owning snapshot、Runtime descriptor view、
   canonical redacted metadata、opaque revision 和 generation digest；safe-read 的真实
   `openat2`/文件类型/并发修改实现仍由 M1 的文件系统负控驱动；
4. `active_recovery_never_selects_incomplete_generation` 先失败；冻结 `active.json`、fsync、
   crash recovery 与 fake filesystem 接口；
5. `request_url_ignores_all_forwarded_headers` 先失败；冻结 public scheme、authority、URL
   rewrite 和 proxy header 规则；
6. `retired_or_quarantined_app_never_reactivates_on_restart` 先失败；冻结 retire 与 crash
   state machine；
7. `crash_loop_does_not_starve_other_app_deploy` 先失败；冻结 replacement backoff、budget
   和 permit fairness；
8. `quarantining_never_retries_to_a_remaining_worker` 先失败；冻结 budget 判定先于 retry 的
   顺序；
9. 建立 Host test target、fake worker、fake clock、sanitizer job 和依赖锁。

完成条件：所有 v1 公共契约都有 golden 和负控，compiler 成为正式 target；生产 Host 代码
仍可很少，但不能存在未被测试表达的安全分支。

### M1：artifact、secret 与单 worker 闭环

M1 合并成四个逻辑门，但 M1A + M1B 作为同一实施批次交付，避免为 runner
和单 worker helper 反复往返。顺序强制在完整数据面和部署面前先建立 Linux
性能 baseline，但不把可信字节码、secret 或 admin 推迟到后续版本。Windows 实现
不进入 M1 发布门；M1A 仅保留平台 adapter 边界，避免新 Host 代码进一步锁死
POSIX：

1. **M1A：benchmark-minimal 单 worker 数据面。** 一个仅用于 M1/benchmark 的明确
   single-worker 启动模式直接加载本地 source bundle；实现 Boost.Asio/Beast HTTP/1、
   keep-alive、单 worker 多 request ID、单一 path listener、M0 URL/header 规范化、
   GET/HEAD 无 request body 的 begin/end、response head/body/end、response credit、内容
   correctness gate 和有界 reaper。携带 request body 或其他 method 的请求在进入
   worker 前返回固定错误，不做隐式 buffering 或部分支持。同时建立 POSIX
   `WorkerEventSource` adapter，并用 source audit 禁止其他
   Host 模块直接调用 `capsid_worker_fd()`。该模式不是部署 API，不写
   `active.json`，也不能被文档描述为生产发布路径。
2. **M1B：性能证据。** `host_single_worker_ab_emits_complete_evidence` 先失败；一次恢复
   最小 Go baseline/loadgen、三轮交错 A/B runner、correctness gate、原始 sample、manifest
   hash 与 Go/C++/worker profile。Runner 可与 M1A 并行实现，但只在 M1A correctness
   gate 绿后启动真实进程；绿后立即运行 15.7 的首轮 baseline，不等待
   M1C/M1D。
3. **M1C：单 worker 数据面完整性。** 在保留首轮 GET/HEAD baseline 的前提下，
   实现 request body 读取、request/response 双向 credit、streaming、disconnect cancel、
   Host + Runtime request timeout、慢客户端背压和有界 shutdown。该批次同时新增独立
   `CAPSID_ENABLE_TSAN` 构建和 Host 并发回归；它不阻塞 M1B 首轮 Release benchmark，
   但 M1C 不得在 TSan 未通过时验收。完成后使用同一 runner 记录第二个数据面检查点，
   不覆盖首轮样本。
4. **M1D：安全部署闭环。** 一次合并 compiler round-trip、artifact safe-read、验签/摘要/
   sourceName/compatibility 选择、secret symlink/FIFO/NUL/越权负控、Policy Compiler、
   `capsid_env_entry[]` 快照和 Unix admin deploy；覆盖源码、可信字节码、兼容失配回退
   源码、secret 进入 worker 四条路径，并移除任何把 single-worker fixture mode 当作部署
   接口的依赖。验收顺序固定为：基础 compiler/read/provider/policy 契约 → managed 真实
   worker deploy/retire/recover → Unix Admin API → 跨平台与 sanitizer 门 → 零探针性能回归。
   Admin API 不得先于 coordinator 的真实 worker 闭环冻结，因为它不负责补全或重新解释
   部署状态机。任何 deploy 路径只有在可信输入已验证、generation durable commit、真实
   worker READY 且 canonical `active.json` 成功发布后才能返回 Active；此前失败必须保留旧
   active generation。

M1A 冻结的 executable 测试入口为：

```text
capsid-host
  --mode single-worker
  --worker <capsid-worker>
  --source-bundle <absolute-path>
  --source-name <absolute-file-URL>
  --application <AppId>
  --listen 127.0.0.1:0
  --routing path
  --public-scheme http
  --public-authority <authority>
  --initial-stream-window <positive-integer>
  --strict-sandbox on|off
  --ready-fd <inherited-fd>
```

`strict-sandbox off` 只允许显式 test/benchmark/native-dev build，生产 Release
必须拒绝。Host 只有在
listener 已绑定且 worker 已验证 READY 后，才向 `ready-fd` 写一行 canonical JSON：
`{"schema":"capsid-host-ready-v1","app":"...","address":"127.0.0.1","port":N}`。
stdout 不承担 readiness 协议。进程收到 SIGTERM 后停止 accept、cancel 未完成请求，并把
阻塞 destroy 交给 reaper 后有界退出。

上述 CLI 是 M1A 的 POSIX 首条路径。Host 业务层不得因此直接调用
POSIX signal 或 fd API；未来 Windows 实现的 out-of-band readiness 与
shutdown/terminate/reap 语义，在具备真实 Windows 机器/hosted runner 后由 RED
测试冻结，不在 M1 中无证据预设 HANDLE/named-pipe/event ownership。

M1C 在同一 CLI 上增加 `--request-timeout-ms <positive-integer>` 并开放已冻结的
request-body/streaming 语义。M1A 不接受该选项，避免未实现契约出现在早期
benchmark executable 表面上。

完成条件：单 worker 端到端测试证明 bytecode 与 secret 的 v1 契约；任一签名/摘要错误
fail closed，secret canary 不出现在 Host 输出；M1-perf 证据可从当前 commit
重放，首轮数据只建立 baseline，不宣传未被 profile + A/B 同时支持的性能提升；
TSan 可以晚于该首轮 baseline，但必须早于 M1C 验收和 M2 多 worker 实施。

### M2：静态池和可靠部署闭环

前置门：独立 TSan 构建已经覆盖 M1 的 HTTP 事件循环、worker 线程、command/event
handoff、disconnect/cancel、timeout 和有界 shutdown；任何第一方代码 data race 都是
阻断错误。TSan 不与 ASan/UBSan 共用构建，不用于性能测量，第三方 suppression 必须精确、
有注释且不能覆盖第一方符号。

1. 从 pool/queue 状态机失败测试开始，实现固定 `minReady == maxWorkers`、shard owner、
   admission、慢客户端和 SSE permit；
2. 从每个持久化边界的 crash test 开始，实现 stage → prewarm → health → active rename
   → drain；
3. 从 rotation test 开始，实现 secret revision 变化生成新 pool；
4. 从 bytecode key rotation/restart test 开始，实现 provenance 随 generation 固化；
5. 从 crash-loop/fake-clock 测试开始，实现 replacement、backoff、instability budget、
   quarantine 和跨 App startup fairness；
6. 从 active health 与 retire crash matrix 开始，实现持续探针、retired tombstone 和有界
   drain；
7. 增加结构化日志、固定指标和明确回退原因。

完成条件：失败永远保留旧版本，重启只恢复完整 generation，请求全程有界；字节码和
secret 在蓝绿、回滚和重启中保持相同语义；crash-loop 不能垄断 permit，retired/
quarantined App 不能因重启复活。

### M3：生产 v1 发布门

1. subdomain 与 trusted-header listener；
2. delegated cgroup hierarchy；验证 Host 不包含 netns 配置、supervisor 或网络管理代码；
3. Host/global/App 完整 admission control；
4. 幂等操作查询、显式 rollback、generation retention/GC；
5. 结构化 Runtime 错误；
6. 全量安全、fuzz、sanitizer、soak、性能 A/B 和 crash matrix；
7. systemd unit、外部网络边界、Admin trust boundary、权限、key/secret rotation、retire、
   quarantine、升级和运维文档。

完成条件：在目标 Linux 环境完成 strict isolation、可信字节码与 secret 的正向和负向
证明，并通过生产流量/发布故障门。至此才称为 v1。

### M4：数据驱动的后续能力

- `minReady < maxWorkers` 的有界扩缩容；
- endpoint/application circuit breaker、显式 opt-in activation-guard rollback；
- 基于 profile/PSS 高分位且保留 hard ceiling 的两级 memory admission；
- 安全 ceiling reload；
- 可选 OTLP adapter；
- HTTP/2、内置 TLS 或第三方 transport adapter；
- 只有 profile 证明后才考虑 io_uring/zero-copy；
- 在可用的真实 Windows 机器或 hosted runner 上启动 Windows native-dev 独立轨道：
  MSVC/CMake、process/transport/reap、加法 event-source ABI、loopback-only Host、本地
  Admin identity/ACL，以及 source/bytecode/env/request/stream/cancel/crash 真实集成；
- Windows 原生生产隔离：先冻结独立 threat model 和 semantic feature bits，再评估
  Job Object、Restricted Token、AppContainer 与部署网络边界；不复用 Linux
  seccomp/Landlock bit 表达不同保证。

## 17. 已确认的决定

1. **源码 + 可信字节码都进 v1**：字节码必须通过签名 provenance、摘要、sourceName
   和 compatibility identity 校验；源码始终保留用于兼容回退；
2. **secret 通过 `capsid:env` 进入 worker**：按权限交集生成不可变快照，轮换生成新
   generation，任何 Host 输出不含明文；
3. **C++20 + Asio/Beast**：保留 C++ owner-shard，但不手写 epoll/HTTP parser；
4. **保持简单**：`active.json` 是单写原子状态文件，active 形态仍只是 generation 指针，
   retire/quarantine 使用同文件 tombstone，不引入 SQLite；静态池先行，autoscaling 后置；
5. **不做 Host netns supervisor**：Host/App 没有 netns 配置；worker 自然使用 Host 的
   网络环境，额外网络隔离完全属于可选的部署环境措施；
6. **crash-loop fail closed**：v1 自动替换、退避并执行 generation budget；超限后持久化
   quarantine 和 503，不默认自动回滚；
7. **显式 retire**：`POST /v1/apps/{app}/retire` 原子写 tombstone、停止路由并 drain，
   不以目录删除表达下线；
8. **URL 不猜代理语义**：listener 显式 public scheme/authority，v1 删除且不信任全部
   Forwarded/X-Forwarded header；
9. **最小持续健康与 streaming 隔离**：配置健康路径时周期抽样，失败纳入 instability
   budget；SSE 使用独立 per-worker permit，保留普通请求槽位；
10. **Windows 原生开发保留但延后**：没有真实 Windows 机器/hosted runner 时不实现、
    不以交叉编译或 WSL2 伪造通过；M1 只建立 Host `WorkerEventSource` adapter 边界，
    防止 POSIX 依赖扩散到数据面。

## 18. 外部选型依据

- [Boost.Beast HTTP 文档](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_http.html)：
  HTTP/1 增量解析、序列化和 buffer-oriented 接口；
- [Boost.Asio POSIX stream descriptor](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview/posix/stream_descriptor.html)：
  接管现有 POSIX fd，并执行异步 read/write/wait；
- [Jansson 解码 API](https://jansson.readthedocs.io/en/latest/apiref.html)：
  `JSON_REJECT_DUPLICATES` 可直接拒绝安全配置中的重复 key；
- [OpenSSL release strategy](https://www.openssl-library.org/policies/releasestrat/)：
  选择仍在上游支持期内并锁定 patch release 的系列；
- [systemd cgroup delegation](https://systemd.io/CGROUP_DELEGATION/)：
  非 root service 管理受委派 cgroup subtree 的边界。

## 19. 最终建议

v1 按一个可证明的垂直闭环交付：

> 源码目录安全快照，类型化权限编译，固定池预热，原子 App 状态切换，旧池有界排空，
> crash-loop 退避并持久 quarantine，显式 retire，稳定的 worker URL 与代理头契约，持续
> 健康和 SSE 独立容量保护；可信字节码经完整信任链进入 worker，secret 经最小权限
> `capsid:env` 快照进入 worker，请求和响应始终受 credit、queue、deadline 与 Linux
> isolation 共同约束。

按 M0 到 M3 的 TDD 切片逐步把这个闭环做小、做严；v1 完成后，再用真实 profile 决定
扩缩容、HTTP/2 和更复杂控制面。这样保持产品简洁性，也与 Runtime 的真实能力边界
一致。
