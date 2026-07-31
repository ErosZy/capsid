# JavaScript 模块与权限参考

本文面向编写 bundle 和配置宿主策略的用户，回答两个问题：

1. 应用可以 import 哪些模块；
2. 每个 JavaScript API 需要哪个宿主权限。

机器权威清单是
[`capability-manifest.json`](capability-manifest.json)，C ABI 权威定义是
[`include/capsid/runtime.h`](../include/capsid/runtime.h)。

## 最重要的规则

- 应用只允许 import 宿主白名单中的 `capsid:*` 公共模块；
- `tjs:*` 和 `tjs:internal/*` 对应用永久禁止，不能配置开放；
- `allowed_modules` 只决定模块能否 import，不自动允许模块里的资源操作；
- `fetch()` 不属于 `capsid:net`，它使用独立的 egress policy；
- 没有模块 import 的 Web API 大多不需要 capability rule，例外是 `fetch()`；
- deny rule 总是优先，未知或格式错误的策略会让 worker 启动失败。

这套查询描述符借鉴了 Deno permission 的表达方式，但不是
`Deno.permissions` API，也不提供 `request()`、`revoke()` 或 permission prompt。
JavaScript 只能通过 `capsid:permissions` 查询宿主给出的不可变结果。

## 模块 specifier 如何判定

| bundle 中的 specifier | 能否配置 | 结果 |
| --- | --- | --- |
| 已构建的 `capsid:*` | 可以 | 必须列入 `allowed_modules`，否则 `module is not authorized` |
| 已知但未构建的 `capsid:*` | 可以列入，但不能使用 | import 时 `module is unavailable` |
| 任意 `tjs:*` | 不可以 | 永久 `module is forbidden` |
| `tjs:internal/*` | 不可以 | 永久 `module is forbidden` |
| `node:`、`file:`、`http:`、`https:`、`data:` | 不可以 | 永久 `module is forbidden` |
| `/绝对路径`、`./相对路径`、`../相对路径` | 不可以 | 永久 `module is forbidden` |
| 未知 specifier | 不可以 | `module is unavailable` |

因此下面不是有效配置：

```cpp
capsid::CapabilityPolicyBuilder policy;
policy.allow_module("tjs:assert"); // 错误：spawn 返回 CAPSID_INVALID_ARGUMENT
```

正确做法是使用 Capsid 公共名称：

```cpp
capsid::CapabilityPolicyBuilder policy;
policy.allow_module("capsid:assert");
```

等价的 C 配置是：

```c
const char *modules[] = {
    "capsid:assert",
    "capsid:hashing"
};

capsid_capability_policy capability;
capsid_capability_policy_init(&capability);
capability.application_identity = "report-worker";
capability.allowed_modules = modules;
capability.allowed_module_count = 2;

capsid_worker_config config;
capsid_worker_config_init(&config);
config.capability_policy = &capability;
```

`allowed_modules` 中出现 `tjs:*`、未知模块、重复模块或永久禁止项时，不会等到
JavaScript import 才失败，而是让 `capsid_worker_spawn()` 直接返回
`CAPSID_INVALID_ARGUMENT`。

应用必须在发布阶段打成自包含 ESM。bundle 内遗留的相对、绝对、远程或 npm
运行时 import 都会被拒绝。

## txiki.js 工具模块的公共映射

Capsid restricted runtime 内部复用了六个不带 ambient authority 的 txiki.js
工具实现，但应用只能使用对应的 `capsid:*` 名称：

| 禁止应用直接 import | 可授权的公共模块 | 公共导出 | 操作权限 |
| --- | --- | --- | --- |
| `tjs:assert` | `capsid:assert` | 默认 assertion 对象：`equal`、`notEqual`、`is`、`isNot`、`ok`、`notOk`、`fail`、`throws`、`doesNotThrow` 及别名 | 无 |
| `tjs:getopts` | `capsid:getopts` | 默认 `getopts(args, options)` | 无 |
| `tjs:hashing` | `capsid:hashing` | `SUPPORTED_TYPES`、`createHash()`；hash 对象提供 `update()`、`digest()`、`bytes()` | 无 |
| `tjs:ipaddr` | `capsid:ipaddr` | 默认 ipaddr.js 对象 | 无 |
| `tjs:utils` | `capsid:utils` | `format()`、`inspect()` | 无 |
| `tjs:uuid` | `capsid:uuid` | 默认 uuid 对象 | 无 |

“操作权限为无”不等于模块自动可见。这六个模块仍需逐个写入
`allowed_modules`，只是导入成功后调用其纯计算 API 不再经过资源 rule。

`capsid:hashing` 的实现需要一次 `tjs:internal/core`。loader 只允许
`tjs:hashing` 在解析自身依赖时取得该模块；应用直接或动态 import
`tjs:internal/core` 始终被拒绝，包括它已经进入模块缓存的情况。
`globalThis.tjs`、`process`、`Deno` 和 `Bun` 也不存在。

## API 到权限的完整映射

| JavaScript API | 必须授权的模块 | C/C++ 权限配置 | rule resource | 匹配语义 |
| --- | --- | --- | --- | --- |
| `permissions.query()` | `capsid:permissions` | 无操作 rule | 查询描述符决定 | 只查询，不授予权限 |
| `env.get(name)` | `capsid:env` | `CAPSID_PERMISSION_ENV` | 环境变量名，如 `APP_MODE` | exact，rule 可使用单个末尾 `*` |
| `system.get("runtimeVersion")` | `capsid:system` | `CAPSID_PERMISSION_SYS` | `runtimeVersion` | exact |
| `system.get("featureFlags")` | `capsid:system` | `CAPSID_PERMISSION_SYS` | `featureFlags` | exact |
| `storage.get/set/delete/clear/keys(namespace, ...)` | `capsid:storage` | `CAPSID_PERMISSION_STORAGE` | namespace | exact |
| `stdio.write("stdout", message)` | `capsid:stdio` | `CAPSID_PERMISSION_STDIO` | `stdout` | exact |
| `stdio.write("stderr", message)` | `capsid:stdio` | `CAPSID_PERMISSION_STDIO` | `stderr` | exact |
| `fs.readText(path)` | `capsid:fs` | `CAPSID_PERMISSION_READ` | canonical absolute path | 本路径及其子树 |
| `fs.stat(path)` | `capsid:fs` | `CAPSID_PERMISSION_READ` | canonical absolute path | 本路径及其子树 |
| `fs.list(path)` | `capsid:fs` | `CAPSID_PERMISSION_READ` | canonical absolute path | 本路径及其子树 |
| 全局 `fetch(url, init)` | 无 | `capsid_egress_policy`；推荐同时配置 capability `net_policy` | hostname/IP/CIDR + port | 两层策略取交集 |
| 六个纯工具模块 | 对应 `capsid:*` | 无操作 rule | 无 | 只有模块 gate |

`CAPSID_PERMISSION_NET` 不能写进普通 `capsid_permission_rule`。网络规则必须使用
`capsid_egress_rule` / `capsid_egress_policy`，C++ builder 对应 `.net()`。

以下 permission 名称能被策略解析或查询，但当前没有对应产品 API：

| 权限 | C 枚举 | 当前状态 |
| --- | --- | --- |
| `write` | `CAPSID_PERMISSION_WRITE` | unavailable；没有文件写入、删除、rename 或 mkdir |
| `ffi` | `CAPSID_PERMISSION_FFI` | unavailable |
| `rawSocket` | `CAPSID_PERMISSION_RAW_SOCKET` | unavailable |
| `engine` | `CAPSID_PERMISSION_ENGINE` | unavailable |
| `sys` 的其他 kind | `CAPSID_PERMISSION_SYS` | unavailable |
| `stdio` 的 `stdin` | `CAPSID_PERMISSION_STDIO` | unavailable |

给这些项目添加 allow rule 不会使操作出现，`permissions.query()` 仍返回
`unavailable`。

## 宿主配置配方

### 只开放纯工具

```cpp
capsid::CapabilityPolicyBuilder capability;
capability
    .application_identity("formatter")
    .allow_module("capsid:assert")
    .allow_module("capsid:hashing")
    .allow_module("capsid:utils")
    .allow_module("capsid:uuid");
```

这里不需要任何 `allow(CAPSID_PERMISSION_...)`，应用也不会因此获得文件、环境变量
或网络访问。

### 环境变量

模块、operation rule 和环境快照缺一不可：

```cpp
capsid::CapabilityPolicyBuilder capability;
capability
    .application_identity("orders-api")
    .allow_module("capsid:env")
    .allow(CAPSID_PERMISSION_ENV, "APP_MODE", 1001)
    .environment("APP_MODE", "production");
```

```js
import { env } from "capsid:env";

env.get("APP_MODE"); // "production"
```

worker 不读取宿主进程环境。`.environment()` 没有对应 allow rule、存在重复键或
模块未授权时，spawn 会 fail closed。

### 只读目录，并拒绝其中一个子目录

```cpp
capsid::CapabilityPolicyBuilder capability;
capability
    .allow_module("capsid:fs")
    .allow(
        CAPSID_PERMISSION_READ,
        "/srv/capsid/orders",
        1101)
    .deny(
        CAPSID_PERMISSION_READ,
        "/srv/capsid/orders/secrets",
        1102);
```

`/srv/capsid/orders/config.json` 被允许，
`/srv/capsid/orders/secrets/token` 被拒绝。路径必须是 canonical absolute
path；deny 优先于 allow。strict sandbox 还会把有效只读根写入 Landlock，
授权根本身若是 symlink 会导致启动失败。

### storage namespace

```cpp
capability
    .allow_module("capsid:storage")
    .allow(CAPSID_PERMISSION_STORAGE, "tenant-a", 1201);
```

```js
import { storage } from "capsid:storage";

storage.set("tenant-a", "session", "value");
storage.get("tenant-a", "session");
storage.keys("tenant-a");
storage.delete("tenant-a", "session");
storage.clear("tenant-a");
```

namespace 必须逐个精确授权。数据只存在于单 worker 内存中，不跨 worker
共享，worker 销毁即丢失。

### 出站 Fetch

推荐把同一目标同时写入宿主直接 egress policy 和 capability net policy：

```cpp
capsid_egress_rule direct_rule;
capsid_egress_rule_init(&direct_rule);
direct_rule.action = CAPSID_EGRESS_ALLOW;
direct_rule.target = "api.example.com";
direct_rule.port_start = 443;
direct_rule.port_end = 443;
direct_rule.rule_id = 2001;

capsid_egress_policy direct;
capsid_egress_policy_init(&direct);
direct.rules = &direct_rule;
direct.rule_count = 1;

capsid::CapabilityPolicyBuilder capability;
capability.net(
    CAPSID_EGRESS_ALLOW,
    "api.example.com",
    443,
    443,
    2002);

const capsid_capability_policy &descriptor =
    capability.descriptor();

capsid_worker_config config;
capsid_worker_config_init(&config);
config.egress_policy = &direct;
config.capability_policy = &descriptor;
```

两个策略同时存在时取交集。每次请求都会检查初始 hostname、DNS 解析后的地址和
redirect。loopback、私网、link-local 等受保护地址还需要显式 IP/CIDR allow。

## JavaScript 权限查询

先授权查询模块：

```cpp
capability.allow_module("capsid:permissions");
```

然后应用可以查询有效状态：

```js
import { permissions } from "capsid:permissions";

permissions.query({
  name: "net",
  host: "api.example.com",
  port: 443,
});

permissions.query({
  name: "read",
  path: "/srv/capsid/orders/config.json",
});

permissions.query({
  name: "env",
  variable: "APP_MODE",
});

permissions.query({
  name: "sys",
  kind: "runtimeVersion",
});

permissions.query({
  name: "stdio",
  stream: "stdout",
});

permissions.query({
  name: "storage",
  namespace: "tenant-a",
});
```

查询返回 `"granted"`、`"denied"`、`"partial"` 或 `"unavailable"`。
支持的 descriptor 字段如下：

| `name` | 资源字段 |
| --- | --- |
| `read`、`write`、`ffi` | `path` |
| `net` | `host` 和 `port`，必须同时提供 |
| `env` | `variable` |
| `sys` | `kind` |
| `stdio` | `stream` |
| `storage` | `namespace` |
| `engine` | `operation` |
| `rawSocket` | 无 |

只有 `net` 支持同时省略 `host` 和 `port` 来查询两层网络策略的汇总状态；
此时可能返回 `"partial"`。其他 permission 应提供表中的资源字段。查询不会
申请权限，也不会改变后续操作结果。

## 不通过 capability rule 的全局 API

固定 Web profile 中的以下类别不需要模块白名单或 operation rule：

- Request、Response、Headers、URL、Streams、Encoding、Events；
- timers、queueMicrotask、structuredClone、Compression；
- Web Crypto 和随机数；
- 固定限制下的 WebAssembly；
- `console.*`。

其中 `console.*` 会形成有界的 `CAPSID_EVENT_LOG`，但不等同于
`capsid:stdio.write()`：console 是固定 Web profile 的一部分，
`capsid:stdio` 是需要显式 module + stream rule 的 stdout/stderr 通道。
宿主仍需持续排空或丢弃日志事件，并自行执行脱敏和限速。

全局 `fetch()` 是唯一直接接入 capability/egress policy 的标准 Web API。

## 错误与审计

模块拒绝分为三类：

- `module is forbidden`：specifier 永久禁止，任何配置都不能开放；
- `module is unavailable`：当前 build 没有该模块或 specifier 未知；
- `module is not authorized`：模块已构建，但不在本 worker 的白名单中。

模块、operation 和 query 判定都会产生 `CAPSID_EVENT_AUDIT`。宿主可使用
`capsid_audit_record_decode()` 读取阶段、决策、rule ID、应用 identity、资源和
manifest hash。
