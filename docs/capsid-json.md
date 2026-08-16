# capsid.json 怎么写（教程）

`capsid.json` 描述**一个应用版本**要什么权限、多少资源、池多大。它放在
版本目录里，和 bundle 一起部署：

```text
<applicationsRoot>/<app-id>/<version>/
    capsid.json     ← 本文的主角
    bundle.mjs      ← 自包含 ESM bundle（必需，同一目录）
```

主机配置（listener、全局权限上限、容量）在
[host.json](host-config.md)，两边的权限取交集：capsid.json 申请的内容
必须同时被 host.json 允许，否则部署失败。

本文按"最小可用 → 逐步加东西 → 完整示例"的顺序教你写。全部字段以
`src/host/config.cc`、`managed_host.cc` 的实现为准。

## 第一步：最小可用版本（3 个字段就能跑）

```json
{
  "apiVersion": "capsid/app-v1",
  "pool": {
    "minReady": 2,
    "maxWorkers": 2
  }
}
```

- `apiVersion`：必须精确等于 `capsid/app-v1`，其他值拒绝；
- `pool.minReady` 和 `pool.maxWorkers`：**两个都必需**，且值必须相等
  （v1 池是固定大小）。`2` 表示这个版本始终维持 2 个 worker。

这样已经能部署，但应用不能导入任何 `capsid:` 模块，也不能出站 fetch。
需要什么就往下逐节加。

## 第二步：模块权限（`permissions.modules`）

应用 `import` 了哪个 `capsid:` 模块，就必须在这里列出，否则 import 直接失败
（`module is not authorized`）：

```json
{
  "permissions": {
    "modules": ["capsid:env", "capsid:fs", "capsid:storage", "capsid:stdio"]
  }
}
```

可用模块见[模块与权限参考](module-permissions.md)。规则：

- 只认 `capsid:*` 公共名；写 `tjs:*` 或 `tjs:internal/*` 会**直接拒绝**，这是
  永久禁止项，不是开放项；
- 纯工具模块（`capsid:assert`、`capsid:hashing` 等）也必须逐个列出；
- 标准 `fetch()` 不走这里——出站网络用下面的 `permissions.fetch`。

## 第三步：环境变量（`permissions.env`）

应用读到的环境变量**只来自这里**（worker 进程环境是清空的），两种来源，
**恰好二选一**：

```json
{
  "permissions": {
    "env": {
      "APP_MODE": { "value": "production" },
      "DATABASE_URL": { "valueFrom": "db-url" }
    }
  }
}
```

- `value`：字面量直接写；
- `valueFrom`：引用 secret 文件。key id `db-url` 对应
  `<secretRootTemplate 替换 {application}>/db-url` 这个普通文件，文件内容就是
  值（见[host.json 参考](host-config.md#secret-文件)）；
- 键名文法：**字母或 `_` 开头**，后续只能字母、数字、`_`（不能带 `*`，
  不能数字开头）——`1APP_MODE`、`APP*MODE` 都会拒绝；
- 同一个键**不能同时写 `value` 和 `valueFrom`**，两个都写或都不写都拒绝；
- 环境值上限受运行时约束，超大值在快照编译阶段失败。

## 第四步：只读文件系统（`permissions.fs`）

`capsid:fs` 只能读、不能写。`allow` 是授权根，`deny` 在 `allow` 之上优先：

```json
{
  "permissions": {
    "fs": {
      "read": {
        "allow": ["/srv/capsid/config"],
        "deny": ["/srv/capsid/config/private.json"]
      }
    }
  }
}
```

- 必须是规范化绝对路径；strict sandbox 下授权根不能是 symlink；
- 根目录必须同时出现在 host.json 的 `permissions.fsReadRoots` 里。

## 第五步：出站网络（`permissions.fetch`）

控制标准 `fetch()` 的目标，语法是 `host` 或 `host:p1,p2`（端口列表）：

```json
{
  "permissions": {
    "fetch": {
      "allow": ["api.example.com:443", "metrics.example.com:443"]
    }
  }
}
```

- 每次请求都会检查 hostname、DNS 解析出的**每个地址**和**每次 redirect**；
- 目标必须同时被 host.json 的 `permissions.fetchTargets` 允许；
- 不写这一节 = 出站 Fetch 全部拒绝（host.json 的 `egress_policy == NULL`
  同理）。

## 第六步：内存存储与日志（`permissions.storage` / `permissions.stdio`）

```json
{
  "permissions": {
    "storage": { "namespaces": ["session"] },
    "stdio": ["stdout", "stderr"]
  }
}
```

- `storage.namespaces`：只接受 ASCII 字母、数字、`_`、`-`、`.`，最长 128
  字符；每个 namespace 有独立 quota，只存活于单个 worker；
- `stdio`：只接受 `stdin`/`stdout`/`stderr` 三个字符串；`capsid:stdio`
  只发有界日志事件，不碰真实 fd。

## 第七步：资源上限（`worker`）——不写也行

不写 = worker 用运行时自带默认。写了是给自己一个明确的边界：

```json
{
  "worker": {
    "jsHeap": "64MiB",
    "processAddressSpace": "256MiB",
    "memoryMax": "256MiB",
    "fileDescriptors": 64,
    "pidsMax": 8
  }
}
```

- 大小统一用后缀：`KiB`/`MiB`/`GiB`/`KB`/`MB`/`GB`，其他后缀（如裸
  `256`、`1M`）拒绝；
- `jsHeap` 限制 QuickJS 堆，`processAddressSpace` 限制进程地址空间，
  `memoryMax` 是整体内存上限——三者独立，不互相冒充；生效内存上限取
  `max(memoryMax, jsHeap, processAddressSpace)`；
- `fileDescriptors` 必须 ≥1；
- 所有值受 host.json `maximums` 封顶：超过即部署失败（`maximums` 里 0 =
  不限）。`host.json defaults` 只作整机声明，不注入生效配置。

## 第八步：请求窗口（`request`）

```json
{
  "request": {
    "timeout": "5s",
    "maxInflightPerWorker": 64,
    "maxStreamingInflightPerWorker": 2,
    "streamIdleTimeoutMs": 60000,
    "writeTimeoutMs": 10000
  }
}
```

- `timeout` 是请求级超时（`ms`/`s`/`m`）；同步 CPU 死循环被 interrupt 打断
  后 worker 视为不可复用；
- `maxInflightPerWorker` 是每个 worker 的并发请求窗口；
- `maxStreamingInflightPerWorker` 是 SSE/streaming 槽位（缺省 2），
  `streamIdleTimeoutMs` 是流空闲超时——慢客户端由 `writeTimeoutMs` 兜底。

## 第九步：队列（`pool` 可选字段）

```json
{
  "pool": {
    "minReady": 2,
    "maxWorkers": 2,
    "queueRequests": 256,
    "queueHeaderBytes": "2MiB",
    "queueTimeout": "10s"
  }
}
```

- 缺省 0 = 关闭排队，负载超窗口直接拒绝；
- 同样受 host.json `maximums.pool` 封顶。

## 第十步：健康检查（`healthCheck`）

```json
{
  "healthCheck": {
    "path": "/health",
    "timeout": "2s"
  }
}
```

- `path` 是 **worker 内部路径**（Host 直接把请求发给 worker，不经过
  listener 路由），所以要写应用自己 `fetch()` 里的路径，如 `/health`；
- 不写或空 path = 不探测；连续失败超过 host.json `recovery.activeHealthFailures`
  次才替换 worker。

## 完整示例

一个读取配置、请求上游、带存储和健康检查的订单应用：

```json
{
  "apiVersion": "capsid/app-v1",

  "permissions": {
    "modules": ["capsid:env", "capsid:fs", "capsid:storage", "capsid:stdio"],
    "env": {
      "APP_MODE": { "value": "production" },
      "DATABASE_URL": { "valueFrom": "db-url" }
    },
    "fs": {
      "read": {
        "allow": ["/srv/capsid/config"],
        "deny": ["/srv/capsid/config/private.json"]
      }
    },
    "fetch": {
      "allow": ["api.example.com:443"]
    },
    "storage": {
      "namespaces": ["session"]
    },
    "stdio": ["stdout", "stderr"]
  },

  "worker": {
    "jsHeap": "64MiB",
    "processAddressSpace": "256MiB",
    "memoryMax": "256MiB",
    "fileDescriptors": 64
  },

  "request": {
    "timeout": "5s",
    "maxInflightPerWorker": 64,
    "maxStreamingInflightPerWorker": 2,
    "streamIdleTimeoutMs": 60000,
    "writeTimeoutMs": 10000
  },

  "pool": {
    "minReady": 2,
    "maxWorkers": 2,
    "queueRequests": 256,
    "queueHeaderBytes": "2MiB",
    "queueTimeout": "10s"
  },

  "healthCheck": {
    "path": "/health",
    "timeout": "2s"
  }
}
```

## 部署三步

```sh
# 1. 把 capsid.json 和 bundle 放进版本目录（app id：小写字母/数字开头，
#    [a-z0-9._-]，≤63 字符；版本 id 同文法）
mkdir -p /srv/capsid/applications/orders/v2
cp capsid.json bundle.mjs /srv/capsid/applications/orders/v2/

# 2. 通过 Admin API 部署（Unix socket，与 Host 同 euid；蓝绿发布：
#    先预热 + 健康检查，原子切换，失败保持旧版本）
curl --unix-socket /run/capsid/admin.sock \
  -X POST http://localhost/v1/deploy \
  -H 'Content-Type: application/json' \
  -d '{"application":"orders","version":"v2"}'

# 3. 查状态
curl --unix-socket /run/capsid/admin.sock http://localhost/v1/apps/orders
```

部署时如果发现 bundle 目录里同时有 `bundle.qjsb`、`bytecode.json`、
`bytecode.sig`，会走可信字节码路径——这三个文件**全有或全无**，缺任一个
都拒绝部署。

## 本地模式（v0.1.3，`--capsid-json`）：不部署，直接跑

`capsid-host --mode single-worker`（及 `static-pool`）是基准测试/本地开发
数据面：不做蓝绿部署，也没有 Admin API。这种模式下没有 host.json——
**capsid.json 文档本身就是权限权威**，不再与 host.json 取交集：

```sh
# 默认读当前目录的 ./capsid.json；不存在 = 回到 v0.1.2 的无权限基线（全拒绝）
capsid-host --mode single-worker --source-bundle bundle.mjs

# 显式指定：文件必须存在，缺失直接启动失败（不静默跳过）
capsid-host --mode single-worker --source-bundle bundle.mjs \
  --capsid-json ./my-policy.json
```

- 权限照常生效：`permissions.modules` / `env` / `fs` / `fetch` / `storage` /
  `stdio`，走与托管模式**完全相同**的冻结 schema 校验和编译管线（规则 id、
  摘要、规范化都在），本教程前面各节都适用；
- `pool` 仍是 schema 必需字段（`minReady` == `maxWorkers`），但数值是惰性的
  ——worker 数量由 CLI（`--workers`）决定；
- 本地模式不能兑现的段**直接拒绝启动**，绝不静默跳过：
  - `worker` / `request` / `healthCheck`：容量、资源与请求窗口由 CLI 掌控；
  - env `valueFrom`：没有托管模式的 secret store，环境变量只能写字面量
    `{"value": "..."}`。

静态池模式下每个 shard 走同一条加载路径，任一 shard 加载失败都会让整个池
启动失败。

## 常见错误（全部 fail closed）

| 错误写法 | 结果 |
| --- | --- |
| `"apiVersion": "capsid/app-v2"` | 拒绝：apiVersion 必须精确 `capsid/app-v1` |
| `"modules": ["tjs:assert"]` | 拒绝：`tjs:*` 永久禁止 |
| `"minReady": 2, "maxWorkers": 4` | 拒绝：cross-field values must be equal |
| 只写 `minReady` 不写 `maxWorkers` | 拒绝：missing required field |
| `"env": { "1APP": {...} }` | 拒绝：环境键名必须字母/`_` 开头 |
| 一个 env entry 同时写 `value` 和 `valueFrom` | 拒绝：恰好一个 |
| `"fetch": { "allow": ["example.com:70000"] }` | 拒绝：端口越界 |
| `"stdio": ["log"]` | 拒绝：只接受 stdin/stdout/stderr |
| `"storage": { "namespaces": ["a/b"] }` | 拒绝：namespace 非法字符 |
| `"worker": { "jsHeap": "64" }` | 拒绝：大小必须有 KiB/MiB/GiB/KB/MB/GB 后缀 |
| 重复 key（同一对象出现两次 `pool`） | 拒绝：JSON_REJECT_DUPLICATES |
| 任何未列出的字段（如 `"cpu": 2`） | 拒绝：unknown configuration field |
| 申请超过 host.json `maximums` | 部署拒绝 |
| bundle 目录只有 `bundle.qjsb` 没有签名 | 拒绝：字节码必须全有或全无 |
| 本地模式写 `worker` / `request` / `healthCheck` 段 | 拒绝启动：not applicable in local mode（CLI-owned） |
| 本地模式 env 用 `valueFrom` | 拒绝启动：valueFrom is unavailable in local mode |
| `--capsid-json` 指向不存在的文件 | 拒绝启动：cannot find …（默认 `./capsid.json` 缺失除外，那是无权限基线） |

所有校验在部署前完成，错误不会在运行时悄悄跳过。
