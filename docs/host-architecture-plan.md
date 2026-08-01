# Capsid Host 架构规划

> 状态：拟议设计，尚未实现。当前可用接口仍以
> [宿主嵌入接口](embedding-api.md)和
> [第三方宿主集成规范](host-integration.md)为准。
>
> 文档定位：本文只保留产品边界和架构概要。v1 的可实现配置 schema、候选默认值、状态
> 机、HTTP 契约、错误语义和验收门以
> [Host 技术评审与 v1 详细方案](host-technical-design-review.md)为唯一规范来源；两者冲突
> 时以后者为准。尤其是详细方案 5.1/5.2 已取代本文第 1/2 节作为配置实现依据。

## 一句话模型

Capsid Host v1 只有三个用户需要理解的东西：

1. 一台机器一份 `host.json`：定义整台 Host 的权限上限、Linux 隔离和资源硬上限；
2. 每个 App 一份 `capsid.json`：只能申请 `host.json` 的子集；
3. 把版本目录推到固定位置，再调用一个部署接口：Host 自动校验、预热、蓝绿切换和
   旧版本排空。

```text
host.json（整机硬上限）
          ∩
capsid.json（App 申请）
          ↓
   有效权限与资源限制
          ↓
版本目录 → 部署接口 → 预热新池 → 原子切换 → 排空旧池
```

v1 不向用户暴露 realm、tenant、binding、policy directory、资源档位或多层交付策略。
这些概念不是完成安全部署和蓝绿发布的必要条件。

## 产品边界

Capsid Host 负责：

- HTTP listener 与应用路由；
- 固定目录中的版本发现；
- Host/App 配置校验与权限编译；
- worker 创建、预热、调度、排空和销毁；
- Linux 隔离、资源限制、背压和过载保护；
- 蓝绿发布、失败保持旧版本和显式回滚；
- 状态、日志、指标和拒绝原因。

Runtime 继续只负责单个隔离 worker 的执行与 FetchRPC IPC。Host 不理解应用业务，
不生成页面，不采集业务行为，也不负责自动决定哪个应用版本更好。

一个 Host 实例就是一个管理和安全边界。如果两组应用互不信任，使用不同 Host
进程、服务账号、应用根目录和状态目录；v1 不在一个进程里重新实现多租户控制面。

## 1. 整机配置 `host.json`

> 本节示例只解释 Host/App 上限模型，不再冻结完整 schema 或默认值；实现和测试必须使用
> [详细方案 5.1](host-technical-design-review.md#51-修订后的-hostjson-轮廓)。

默认位置：

```text
/etc/capsid/host.json
```

它是整台 Capsid Host 唯一的运维配置。应用部署者不能写这个文件。

### 安全默认值

没有 `host.json` 时，Host 使用以下安全约定：

- 应用根目录：`/srv/capsid/apps`；
- Host 状态目录：`/var/lib/capsid`；
- 应用默认不能读取环境、文件和 secret，也不能出站访问网络；
- 生产 strict sandbox 固定启用；
- worker 使用内建的有界默认限制；
- 只在 loopback/Unix 管理入口接受部署操作。

因此自包含 App 可以零 Host 配置运行。需要开放能力时，运维只修改这一份文件。

### 完整示例

```json
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/apps",
  "stateRoot": "/var/lib/capsid",
  "secretRootTemplate": "/run/capsid/secrets/{application}",
  "permissions": {
    "modules": [
      "capsid:permissions",
      "capsid:env",
      "capsid:fs",
      "capsid:storage",
      "capsid:stdio",
      "capsid:hashing"
    ],
    "environmentNames": ["APP_MODE", "API_TOKEN"],
    "fsReadRoots": ["/srv/capsid/data/{application}"],
    "fetchTargets": ["*.internal.example.com:443"],
    "storageNamespaces": ["{application}-*"],
    "stdioStreams": ["stdout", "stderr"],
    "protectedAddresses": "deny-unless-explicit-cidr"
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
  "workerDefaults": {
    "jsHeap": "64MiB",
    "processAddressSpace": "256MiB",
    "memoryMax": "256MiB",
    "cpuQuota": "50%",
    "fileDescriptors": 64,
    "pidsMax": 8,
    "requestTimeout": "5s",
    "maxInflight": 32,
    "minReady": 4,
    "maxWorkers": 4
  },
  "workerMaximums": {
    "jsHeap": "256MiB",
    "processAddressSpace": "1GiB",
    "memoryMax": "1GiB",
    "cpuQuota": "200%",
    "fileDescriptors": 256,
    "pidsMax": 32,
    "requestTimeout": "30s",
    "maxInflight": 128,
    "minReady": 64,
    "maxWorkers": 64
  }
}
```

这里没有 `small`、`medium`、`large`。限制使用明确数值：

- `workerDefaults`：App 没写时采用的值；
- `workerMaximums`：App 最多可以申请到的值；
- App 只能申请更小或相等的值；
- 静态池 v1 的有效配置还必须满足 `minReady == maxWorkers`，不能仅因两个字段分别低于
  maximum 就接受不等值；
- Linux isolation 只由 Host 决定，App 不能覆盖。

上例容量只是静态池候选值，必须按详细方案的 profile 和故障测试门验证后再冻结；一旦
`capsid/host-v1` 冻结，就不能在同一 API version 下静默改变。

如果 Host 没有显式配置 `workerMaximums`，最大值等于相应默认值。这样默认不会因为
某个 App 修改配置而扩大整机资源授权。

## 2. App 配置 `capsid.json`

每个版本目录中放一份 `capsid.json`。App 名来自父目录，版本名来自版本目录，不在
JSON 中重复填写。

### 最小配置

如果入口文件使用约定名 `bundle.mjs`，且 App 不需要外部能力：

```json
{
  "apiVersion": "capsid/app-v1"
}
```

### 申请 Host 能力

```json
{
  "apiVersion": "capsid/app-v1",
  "entry": "bundle.mjs",
  "permissions": {
    "modules": [
      "capsid:hashing",
      "capsid:permissions",
      "capsid:env",
      "capsid:fs",
      "capsid:storage",
      "capsid:stdio"
    ],
    "env": {
      "APP_MODE": {
        "value": "production"
      },
      "API_TOKEN": {
        "valueFrom": "orders-api-token"
      }
    },
    "fs": {
      "read": {
        "allow": ["/srv/capsid/data/orders"],
        "deny": ["/srv/capsid/data/orders/private"]
      }
    },
    "fetch": {
      "allow": ["https://orders-api.internal.example.com:443"]
    },
    "storage": {
      "namespaces": ["orders-cache"]
    },
    "stdio": ["stdout", "stderr"]
  },
  "limits": {
    "jsHeap": "64MiB",
    "memoryMax": "256MiB",
    "requestTimeout": "3s",
    "maxInflight": 16
  },
  "workers": {
    "minReady": 4,
    "maxWorkers": 4
  },
  "healthCheck": {
    "path": "/_capsid/health",
    "timeout": "1s"
  }
}
```

所有字段都是申请：

- `permissions` 必须是 `host.json.permissions` 的子集；
- `limits` 和 `workers` 不能超过 Host 的 `workerMaximums`；
- 省略限制时使用 Host 的 `workerDefaults`；
- App 不能声明或关闭 seccomp、Landlock、namespace、cgroup 根和
  `no_new_privs`；
- 超出 Host 上限时部署整体失败并返回具体差异，不做静默裁剪。

有效权限只有一个公式：

```text
Runtime 实际能力 ∩ host.json 上限 ∩ capsid.json 申请
```

`env.valueFrom` 是 secret key，不是文件路径。Host 只在
`secretRootTemplate` 展开的 App 专属目录中读取，不把文件路径、进程环境或明文持久化
配置交给 worker；但 secret value 会按设计作为不可变 `capsid:env` 快照进入 worker，
并能被获得该 key 权限的应用代码读取。轮换 secret 会创建新 generation，不原地修改
READY worker。

## 3. 版本目录

用户把每个待部署版本放到固定应用目录下：

```text
/srv/capsid/apps/
└── orders/
    ├── 2026-07-31-001/
    │   ├── capsid.json
    │   ├── bundle.mjs
    │   ├── bundle.qjsb
    │   ├── bytecode.json
    │   └── bytecode.sig
    └── 2026-07-31-002/
        ├── capsid.json
        └── bundle.mjs
```

约定如下：

- Application ID：`[a-z0-9][a-z0-9._-]{0,62}`；
- Version ID：`[A-Za-z0-9][A-Za-z0-9._-]{0,127}`；
- 一个 Version ID 对应一个不可变输入目录；
- 已经成功部署的目录不得原地修改，更新必须使用新 Version ID；
- `bundle.mjs` 是默认入口；
- `bundle.qjsb`、`bytecode.json`、`bytecode.sig` 是可选但不可拆分的可信预编译产物组；
- App 和 Version 只能作为规范化 ID，部署接口不能接收任意文件路径。

上传工具不受 Capsid 限制，可以是 `rsync`、`scp`、CI 发布系统、共享卷或容器镜像。
用户只需先完成目录同步，再调用部署接口。

## 4. 唯一部署接口

> 本节只描述产品流程；管理 API、状态机、幂等和 crash-safe retire 的实现契约见
> [详细方案第 7 节](host-technical-design-review.md)。

默认通过独立 Unix 管理 socket 提供：

```http
POST /v1/deploy HTTP/1.1
Content-Type: application/json

{
  "app": "orders",
  "version": "2026-07-31-002"
}
```

这是 v1 唯一激活某个应用 Version 的接口。部署行为固定，不再要求用户选择一组策略：

1. 从固定 `applicationsRoot` 解析 App/Version；
2. 安全打开 `capsid.json`、源码和可选的完整字节码产物组；
3. 计算摘要并复制到 Host 管理的不可变 staging 目录；
4. 校验 App 权限和资源申请没有超过 Host；
5. 编译 Runtime capability、Landlock、egress、cgroup 和 resource limits；
6. 为新版本启动 `minReady` 个 worker；
7. 等待 READY，并执行可选 `healthCheck`；
8. 全部成功后原子切换该 App 的 active version；
9. 旧版本停止接收新请求，完成在途请求后退出；
10. 返回新版本已激活的结果。

```json
{
  "app": "orders",
  "version": "2026-07-31-002",
  "status": "active",
  "previousVersion": "2026-07-31-001",
  "digest": "8f3a9c..."
}
```

固定失败语义：

- 目录不存在或格式无效：拒绝部署；
- 权限或资源越过 Host 上限：拒绝部署并返回字段差异；
- worker 启动、READY 或健康检查失败：拒绝切换；
- 任何失败都保持旧版本继续服务；
- 相同 App/Version 重复请求是幂等操作；
- 回滚就是对旧 Version 再调用同一个接口。

上传目录本身不是运行目录。部署成功后，即使上传目录被误删，当前 worker 仍使用
Host 已复制并校验的内部快照。停止或删除 App 必须使用单独的显式运维动作，不能把
一次文件同步错误直接解释为下线。

显式下线固定为 `POST /v1/apps/{app}/retire`。它不选择另一个 Version，而是原子写入
retired tombstone、停止新路由并有界 drain；完整 crash-safe 语义见详细方案 7.6。

## 5. 蓝绿和 worker 映射

用户只看见 App 和 Version；Host 内部维护每个版本自己的 worker pool：

```text
切换前
orders → V1 → [W1, W2, W3, W4] ACTIVE

预热中
orders → V1 → [W1, W2, W3, W4] ACTIVE
         V2 → [W5, W6, W7, W8] WARMING

切换后
orders → V2 → [W5, W6, W7, W8] ACTIVE
         V1 → [W1, W2, W3, W4] DRAINING
```

每个 worker 一生只属于一个 App Version。它的 bundle、有效权限、Linux sandbox、
secret snapshot 和资源限制在 READY 后不能原地修改。

同一 App 的普通请求都进入 active version 的 pool，再由负载调度器选择 READY
worker。默认不保证相同客户端落到相同 worker；需要外部持久状态的应用不能依赖
worker 私有内存。

相同 App/Version/generation 的并发 deploy 使用 singleflight，只启动一套预热 pool。
数据请求本身永远不触发 spawn。v1 使用 `minReady == maxWorkers` 的固定池，只有全部候选
worker READY 并通过部署健康检查后才激活，不把冷启动延迟暴露给第一个生产请求。

## 6. URL、Header 与 App 的映射

请求只能选择已经登记的 App，不能选择 Version、文件或 worker：

```text
HTTP Host / 固定 Path / 受信 Header
                  ↓
                AppKey
                  ↓
          active version pool
                  ↓
             READY worker
```

支持三种 Host 级路由约定：

```text
orders.apps.example.com            → orders
/@capsid/orders/api/orders/123     → orders
Capsid-App: orders                 → orders（仅受信内部 listener）
```

Path 模式默认从传给 worker 的 URL 中移除 `/@capsid/orders` 前缀。Header 模式只能用于
Unix socket、mTLS 内部入口或严格代理 allowlist；公网客户端提供的同名 Header 必须
删除。请求参数永远不能拼接成磁盘路径。

## 7. Host 内部状态

用户不维护 Host 状态目录。建议结构：

```text
/var/lib/capsid/apps/orders/
├── active.json
└── versions/
    ├── 8f3a9c.../
    │   ├── capsid.json
    │   ├── effective.json
    │   ├── bundle.mjs
    │   ├── bundle.qjsb
    │   ├── bytecode.json
    │   └── bytecode.sig
    └── 729abe.../
        ├── capsid.json
        ├── effective.json
        └── bundle.mjs
```

目录名使用内容摘要。`active.json` 是单写原子 App 状态文件：active 形态保存当前
generation 指针，retired/quarantined 形态保存 tombstone；三者都通过临时文件、同步和
原子 rename 更新。详细格式与恢复不变量见详细方案第 6 节。
内部快照至少固定：

- 源码和可信字节码摘要；
- 规范化后的 `capsid.json`；
- Host 配置摘要；
- 有效 permission、sandbox 和 resource limits；
- 不含 secret 明文的 secret revision；
- Runtime/QuickJS compatibility ID。

Host 配置改变后，已有 READY worker 不原地修改。运维先执行 plan/reload，Host 对当前
App 重新校验和预热，再逐 App 原子切换。

## 8. 文件与字节码安全

> 本节只描述安全目标；可信字节码、generation identity、兼容回退和验收负控见
> [详细方案第 3.5、6.3 和 15.5 节](host-technical-design-review.md)。

Host 以 `applicationsRoot` 的目录 fd 为根，使用等价于
`openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS)` 的方式读取
App/Version。必须拒绝：

- symlink、magic link、device、FIFO 和 socket；
- `..`、绝对 artifact path 和越过版本目录的路径；
- 超出大小上限的配置或 bundle；
- 读取过程中发生 inode、size 或内容变化的输入；
- JSON/schema 未知字段和摘要不一致。

Host 从已经打开的 fd 计算摘要并复制，不在请求处理阶段再次读取上传目录。

QuickJS bytecode 只有完整验证后才是“可信”：构建流水线必须使用与目标发布物同源的
`capsid-bytecode-compile`，并用 Host 配置信任的 Ed25519 key 签名 attestation。Host
验证签名、App、Version、精确 source name、源码/字节码 SHA-256 和 compatibility ID。
ID 至少覆盖 Capsid worker build、QuickJS commit、编译 flags、architecture 和 bytecode
format version。

签名、摘要、App/Version 或 source name 错误表示 provenance 失败，部署直接失败，不能
静默回退；签名及摘要有效但 compatibility ID 不匹配时，回退到同版本 `bundle.mjs` 并
记录原因。源码始终必需。只有完全匹配时才调用 Runtime trusted bytecode API。

## 9. 权限与 Linux 隔离

JavaScript capability 和 Linux 隔离同时生效：

| 边界 | 来源 | 执行机制 |
| --- | --- | --- |
| `capsid:*` module 与操作 | Host 上限 ∩ App 申请 | Runtime capability policy |
| 文件读取 | 有效 `fs.read` | `capsid:fs` + Landlock + `openat2` |
| 出站网络 | 有效 `fetch` | Runtime 双层 egress policy；外部网络隔离可选 |
| syscall | Host isolation | seccomp BPF |
| 提权 | Host isolation | `no_new_privs` + user namespace |
| 内存、CPU、PID、fd | Host 上限 ∩ App 申请 | QuickJS limit + rlimit + cgroup v2 |

所有 `tjs:*`、FFI、raw socket、任意文件写入、相对/绝对/远程 module import 默认继续
fail closed。公共 API 和文档只使用 `capsid:*` 命名。

App 的 `fs.read` 会同时生成 Runtime operation rule 和 Landlock 只读根。App 的
`fetch` 会同时生成 direct egress policy 与 capability net policy；hostname、DNS
结果和 redirect 每次都重新检查。私网、loopback、link-local 和 metadata 地址默认
拒绝，除非 Host 用精确 CIDR 显式开放。

生产 Host 缺少任何 `isolation.required` feature 时启动失败，不能降级运行。Host
进程本身使用专用非 root 账号；cgroup 只能来自 systemd `Delegate=yes` 或等价的预先
委派。Host 不配置或切换 network namespace，worker 自然使用与 Host 相同的网络环境。
若运维需要更强网络隔离，可以把整个 Host 放进 systemd、容器或 Kubernetes 已准备的
网络环境；这不是运行 Capsid Host 的前置要求。

第一方 Host 不实现、也不规划 privileged supervisor、root helper、supervisor socket
或 netns 创建协议，Host/App schema 中也没有 network namespace 配置字段。

## 10. 调度、背压与过载

> 本节只描述调度概要；credit、admission、retry、crash recovery 和 profile gate 见
> [详细方案第 9、10 和 15.7 节](host-technical-design-review.md)。

每个 worker 从 READY 到销毁只属于一个 reactor owner。其他线程通过队列投递命令，
不能同时调用同一 worker。

### 现有 benchmark 与目标实现

当前已发布的完整 Capsid benchmark 使用 Go `capsid-http-gw`：HTTP 层是并发
`net/http` handler，Go runtime 底层使用自己的 netpoll；它没有实现这里规划的
Capsid Host owner-shard reactor。因此现有结果是 Runtime + Go gateway 的基线，不能
声称已经测量了第一方 Host。

第一方 Host 的目标实现是少量 C++ epoll shard：每个客户端连接和 worker IPC fd 在
其生命周期内固定归属一个 shard，readable/writable 事件批量 drain；跨 shard 操作只
通过有界队列和 eventfd 投递。spawn、bundle load 和预热放在独立 bootstrap executor，
READY 后再把 worker ownership 一次性交给数据面。v1 先使用成熟的 epoll，不在没有
profile 证据时引入 io_uring、shared-memory ring 或自定义 zero-copy 协议。

这个方案预期减少 cgo/goroutine 调度、跨线程锁和无效轮询，但“更快”必须由同条件
A/B 证明。验收时使用相同 bundle、worker 数、连接数、inflight 和资源限制，对比 Go
gateway 与第一方 Host 的 QPS、p99、CPU/response、RSS、queue wait、IPC 时间和
open-loop 完成率；没有数据就不写性能结论。

若重新 profile 后确认 Go gateway 约占 20 CPU units、workers 占 60–65 units，则
Gateway 自身成本下降 30%/50%/67% 对应的系统吞吐推演约为 1.08x/1.13–1.14x/
1.19–1.20x。套入已有 495.8/600/762.5 QPS，合理的 8%–15% 假设约为
535–570/648–690/824–875 QPS；20% 只作为优秀结果，不作为承诺。

这些数字必须由 profile 转成证据。每个 Host 数据面里程碑和性能优化 PR 都要提交同环境
before/after benchmark、Gateway/worker 分层 CPU、Go pprof 或 C++ `perf` profile、关键
queue/IPC/credit trace 和原始数据。没有 profile 指向瓶颈，或者只有 profile 没有端到端
A/B，都不能形成性能结论。详细门槛见[技术设计 15.7](host-technical-design-review.md)。

Host 至少限制：

- 全局、单 App 和单 worker 的 inflight；
- 单 App 排队请求数、排队字节和 queue deadline；
- 全局和单 App 启动并发；
- worker memory reservation 与 cgroup hard limit；
- request/response 未确认字节和 stream credit。

请求 body 和响应 body 沿用 Runtime credit 模型。客户端断开、deadline 到期或 body
失败立即 cancel。SSE 和慢客户端不能绕过队列及未确认字节上限。

默认错误语义：

- App 不存在：`404`；
- pool 不可用、预热失败或过载：`503`；
- App queue/quota 超限：`429`；
- Host deadline：`504`。

只有尚未发送 response head、请求可安全重放且属于 worker/IPC 基础设施故障时，才
允许自动重试一次。应用返回的 HTTP 5xx 不是 worker 故障。

## 11. 可观测性

每次部署至少记录：

- App、Version、内容摘要和前一版本；
- 配置校验与越权差异；
- 源码/字节码选择和回退原因；
- worker spawn、load、READY、health check 各阶段耗时；
- 预热数量、切换时间、排空时间和失败原因。

每个 App/Version/pool 至少暴露：

- READY、busy、starting、draining worker 数；
- inflight、queued、rejected、timeout 和 cancel；
- queue wait、cold-start wait、worker latency 和总延迟；
- worker PID RSS/PSS、cgroup memory、CPU 和 crash；
- capability deny 与 Linux sandbox 安装结果；
- config、bundle、effective policy 和 Runtime compatibility digest。

QuickJS heap 遍历只用于显式诊断和回归门，不进入请求热路径或高频采样。

## 12. 实施顺序

所有阶段都使用 TDD：先写一个只因目标行为尚不存在而失败的测试，再写最小实现，测试
保持全绿后才重构。安全能力先写拒绝负控；bug 必须先有可复现的失败测试。不接受功能
先合入、测试后补。

### 第一阶段：可执行契约

1. 从 schema 负控开始冻结 Host/App 配置和容量，并拒绝 network namespace 配置字段；
2. 从三方 identity mismatch 测试开始实现 Runtime/worker/compiler compatibility ID；
3. 从逐字段篡改测试开始实现 bytecode attestation 和 Ed25519 验签；
4. 从 secret 泄漏 golden 开始冻结 `capsid:env` snapshot、redaction 和 rotation；
5. 从 crash recovery 不变量开始冻结 generation digest 和 active/retired/quarantined
   三种 `active.json` 状态；
6. 建立 Host test target、fake worker/clock/filesystem 和 sanitizer job。

### 第二阶段：单 worker 垂直闭环

1. compiler round-trip 与真实 worker 字节码加载；
2. 安全 Version 读取、签名 provenance、摘要、源码回退和 Host 内部快照；
3. secret safe-read、Host/App 权限交集和 `capsid_env_entry[]`；
4. 单一部署接口、一个 worker、path 路由、credit、cancel 和 timeout；
5. 同时验证源码、可信字节码、兼容失配回退源码、secret 进入 worker 四条路径。

### 第三阶段：静态池与可靠蓝绿

1. 固定大小的 per-App-Version worker pool 和完整 admission control；
2. staging、`minReady` 预热、health check 和 active version 原子切换；
3. 旧版本 drain、超时销毁、显式回滚和逐边界 crash injection；
4. worker replacement、退避、crash budget、quarantine 和跨 App startup fairness；
5. 持续健康、SSE 独立 permit、显式 retire 及对应故障矩阵；
6. secret/key rotation、重启恢复、结构化日志和基础 metrics；
7. delegated cgroup 和 Runtime egress policy 的真实 Linux 集成测试；外部网络隔离属于
   部署环境验收，不是 Host feature。

前三个阶段全部完成并通过 fuzz、sanitizer、soak、故障注入和性能门后才称为 v1。

### 后续：弹性与效率

1. 带 cold-start singleflight 的有界自动扩缩容；
2. endpoint/application circuit breaker 与显式 opt-in activation rollback；
3. 基于 profile 的两级 memory admission；
4. HTTP/2、TLS 或第三方 transport adapter。

v1 完成之前不增加多租户 realm、App binding、资源档位、权重发布 DSL 或业务控制面。

## 最终原则

> Host 定义整机硬边界，App 只能申请子集；版本通过目录交付，通过 deploy 完成预热和
> 蓝绿，通过 retire 显式下线；部署失败保持旧版本，active crash-loop 则退避并 fail
> closed，worker 永远不能越过 Host。
