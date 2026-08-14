# host.json 与 capsid.json 配置参考

managed 模式（`capsid-host --mode managed`）用两个 JSON 文件描述整机和每个
应用版本。两个文件都 fail closed：重复 key、未知字段、非法枚举和越界值都会
让启动或部署失败，不会取默认值悄悄放行。本文以当前 `src/host/config.cc`、
`host_config_model.cc` 和 `managed_host.cc` 的实现为准。

## 两层配置的职责

| 文件 | 位置 | 职责 |
| --- | --- | --- |
| `host.json` | `--host-config` 传入 | 整机：应用根、状态根、secret 根、listener、全局权限上限、容量、恢复策略 |
| `capsid.json` | `<applicationsRoot>/<app>/<version>/` | 单个 app 版本：入口、权限申请、worker 资源、请求窗口、池大小 |

Host 的 `permissions` 与 App 的 `permissions` 是**交集**，App 申请不能扩大
Host 上限。`maximums` 封顶 App 的申请（0 = 不限）；`defaults` 只作整机声明，
不注入生效配置——capsid.json 不写的字段用 worker 自身默认。

## 目录布局

```text
<applicationsRoot>/              # host.json: applicationsRoot
  orders/                        # app id（小写字母/数字开头，[a-z0-9._-]，≤63）
    v1/                          # 版本 id（每版本一个目录）
      capsid.json                # app-v1 配置（必需）
      bundle.mjs                 # 自包含 ESM bundle（必需）
      bundle.qjsb                # 可信字节码：三者全有或全无
      bytecode.json              #   （bytecode.json = 摘要/来源信息）
      bytecode.sig               #
<stateRoot>/                     # host.json: stateRoot（Host 拥有，禁止人工编辑）
  apps/
    orders/
      active.json                # 当前 active generation（原子落盘）
      generations/<generation>/  # 部署时快照的配置与 artifact 记录
<secretRootTemplate 替换 {application}>   # 例如 secrets/orders/
  API_TOKEN                      # 每个 secret key id 一个普通文件，内容即值
```

## host.json

必需字段：`apiVersion`、`applicationsRoot`、`stateRoot`、
`secretRootTemplate`（必须包含 `{application}` 占位符）、`admin.unix`。
其他字段全部可选。

```json
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/applications",
  "stateRoot": "/srv/capsid/state",
  "secretRootTemplate": "/srv/capsid/secrets/{application}",

  "admin": {
    "unix": "/run/capsid/admin.sock",
    "mode": "0600"
  },

  "listeners": [
    {
      "name": "public",
      "tcp": "0.0.0.0:8080",
      "publicScheme": "https",
      "publicAuthority": "orders.example.com",
      "trusted": false,
      "routing": { "mode": "path", "suffix": "" },
      "limits": {
        "connections": 512,
        "headerBytes": "32KiB",
        "headerTimeout": "5s",
        "bodyIdleTimeout": "30s",
        "streamIdleTimeout": "60s"
      }
    }
  ],

  "permissions": {
    "modules": ["capsid:env", "capsid:fs", "capsid:storage", "capsid:stdio"],
    "environmentNames": ["APP_MODE", "DATABASE_*"],
    "fsReadRoots": ["/srv/capsid/config"],
    "fetchTargets": ["api.example.com:443", "metrics.example.com:443"],
    "storageNamespaces": ["session"],
    "stdioStreams": ["stdout", "stderr"]
  },

  "isolation": {
    "mode": "strict",
    "required": ["cgroup-v2"],
    "cgroupRoot": "/sys/fs/cgroup/capsid"
  },

  "trustedBytecodeKeys": {
    "2026-08": "/etc/capsid/keys/ed25519-2026-08.pub"
  },

  "defaults": {
    "worker": {
      "jsHeap": "64MiB",
      "processAddressSpace": "256MiB",
      "memoryMax": "256MiB",
      "fileDescriptors": 64,
      "pidsMax": 8
    },
    "request": {
      "timeout": "5s",
      "maxInflightPerWorker": 64,
      "maxStreamingInflightPerWorker": 2,
      "streamIdleTimeoutMs": 60000,
      "writeTimeoutMs": 10000
    },
    "pool": {
      "queueRequests": 256,
      "queueHeaderBytes": "2MiB",
      "queueTimeout": "10s"
    }
  },

  "maximums": {
    "worker": { "memoryMax": "512MiB" },
    "request": { "maxInflightPerWorker": 128 },
    "pool": { "queueRequests": 1024 }
  },

  "capacity": {
    "workersTotal": 16,
    "activationSurgeWorkers": 0,
    "startupsConcurrent": 2,
    "queuedRequestsTotal": 2048,
    "queuedHeaderBytesTotal": "16MiB",
    "workerMemoryCommitTotal": "4GiB"
  },

  "recovery": {
    "crashBudget": { "maxEvents": 5, "window": "60s" },
    "restartBackoff": {
      "initial": "100ms",
      "maximum": "10s",
      "jitter": "10%",
      "stableReset": "60s"
    },
    "replacementsConcurrentPerApp": 1,
    "activeHealthInterval": "5s",
    "activeHealthFailures": 3
  }
}
```

### 字段说明与硬性校验

- `apiVersion` 必须精确等于 `capsid/host-v1`；
- `admin.mode` 只接受字符串 `"0600"`；Admin socket 只接受与 Host 同 euid
  （`SO_PEERCRED`/`getpeereid`）的 peer，其他进程立即 403；
- `isolation.mode` 只接受 `"strict"`；`required` 是额外 sandbox feature
  数组（如 `"cgroup-v2"`），`cgroupRoot` 是委派的 cgroup 父目录；
- `listeners`：`tcp` 为 `IP:port`；`publicScheme`/`publicAuthority` 是 worker
  可观察 URL 的组成；`trusted` 缺省 false，控制代理头信任边界；
- `permissions`：Host 全局 allowlist。`environmentNames` 支持
  `NAME*` 后缀通配（与运行时 `valid_env_pattern` 同文法）；`fetchTargets`
  语法是 `host` 或 `host:p1,p2`（逗号分隔端口列表）；`fsReadRoots` 是只读根；
- `trustedBytecodeKeys`：release id → Ed25519 公钥文件路径的自由映射；
- `defaults`/`maximums` 的 `worker`/`request` 子字段与 capsid.json 同语法
  （见 [capsid.json 怎么写](capsid-json.md)）；`fileDescriptors` 必须 ≥1；
- `capacity.workersTotal` 是整机 worker 数唯一上限（`activationSurgeWorkers`
  ≥0，缺省 0 表示拒绝零停机替换）；`workerMemoryCommitTotal` 是所有 worker
  内存承诺总量；
- `recovery` 缺省值：crashBudget 5 次/60s、backoff 初始 100ms 上限 10s、
  jitter 10%、stableReset 60s、并发替换 1。`jitter` 语法是 `"10%"`（百分比）
  或裸整数（basis points）。
- 大小统一用 `KiB`/`MiB`/`GiB`/`KB`/`MB`/`GB` 后缀；时长用 `ms`/`s`/`m`。

## capsid.json（每个 app 版本）

**怎么一步一步写，看 [capsid.json 怎么写（教程）](capsid-json.md)**——从最小
可用版（3 个字段）逐节加到完整配置，含每个字段的值域、常见错误表和部署三步。
这里只留字段速查：

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| `apiVersion` | ✓ | 必须精确 `capsid/app-v1` |
| `pool.minReady` | ✓ | 固定池大小，必须与 `maxWorkers` 相等 |
| `pool.maxWorkers` | ✓ | 固定池大小，必须与 `minReady` 相等 |
| `pool.queueRequests` / `queueHeaderBytes` / `queueTimeout` | | 队列，0 = 关闭排队；受 `maximums.pool` 封顶 |
| `permissions.modules` | | 导入的 `capsid:*` 模块（`tjs:*` 恒不可开放） |
| `permissions.env` | | 环境变量，键 → `{value}` 或 `{valueFrom}`（恰好一个） |
| `permissions.fs.read.allow` / `deny` | | 只读根；`deny` 优先于 `allow` |
| `permissions.fetch.allow` | | 出站目标 `host` 或 `host:p1,p2`；不写 = 全部拒绝 |
| `permissions.storage.namespaces` | | 只读存储 namespace（`[A-Za-z0-9._-]` ≤128） |
| `permissions.stdio` | | 只接受 `stdin`/`stdout`/`stderr` |
| `worker.jsHeap` / `processAddressSpace` / `memoryMax` / `fileDescriptors` / `pidsMax` | | 不写 = worker 自身默认；受 `maximums.worker` 封顶 |
| `request.timeout` / `maxInflightPerWorker` / `maxStreamingInflightPerWorker` / `streamIdleTimeoutMs` / `writeTimeoutMs` | | 请求窗口与 SSE 槽位 |
| `healthCheck.path` / `timeout` | | worker 内部路径（不走 listener 路由）；空 = 不探测 |

与 capsid.json 同目录的 artifact 规则：

- `bundle.mjs` 必需；`bundle.qjsb` + `bytecode.json` + `bytecode.sig` 是可信
  字节码三元组，**全有或全无**，缺任一个都拒绝部署；
- 字节码只有通过 `trustedBytecodeKeys` 中对应 release 的 Ed25519 验签、
  摘要、精确 source name 与 Runtime compatibility ID 校验后才被接受；
- 每次部署把配置、bundle 与校验结果快照进
  `stateRoot/apps/<app>/generations/<generation>/`，generation identity 随
  任何配置或 artifact 变化。

## secret 文件

`secretRootTemplate` 替换 `{application}` 后得到该 app 的 secret 目录；
`valueFrom` 引用的 key id 就是目录内文件名：

```sh
mkdir -p /srv/capsid/secrets/orders
printf '%s' 'postgres://user:pass@db.example.com/app' \
  > /srv/capsid/secrets/orders/db-url
chmod 0600 /srv/capsid/secrets/orders/db-url
```

- key id 文法 `[A-Za-z0-9._-]`，不含 `..`，有长度上限；
- secret 文件必须是普通文件（symlink 拒绝），读取前后核验 size/ctime，
  中途被修改会失败；值进入不可变 `capsid:env` 快照，不进入 worker 进程环境。

## 启动与运维

```sh
./build-release/capsid-host --mode managed --host-config /etc/capsid/host.json
```

`--host-config` 必须是 Host 属主拥有的普通文件（O_NOFOLLOW、euid 检查、
≤1 MiB、读取前后 mtime 核验）。

Admin API 只走 Unix socket（与 Host 同 euid）：

```sh
# 部署（蓝绿：先 staging/预热/健康检查，原子切换，失败保持旧版本）
curl --unix-socket /run/capsid/admin.sock \
  -X POST http://localhost/v1/deploy \
  -H 'Content-Type: application/json' \
  -d '{"application":"orders","version":"v2"}'
# → 202 {"operationId":"...","application":"orders","version":"v2","status":"..."}

# 查询操作状态
curl --unix-socket /run/capsid/admin.sock \
  http://localhost/v1/operations/<operationId>

# app 状态
curl --unix-socket /run/capsid/admin.sock http://localhost/v1/apps/orders

# 显式下线（retire 管理动作，tombstone 表达，不以删目录表达）
curl --unix-socket /run/capsid/admin.sock \
  -X POST http://localhost/v1/apps/orders/retire

# 指标
curl --unix-socket /run/capsid/admin.sock http://localhost/metrics
```

所有端点响应都是有界的 JSON/文本；未知路径 404，带 body 的非 deploy 请求
400，方法不符 405，未授权 peer 403。

## 请求路由

listener 使用 `routing.mode = "path"` 时，请求路径必须带
`/@capsid/<app>/` 前缀（与 CLI 的 `--routing path` 同一契约）；`suffix`
与 `publicAuthority` 等字段共同决定 worker 可观察的绝对 URL。外部反向代理
（nginx/Caddy/Envoy）负责 TLS/H2 终止与对外路径映射。
