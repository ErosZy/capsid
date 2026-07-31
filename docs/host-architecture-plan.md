# Capsid Host 架构规划

> 状态：拟议设计，尚未实现。本文用于约束后续 Host 的产品与架构方向，不代表
> 当前 `libcapsid_runtime` 已经提供 HTTP listener、应用发现、发布管理或
> worker pool。当前可用接口与集成要求仍以
> [宿主嵌入接口](embedding-api.md)和
> [第三方宿主集成规范](host-integration.md)为准。

## 目标与核心决定

Capsid Host 应交付为第一方宿主进程，而不是只提供一段 HTTP 到 FetchRPC 的示例。
它负责 listener、应用发现、发布、权限策略编译、worker pool、调度、背压、过载
保护和可观测性。Runtime 继续只负责单个隔离 worker 的执行与 IPC。

Host 的核心调度对象是不可变 Release，而不是 bundle 文件或可以反复换代码的
worker：

```text
HTTP 请求
  → RouteKey
  → Application
  → Active Release
  → Release Worker Pool
  → READY Worker
```

约定可以用于发现应用和减少重复配置，但任何会扩大权限、改变租户边界或影响
资源隔离的行为都必须显式配置。请求数据只能选择已经登记的 Application，不能
直接指定文件、发布摘要、权限或具体 worker。

建议最终提供：

- `capsid-host`：可以独立部署的数据面与控制面进程；
- `libcapsid_host_core`：供第一方 executable 和未来 transport adapter 复用的
  内部组件库，早期不承诺稳定公共 ABI；
- `capsid host validate|plan|reload|drain`：验证、解释和运维命令；
- 独立 Unix 管理 socket，管理接口不复用公网 listener。

第一阶段优先支持 loopback/Unix socket 和 HTTP/1.1。生产 TLS、HTTP/2 和 HTTP/3
可以先由 nginx、Caddy 或 Envoy 终止，避免 Host 在发布、调度和隔离尚未稳定前
同时维护完整边缘协议栈。

## 产物与应用目录

默认应用根目录为 `/srv/capsid/apps`，目录名就是规范化后的 Application ID：

```text
/srv/capsid/apps/
├── orders/
│   ├── capsid.json
│   ├── current.json
│   └── releases/
│       ├── 8f3a9c.../
│       │   ├── bundle.mjs
│       │   ├── bundle.qjsb
│       │   └── release.json
│       └── 729abe.../
│           ├── bundle.mjs
│           └── release.json
└── catalog/
    ├── capsid.json
    ├── current.json
    └── releases/
        └── 19c0d2.../
            ├── bundle.mjs
            └── release.json
```

各文件职责如下：

- `capsid.json`：应用请求的权限、路由例外、资源限制和 pool 策略；
- `current.json`：当前发布摘要，通过写临时文件再原子 rename 的方式更新；
- `release.json`：bundle 摘要、source name、可信字节码兼容标识和构建元数据；
- `releases/<digest>/`：发布后不可原地修改的产物目录。

`current.json` 示例：

```json
{
  "release": "8f3a9c..."
}
```

`release.json` 示例：

```json
{
  "apiVersion": "capsid/release-v1",
  "bundle": {
    "source": "bundle.mjs",
    "sha256": "8f3a9c...",
    "sourceName": "capsid://orders/8f3a9c/bundle.mjs"
  },
  "bytecode": {
    "file": "bundle.qjsb",
    "sha256": "4192ac...",
    "compatibilityId": "capsid-qjs-abc123-linux-x86_64"
  }
}
```

bundle 是现有 Runtime 接受的自包含 ESM：

```js
export default {
  async fetch(request) {
    const url = new URL(request.url);

    return Response.json({
      app: "orders",
      path: url.pathname,
    });
  },
};
```

Host 在启动、目录重扫或显式 reload 时建立 App Registry。高优先级应用可以立即
读取产物并预热；允许 scale-to-zero 的应用可以只验证 release metadata，在首次
启动前延迟读取已登记的产物。两种方式都不能在请求处理中把客户端输入拼成文件
路径。

Application ID 只允许：

```text
[a-z0-9][a-z0-9._-]{0,62}
```

Host 应以固定 apps root 的目录 fd 为起点，使用等价于
`openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS)` 的方式读取
配置和产物，并在进入 Staging 前验证文件类型、大小与 SHA-256。

## App Registry 与执行身份

内存 Registry 保存已经验证的映射，不保存由请求临时计算出的文件名：

```text
Registry["orders"]
  ├── application: orders
  ├── active release: 8f3a9c
  ├── artifact descriptor
  ├── effective policy digest
  ├── sandbox/resource profile
  └── worker pool
```

实际执行池的身份至少包含：

```text
tenant
+ application
+ release digest
+ effective permission digest
+ sandbox/resource profile digest
+ Runtime/QuickJS compatibility ID
```

相同 bundle bytes 可以在 Host artifact cache 中按摘要去重，但默认不能跨应用或
租户共享 worker。即使 bundle 摘要相同，只要权限、sandbox 或资源限制不同，就
必须进入不同的 pool。

## Listener 与应用选择

一个 listener 只使用一种主要路由模式，避免 Host、Path 和 Header 同时出现时
依靠隐含优先级猜测。拟议配置如下：

```json
{
  "apiVersion": "capsid/host-v1",
  "appsRoot": "/srv/capsid/apps",
  "listeners": [
    {
      "name": "public",
      "listen": "0.0.0.0:8080",
      "routing": {
        "mode": "subdomain",
        "suffix": ".apps.example.com"
      }
    },
    {
      "name": "shared-domain",
      "listen": "127.0.0.1:8081",
      "routing": {
        "mode": "path",
        "prefix": "/@capsid/",
        "stripPrefix": true
      }
    },
    {
      "name": "service-mesh",
      "unix": "/run/capsid/data.sock",
      "routing": {
        "mode": "header",
        "header": "Capsid-App",
        "removeBeforeForward": true
      }
    }
  ]
}
```

推荐使用范围：

- 公网生产入口默认使用 subdomain；
- 单域名部署和开发环境使用固定 path prefix；
- 内部网关和 service mesh 通过受信 listener 使用 Header。

### Subdomain 示例

请求：

```http
GET /api/orders/123 HTTP/1.1
Host: orders.apps.example.com
```

解析过程：

```text
orders.apps.example.com
       │
       └─ 去掉 .apps.example.com
                ↓
             AppKey = orders
                ↓
          Registry["orders"]
                ↓
       Release 8f3a9c worker pool
```

worker 收到的 URL 路径保持不变：

```text
https://orders.apps.example.com/api/orders/123
```

类似地：

```http
GET /products/42 HTTP/1.1
Host: catalog.apps.example.com
```

会映射到 `Registry["catalog"]`，不会进入 orders pool。

### Path prefix 示例

请求：

```http
GET /@capsid/orders/api/orders/123 HTTP/1.1
Host: apps.example.com
```

固定语法为：

```text
/@capsid/{application}/{application-path}
```

解析结果：

```text
AppKey          = orders
ApplicationPath = /api/orders/123
```

`stripPrefix: true` 时，worker 收到：

```text
https://apps.example.com/api/orders/123
```

`stripPrefix: false` 时，worker 收到原始 URL：

```text
https://apps.example.com/@capsid/orders/api/orders/123
```

默认应使用 `stripPrefix: true`，让应用 bundle 不依赖部署挂载位置。需要自己处理
mount path 的框架才显式关闭。固定的 `/@capsid/` 前缀也避免把业务路径第一段误解
为 Application ID。

### Trusted Header 示例

内部网关通过 Unix socket 或受信 listener 发送：

```http
GET /api/orders/123 HTTP/1.1
Host: capsid-host.internal
Capsid-App: orders
```

解析过程：

```text
Capsid-App: orders
        ↓
AppKey = orders
        ↓
Registry["orders"]
        ↓
orders active release pool
```

`Capsid-App` 属于宿主控制信息，默认在构造 FetchRPC request 前删除。worker 只
收到业务请求：

```http
GET /api/orders/123 HTTP/1.1
Host: capsid-host.internal
```

公网 listener 禁止 Header routing。通过 TCP 接入的可信反向代理必须先删除客户端
提供的同名 Header，再写入经过认证的值；Host 还必须用 source allowlist、mTLS 或
独立 listener 验证代理身份。普通请求不能通过 Header 指定 release digest 或具体
worker。

## 从 Application 到 Worker

所有入口最终只产生同一种规范化结果：

```text
Host / Path / Trusted Header
              ↓
           AppKey
              ↓
      AppRegistry[AppKey]
              ↓
        Active Release
              ↓
       Release Worker Pool
              ↓
        Scheduled Worker
```

假设当前 Registry 为：

```text
orders
└── Release R42
    ├── bundle digest: 8f3a9c
    ├── policy digest: 71bc02
    └── pool
        ├── W1 READY, inflight=3
        ├── W2 READY, inflight=1
        ├── W3 READY, inflight=5
        └── W4 READY, inflight=0
```

`GET /@capsid/orders/api/orders/123` 先固定为：

```text
AppKey  = orders
Release = R42
Pool    = orders/R42
```

调度器再从该 pool 选择 worker。默认可以采用 Power of Two Choices：从两个候选
中选择当前负载更低者，而不是每次扫描整个 pool。负载分数至少包含 inflight、
未确认 response bytes、queue wait 和 unhealthy penalty。本例会优先选择 W4。

默认语义是：

```text
相同 AppKey
  → 一定进入相同的 active Release
  → 一定执行相同的 bundle 和 policy
  → 不保证进入相同的具体 worker
```

应用需要 worker 亲和性时显式配置：

```json
{
  "pool": {
    "minReady": 2,
    "maxWorkers": 16,
    "affinity": {
      "source": "cookie",
      "name": "session_id",
      "mode": "bounded"
    }
  }
}
```

Host 使用带私有随机密钥的 Rendezvous Hash，把 `session_id=user-123` 优先映射到
一个 READY worker；preferred worker 过载、drain、崩溃或属于旧 Release 时允许
spill 到其他 worker。Affinity 只是性能提示，不是状态正确性保证。

当前 `capsid:storage` 只存活于单 worker 私有内存。应用启用 storage 且
`maxWorkers > 1` 时，Host 配置验证应明确警告：worker affinity 不能替代外部持久
存储，扩缩容、崩溃和发布切换都会改变映射。

## JSON 权限与配置分层

权限配置分为三层：

1. Runtime restricted build 中实际存在的能力；
2. Host/tenant 运维策略允许的上限；
3. 应用 `capsid.json` 声明的需求。

有效权限为三者交集。应用申请超过运维上限时，发布应失败并报告具体字段，不能
静默删减后继续运行。CLI、URL 和请求 Header 都不能临时扩大权限。

应用配置应隐藏当前 C ABI 中不必要的重复描述：

```json
{
  "apiVersion": "capsid/app-v1",
  "permissions": {
    "modules": [
      "capsid:hashing",
      "capsid:permissions"
    ],
    "env": {
      "APP_MODE": { "value": "production" },
      "API_TOKEN": {
        "valueFrom": "/run/secrets/orders-api-token"
      }
    },
    "fs": {
      "read": {
        "allow": ["/srv/capsid/data/orders"],
        "deny": ["/srv/capsid/data/orders/private"]
      }
    },
    "fetch": {
      "allow": ["https://api.example.com:443"]
    },
    "storage": {
      "namespaces": ["orders-cache"]
    },
    "stdio": ["stdout", "stderr"]
  },
  "pool": {
    "class": "latency",
    "minReady": 1,
    "warmSpare": 1,
    "maxWorkers": 16,
    "idleTtl": "5m"
  }
}
```

Config Compiler 自动展开为 Runtime descriptor：

- `env` 生成 `capsid:env`、ENV rule 和不可变 environment snapshot；
- `fs.read` 生成 `capsid:fs` 和 READ allow/deny rules；
- `storage` 生成 `capsid:storage` 和精确 namespace rules；
- `stdio` 生成 `capsid:stdio` 和 STDIO rules；
- `fetch` 同时生成 direct egress policy 与 capability net policy；
- rule ID 从规范化 JSON pointer 稳定生成，Host 保存 ID 到配置位置的反查表。

所有 `tjs:*`、FFI、raw socket、write、相对/绝对/远程 module import 继续
fail closed。应用 manifest 声明权限只是请求授权，不能绕过运维策略上限。

## Release 与 Worker 生命周期

Release 状态机：

```text
DISCOVERED
  → VALIDATING
  → STAGING
  → WARMING
  → ACTIVE
  → DRAINING
  → RETIRED
```

校验、加载或健康检查失败进入 `FAILED`，记录失败阶段并使用带抖动的指数退避。
配置或产物变化总是创建新 generation，不修改已有 worker。

发布步骤：

1. 读取并验证新 release artifact；
2. 编译有效权限与 sandbox/resource profile；
3. 创建新 generation；
4. 启动至少 `minReady` 个 worker；
5. 验证 READY flags 和可选内部健康请求；
6. 原子切换 Registry snapshot；
7. 旧 generation 停止接收新请求；
8. 等待 inflight 清零，再执行 shutdown/terminate/destroy。

例如：

```text
切换前：orders → R42 → [W1, W2, W3, W4]

预热中：orders → R42 → [W1, W2, W3, W4]
                  R43 → [W5, W6] warming

切换后：orders → R43 → [W5, W6]
                  R42 → [W1, W2, W3, W4] draining
```

worker 状态机：

```text
SPAWNING → LOADING → READY → BUSY
                         └→ IDLE
READY/BUSY → DRAINING → STOPPED
任意状态 → FAILED
```

每个 worker 一生只属于一个 Release，其 bundle、policy 和 sandbox 不原地变更。

## 热池、冷启动与并发启动

Host 使用以下术语：

- Hot：READY 且参与调度；
- Standby：READY，但主要用作预留容量；
- Cold：artifact 已登记或缓存，但当前没有 READY worker。

建议提供三个高层 service class：

- `latency`：默认 `minReady=1` 并保留 spare capacity；
- `elastic`：允许 `minReady=0`，首次请求承担冷启动；
- `batch`：默认不保留 worker，并允许更长 queue deadline。

当前实测单 worker READY 后约 6–7 MiB PSS，真实 Hono bundle 的 READY
median 约 30 ms；parse-heavy 大 bundle 源码冷启动会显著高于可信字节码。因此
交互式服务默认保留一个 READY worker，scale-to-zero 必须显式选择。

Cold Release 的并发请求使用 singleflight：

```text
首个请求 → 创建一次 startup flight
后续请求 → 等待同一个 startup future
```

不能为每个排队请求分别 spawn worker。通用的“未加载 spare worker”也不能在应用
之间复用，因为 policy、sandbox 和资源配置在 spawn 时已经固定。并发启动池实际
是有界的 startup executor，预热 worker pool 则必须属于具体 Release。

启动许可同时受以下约束：

- Host 全局 startup concurrency；
- tenant startup concurrency；
- 单 Release startup concurrency；
- CPU affinity 与 cgroup CPU quota；
- worker memory reservation；
- 请求剩余 deadline。

容量按 slot 而不是只按 worker 数量计算：

```text
desiredWorkers =
  ceil((inflight + queued) / targetInflightPerWorker)
  + warmSpare
```

结果再限制到 `minReady..maxWorkers`。`targetInflightPerWorker` 是 Host 软限制，
Runtime `max_inflight_requests` 是不可突破的硬限制。CPU-heavy 应用从每 worker
一个 inflight 开始；I/O-heavy 应用可以允许更多并发。自适应模式必须基于
queue wait、worker execution latency 和尾延迟缓慢增大、快速收缩，不能把某次
benchmark 的最优 worker 数硬编码成产品默认值。

缩容只处理超过 idle TTL 的 worker，不低于 `minReady`，每个控制周期最多缩减
一个，且必须先 drain。

## Reactor、背压与过载

数据面使用多个 reactor shard。每个 worker 从接管到销毁只属于一个 owner
shard；其他线程通过队列投递命令，不能并发调用同一个 `capsid_worker`。控制面向
各 shard 发布不可变 Registry snapshot。

spawn、bundle load 和等待 READY 可以在 bootstrap executor 中完成；READY 后以
明确的串行 ownership handoff 交给数据面 shard，避免同步启动阻塞 HTTP I/O loop。

Request/response body 沿用 Runtime credit 模型：

- 没有 request credit 时，不继续从客户端读取 body；
- 下游成功消费 response bytes 后才归还 response credit；
- 冷启动排队只保留有界 headers 和元数据，不完整缓存大 request body；
- 客户端断开、Host deadline 或 body 读取失败立即 cancel；
- SSE 和慢客户端不能绕过队列、连接与未确认字节上限。

Admission control 至少分为 global、tenant、application/release 三层，并分别限制
并发请求、排队请求、排队字节和 queue deadline。应用之间使用 weighted fair
queue 或 DRR，避免单一应用占满全局 startup 和请求额度。

建议错误语义：

- 未找到应用：`404`；
- Release 不可用、启动失败或 pool 饱和：`503`，必要时带 `Retry-After`；
- tenant quota：`429`；
- Host deadline：`504`。

只有尚未发送 response head、请求可安全重放且属于 worker/IPC 基础设施故障时
允许重试一次。不能自动重试任意 POST，也不能把应用返回的 HTTP 5xx 当成 worker
故障。

## 可观测性

每个请求至少区分：

```text
route resolve
queue wait
cold-start wait
worker dispatch
time to response head
response duration
total duration
```

每个 application/release/pool 应暴露：

- READY、idle、busy、starting 和 draining worker 数；
- inflight、queued、rejected 与 retry；
- spawn、load、READY 分阶段耗时；
- source/bytecode 选择与回退原因；
- crash、timeout、cancel 和 circuit breaker；
- response credit backlog；
- worker PID RSS/PSS 与 cgroup memory；
- capability audit deny；
- config、release、policy 和 runtime compatibility digest。

现有 QuickJS heap memory metrics 会遍历 heap，只用于显式诊断和回归门，不进入
请求热路径或常规高频采样。

## Runtime 前置补充

Host v1 的大部分功能可以由当前 ABI 实现。可信字节码进入生产发布前，还需要
Runtime/worker 暴露明确的 bytecode compatibility ID，至少覆盖：

```text
Capsid worker build
+ QuickJS commit
+ compile flags
+ architecture
+ bytecode format version
```

`release.json` 中的 compatibility ID 必须与实际 worker HELLO 返回值完全一致；
不一致时回退到已验证源码，缺少源码时拒绝发布。Capsid 版本号或 C ABI version
不能单独证明 QuickJS bytecode 兼容。

## 实施顺序

### 第一阶段：可靠的静态 Host

1. Host/app/release JSON schema 与 Config Compiler；
2. 安全目录读取、artifact hash 与 App Registry；
3. subdomain、path prefix、trusted header 三种 listener；
4. 固定大小的 per-Release worker pool；
5. reactor ownership、credit、cancel 和 drain；
6. `validate`、`plan`、结构化日志和基础 metrics。

### 第二阶段：生产发布与弹性

1. generation staging、prewarm、健康检查和原子切换；
2. cold-start singleflight；
3. startup concurrency 与 memory permits；
4. admission control、fair queue 和 circuit breaker；
5. transactional reload、失败回滚与旧 Release 回收。

### 第三阶段：自适应与多租户

1. adaptive concurrency 和 autoscaling；
2. bounded affinity 与稳定 canary；
3. bytecode compatibility ID、可信编译工具与缓存；
4. tenant policy ceiling 和资源配额；
5. 更完整的 HTTP/2、TLS 或第三方 transport adapter。

Host 的设计原则最终归纳为：

> 路由按约定，权限按声明，发布不可变，worker 按 Release 池化，亲和性只作
> 提示，所有启动、排队和执行都有资源预算与 deadline。
