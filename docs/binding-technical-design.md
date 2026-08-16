# Capsid Binding v1 技术设计

> 状态：已实现（§7.1-§7.9，见下方验收矩阵）；§7.9 的 Linux 特权
> conformance probe 由 Hosted Validity CI 在 root 下强制执行（skip 即
> 失败）。WASI profile 以受控 preopen 运行真实 WebAssembly 工作负载。

## 1. 目标与核心决策

Binding 用于承载由 Host 编写、安装和授信的能力实现，例如 MongoDB、MySQL
或 Redis 客户端。应用只能调用 Binding 主动暴露的方法，不能直接或间接取得
Binding Runtime 的网络、文件、Socket 或其他原生能力。

v1 采用以下设计：

- 一个 worker 进程内最多存在两个 QuickJS Runtime：User Runtime 和
  Binding Runtime；两者拥有独立 Heap、Global、Module Loader 和 Job Queue。
- 只有 worker 至少声明了一个 Binding 时才创建 Binding Runtime。零 Binding
  worker 保持当前单 Runtime 路径，不加载 Binding 代码、不增加 Binding Heap，
  也不扩大 Linux Sandbox。
- Binding Package 名称直接对应 Host 目录名、App 配置键和 import specifier，
  不引入 `db -> provider: mongo` 之类的别名层。
- v1 每个 App 对同一种 Binding 只允许一份配置。例如 App 只能有一个
  `mongo`；多个 Mongo 连接留给后续协议版本。
- User 到 Binding 的调用全部异步。Runtime 之间只通过 C++ 中立值和异步队列
  通信，禁止传递 QuickJS `JSValue`、对象引用或 Native Handle。
- Binding Package 显式声明高层能力和 Capsid 内置 Linux Sandbox Profile。
  Capsid 不分析 JavaScript 猜测 syscall，也不允许 Package 配置原始 syscall。
- Seccomp、Landlock、namespace、cgroup 和 rlimit 是进程级边界；User 与
  Binding 的细粒度权限由不可伪造的 Native Operation Gate 分开执行。
- Server/Listener、Raw Socket、FFI、动态库、进程创建和 Worker 等能力在 v1
  永久关闭。

概念结构如下：

```text
capsid-worker process
├── User Runtime
│   ├── 不可信 App JS
│   ├── capsid:* User Facade
│   └── UserCapabilityPolicy
├── Binding Runtime（仅 Binding 非空时创建）
│   ├── Host 信任的 Binding index.js
│   ├── 授权后的 TJS 模块
│   └── 每个 Binding 独立 BindingCapabilityPolicy
└── Process Sandbox
    ├── Seccomp：User + Binding Profile 并集
    ├── Landlock：User + Binding 路径并集
    ├── Namespace/Firewall
    ├── cgroup v2
    └── rlimit
```

同进程双 Runtime 提供 JavaScript 对象和能力隔离，不提供 Native Exploit 级
隔离。如果威胁模型要求在 QuickJS、TJS 或 C++ 内存破坏后仍隔离 User 和
Binding，必须把 Binding 移到独立进程；这不属于 v1。

## 2. Package 与配置契约

### 2.1 Host 扫描目录

`capsid/host-v2` 增加可选的 `bindingsRoot`：

```json
{
  "apiVersion": "capsid/host-v2",
  "bindingsRoot": "/etc/capsid/bindings"
}
```

目录结构固定为：

```text
/etc/capsid/bindings/
  mongo/
    manifest.json
    index.js
  redis/
    manifest.json
    index.js
```

目录名就是公开的 Binding ID，并同时用于：

```text
Package: /etc/capsid/bindings/mongo
Config:  bindings.mongo
Import:  capsid:binding/mongo
```

Binding ID 必须匹配 `[a-z][a-z0-9-]{0,62}`。Host 只扫描
`bindingsRoot` 的直接子目录，每个 Package 必须且只能包含
`manifest.json` 和 `index.js`。

扫描必须满足以下安全规则：

- 禁止 Symbolic Link、Hard Link、FIFO、Socket、设备文件和额外文件。
- Root、Package 目录及文件必须属于 root 或 Host 有效 UID，且不能
  group/world writable。
- 扫描从一个 `O_NOFOLLOW` Root FD 开始；Package 和文件只通过
  `openat/fstatat(AT_SYMLINK_NOFOLLOW)` 访问。打开前后的 `dev/ino`、Owner、
  Mode、Link Count、Size、mtime 和 ctime 必须一致。
- 文件读取后重新比对打开 FD 与最终目录项；每个 Package 和 Root 在扫描结束时
  都重新枚举并比对。因此 symlink、rename/replace、扫描中增删文件或原地改写都
  fail-closed。
- `manifest.json` 最大 1 MiB，`index.js` 最大 16 MiB；单个 Generation
  使用的 Binding 源码总量最大 64 MiB。
- Manifest 或目录结构错误导致 Host 启动失败；JavaScript 语法、Factory 和
  方法导出错误导致引用该 Binding 的 Generation 预热失败。
- Host 启动时形成不可变 Package 快照。v1 不 Watch 文件，也不支持在线
  Reload；更新 Package 后需重启 Host。
- Generation 提交的 `bindings.json` 是按 Binding ID 排序、字段严格且有界的
  Canonical Snapshot。恢复只读取该快照，不重新扫描 `bindingsRoot`；恢复时重新
  校验 Manifest、App 子权限、Profile/Module 派生值、Config、Secret Revision
  和 64 MiB 聚合上限，重复键、额外字段或非 Canonical Config 一律拒绝。
- `index.js` 必须是单文件、自包含 ESM。第三方依赖需提前 Bundle；禁止相对、
  绝对、`file:`、HTTP 和动态 import。

Host 扫描到 Package 不代表所有 worker 自动加载它。只有 App 明确声明的
Binding 才会进入 Generation，并通过 startup protocol 发给对应 worker。

### 2.2 Binding Manifest

```json
{
  "apiVersion": "capsid/binding-v1",
  "sandbox": {
    "requires": [
      "network-client",
      "filesystem-read"
    ]
  },
  "permissions": {
    "modules": [
      "tjs:internal/core",
      "tjs:utils"
    ],
    "net": {
      "allow": ["*:27017"]
    },
    "fs": {
      "read": ["/etc/capsid/mongo"],
      "write": []
    },
    "env": [],
    "stdio": []
  }
}
```

Manifest 定义 Package 的最大权限，但不声明 Binding ID、方法列表、配置
Schema、用户侧 JavaScript 或 App 实例名。安装 Package 到 Host 管理的
`bindingsRoot` 即表示 Host 批准 Manifest 中声明的上限。

`permissions.modules` 必填，其他权限省略即 deny。未知字段、重复键、重复权限、
未知模块或当前构建不可用的模块全部拒绝。

### 2.3 App 配置

`capsid/app-v2` 增加直接以 Binding ID 为键的 `bindings`：

```json
{
  "apiVersion": "capsid/app-v2",
  "entry": "bundle.mjs",
  "bindings": {
    "mongo": {
      "permissions": {
        "net": {
          "allow": ["127.0.0.1:27017"]
        },
        "fs": {
          "read": ["/etc/capsid/mongo/ca.pem"],
          "write": []
        },
        "env": [],
        "stdio": []
      },
      "config": {
        "database": "orders",
        "tls": true
      },
      "secrets": {
        "password": {
          "valueFrom": "mongo-password"
        }
      }
    }
  },
  "pool": {
    "minReady": 1,
    "maxWorkers": 1
  }
}
```

规则如下：

- `bindings` 的 key 必须命中 Host Registry 中的 Binding ID。
- 不存在 `provider`、`alias` 或 `instance` 字段；这些未知字段必须严格拒绝。
- v1 每个 Binding ID 每个 App 只允许一份配置。
- `config` 是不透明 JSON Object。Host 只校验 JSON、深度和大小，不解释成员；
  每个 Binding 最大 256 KiB。
- `secrets` 只允许 `{ "valueFrom": "<secret-key>" }`，不允许内联 Secret。
- App 的 `net/fs/env/stdio` 必须是 Manifest 上限的可证明子集；省略某类权限
  表示该类权限全部拒绝。
- Binding 所需 TJS 模块由 Manifest 决定，App 不重复声明模块。
- `host-v1` 和 `app-v1` 的严格 Schema 与行为保持不变；Binding 只在 v2 启用。

网络目标统一描述最终 Host/IP/CIDR 与端口，不使用 `mongo://` 或 `mysql://`
Scheme：

```text
db.example.com:27017
*.internal.example.com:443
127.0.0.1:6379
10.0.0.0/8:3306
[::1]:6379
[2001:db8::/32]:443
*:27017
```

每条规则必须有单一明确端口。App 规则只有在能够静态证明被 Manifest 某条规则
包含时才有效；不通过 DNS 结果推导包含关系。`*` 包括 loopback、private、
link-local、IPv6 和 Metadata 地址，但不能突破 Host 配置的 Network Namespace、
路由或 Firewall。

### 2.4 Binding JavaScript 接口

```js
export default function createBinding({ config, secrets, log }) {
  let client;

  return {
    async find(input, call) {
      client ??= await createClient({
        ...config,
        password: secrets.password
      });

      return client.find(input, {
        signal: call.signal
      });
    }
  };
}
```

约束如下：

- Default Export 必须是同步 Factory，不能返回 Promise。
- Factory 初始化阶段只能执行纯 JavaScript 计算，不得启动网络、文件、Timer 或
  其他原生操作。
- `config` 和 `secrets` 是深度冻结的 null-prototype 对象。
- `log` 提供冻结的 `debug/info/warn/error(message, fields?)`，并自动附加 App、
  Generation、Binding 和 Request 元数据。
- Factory 返回对象的 own enumerable function 自动成为公开方法；至少一个、
  最多 128 个。
- 方法名必须是合法 JavaScript Identifier，最长 64 字节；拒绝
  `constructor`、`prototype`、`__proto__`、`then`、`catch` 和 `finally`。
- 用户方法接受零个或一个结构化输入；Binding 实现收到 `(input, call)`。
- `call` 是冻结对象，包含 `requestId`、`deadline` 和 Binding Runtime 内的
  `AbortSignal`。
- 同步值和 Promise 都是合法返回，但用户侧始终异步完成。
- Binding 可以在闭包内维护连接池。Native Handle 归属于 Binding ID，可由同一
  Binding 跨请求复用，但不能跨 Binding 使用。
- v1 不提供显式 Shutdown Hook；worker 销毁时释放 Runtime 和 libuv 资源。

用户接口由 worker 自动生成，不需要 Package 提供 `user.js`：

```js
import mongo from "capsid:binding/mongo";

const rows = await mongo.find({
  collection: "users",
  filter: { active: true }
});
```

Synthetic ESM 只有 frozen、null-prototype 的 default export。用户只能在活跃
Request Async Context 中调用；请求外调用返回 rejected Promise。Binding Runtime
不能导入 `capsid:binding/*`。

### 2.5 Binding 日志边界

Binding 日志不复用可伪造的 User 文本日志格式。Worker 只发送带专用 Flag 的
精确长度帧：

```text
binding-id:u16 | level:u16 | message:u32 | fields-json:u32
```

- Worker 只接受普通 Object 的 `fields`，拒绝 Array、Proxy、不可 JSON 序列化值
  和超过 16 KiB 的 Message/Fields；输出前对当前 Binding 的所有 Secret 明文做
  Message 与 JSON-escaped Fields 双重脱敏。
- Host 要求 Flag 精确、帧无尾随字节、Binding ID/Level 合法，并以拒绝重复键的
  JSON Parser 重新解析和 Canonicalize Fields；Fields 作为 JSON Object 写入，
  不能注入第二条日志行。
- `application` 和 `generation` 只由 Host 的 Pool/Executor 附加，Worker 不能
  提供或覆盖；`binding` 来自已认证帧，`request` 来自协议头。
- 预热阶段使用同一 Decoder。Malformed Binding Log 会使 worker 预热失败；运行
  阶段不会降级成普通文本日志。Secret 不进入 Generation Snapshot 或日志。

## 3. 权限与 Runtime 隔离

### 3.1 三类策略

有效策略必须分为三类，不能把进程 Sandbox 当作 Runtime 权限判断：

```text
ProcessSandbox
    = User OS Requirements
    ∪ 当前 worker 所有 Binding Sandbox Profile

UserCapabilityPolicy
    = Host User Policy ∩ App User Permissions

BindingCapabilityPolicy[bindingId]
    = Binding Manifest ∩ App Binding Permissions
```

每次 Native 操作必须执行：

```cpp
authorize(origin, operation, resource);
```

`origin` 来自不可由 JavaScript 修改的 `JSContext` 或 Native Opaque：

- User Runtime 只查询 `UserCapabilityPolicy`。
- Binding Runtime 查询当前 Binding ID 对应的 `BindingCapabilityPolicy`。
- 没有有效 Origin 或 Binding Context 时 fail-closed。
- 不接受 JavaScript 参数传入或覆盖 Origin/Binding ID。
- Native Handle 创建时记录 Runtime Domain、Binding ID 和打开模式；后续操作继续
  校验 Owner，而不是仅依赖当前调用栈。

### 3.2 User FS 与 Binding FS

Seccomp 和 Landlock 是进程级规则，因此 Binding 的 FS Write 可能扩大 worker 的
内核层 syscall/path 并集，但不能扩大 User JavaScript API：

- User 永远不能 import `tjs:internal/core`、TJS FS、SQLite、WASI 或 Posix Socket。
- User 声明 `capsid:fs` 只获得现有 Capsid User Facade，且仍需 Host Policy 允许。
- Binding 权限不能给 User Facade 增加方法；例如 Binding 的 FS Write 不会让
  User 获得 write API。
- User FS Gate 只使用 User Policy；不能因为 Landlock 已为 Binding 开放某目录
  而允许 User 访问。
- Binding FS Gate 只使用当前 Binding Policy。
- File、Socket、SQLite、WASI 和 Stream Handle 不允许跨 Runtime Clone。
- FFI、Native Addon、Raw Socket、`createFromFD` 和 User WASI 永久拒绝。

例如：

```text
User Policy:
  read /srv/apps/orders/public

Mongo Binding:
  read  /etc/capsid/mongo
  write /var/lib/capsid/mongo
```

Landlock 的进程级规则包含三个路径，但 User 访问 Mongo 路径仍在 User Native
Gate 被拒绝，Mongo 访问 User 路径也在 Binding Gate 被拒绝。

### 3.3 模块与永久禁止能力

模块授权和操作授权必须分离：能够 import 模块不等于能够执行其所有 Native
操作。`tjs:internal/core` 在 Binding Runtime 中是受控 Core；所有网络、文件和
资源操作仍按当前 Binding Policy 检查。

可授权模块包括：

- 纯工具：assert、getopts、hashing、ipaddr、path、utils、uuid。
- Client/资源模块：DNS、TCP/TLS/UDP Client、HTTP/WS Client、FS、FSWatch、
  SQLite、stdio/readline、环境读取、OS Metadata、engine、timers、crypto、
  compression、WASM/WASI。

永久禁止：

- `listen`、`serve`、TCP/TLS/UDP Listener、HTTP/WS Server。
- 整个 `tjs:posix-socket`（v1 不可授权）；以及 Posix
  `bind/listen/accept`、AF_UNIX、Raw Socket、`createFromFD`。
- FFI、动态库、spawn/exec、Worker、exit/kill/signals 和进程控制。
- SQLite Extension Loading。

v1 不直接暴露 txiki 的 Posix Socket 模块，因为该模块把 Client API 与
`createFromFD`、Listener、AF_UNIX 和危险 Socket Option 放在同一原生表面。
后续若提供只含 `AF_INET/AF_INET6` Client TCP/UDP 的独立 Facade，必须在加入
可授权集合前补齐操作级 egress 与 Owner 测试；不能通过初始化现有模块来扩权。

## 4. Linux Sandbox Profile

### 4.1 显式声明，不做源码推断

Capsid 不能可靠地通过扫描 `index.js`、import 列表或执行一次 Trace 推导完整
syscall 集。不同代码路径、TJS/libc 版本和 CPU 架构都会使这种推断不完整。

因此 Manifest 必须在 `sandbox.requires` 中显式选择由 Capsid 定义并版本化的
Profile：

| Profile | 能力 |
| --- | --- |
| `network-client` | DNS、IPv4/IPv6 TCP/TLS/UDP Client |
| `filesystem-read` | 只读文件和目录 |
| `filesystem-write` | 写入、创建、截断、同步、受控 rename/remove |
| `filesystem-watch` | inotify/fswatch |
| `sqlite` | SQLite 文件、锁、pread/pwrite、fsync/ftruncate |
| `wasi` | 受控 WASI Preopen |

Profile 规则：

- 名称和语义由 Capsid Build 固定，Manifest 不能列出原始 syscall。
- 未知、当前架构不支持或尚未实现的 Profile 拒绝加载。
- Profile 与 Capsid/TJS Build Compatibility ID 绑定；TJS 升级必须重新审计。
- `strace` 和 Seccomp Audit 只用于发现遗漏，不能自动生成生产 Allowlist。
- 未进入最终 Profile 的 syscall fail-closed，运行时不得自动扩权。
- Profile 与资源权限必须一致：`net.allow` 要求 `network-client`，`fs.read`
  要求 `filesystem-read`，`fs.write` 要求 `filesystem-write`，TJS SQLite/WASI
  分别要求 `sqlite`/`wasi`。

### 4.2 Seccomp/Landlock 的实现边界

当前严格 Sandbox 已允许常见数据库 Client 所需的 IPv4/IPv6 Stream/Datagram
Socket、`connect`、send/recv、DNS、TLS、event loop 和内存 syscall。因此
MongoDB、MySQL 和 Redis 的常见 TCP/TLS Binding 通常只声明
`network-client`，不需要扩大当前 Client syscall 集。

所有 Profile 的并集仍硬拒绝：

- `bind/listen/accept`。
- Unix/Raw Socket。
- clone/fork/exec。
- ptrace、BPF、perf、io_uring。
- Mount/Namespace 变更。
- 可执行内存。

`filesystem-write`、`filesystem-watch`、`sqlite` 和 `wasi` 已实现为受控
Profile：

- Seccomp 从永久禁止表开始，再按 Profile 追加固定 syscall；永久禁止项优先，
  任何 Profile 都不能覆盖。`filesystem-write` 只增加创建、写入、同步、受控
  rename/remove/mkdir 所需调用；`sqlite` 增加锁、pread/pwrite、ftruncate/fsync。
- Landlock Path Rule 使用显式 Access Mask。Read 与 Write 路径分别形成 Rule，
  进程安装所有 User/Binding Rule 的并集。
- 目录 Write 可以允许普通文件创建、修改、删除和目录内 rename，但不能创建
  设备、FIFO、Unix Socket 或越出授权目录。
- `filesystem-write` 要求能够安全表达所需 Access Mask 的 Landlock ABI；内核
  ABI 太旧时 worker 启动失败，不能降级成部分保护。
- TCP/UDP Client 不开放用户指定本地 bind 地址；不为了 Client Profile 放开
  整个进程的 `bind`。

Linux Profile Conformance Test 直接执行真实工作负载：read、create/write/
rename/unlink/mkdir、inotify、SQLite 所需的 pwrite/fsync/ftruncate/flock、TCP
connect 和 WASI preopen。每个 Profile 同时验证 fork、AF_UNIX、Raw Socket、
bind 和 executable mmap 精确返回拒绝；所有 Profile 的并集也重复永久拒绝项。
Hosted Validity 在 root 下运行 `ctest -L sandbox`，任何 Skip 77 都使 CI 失败。

Network Namespace、Firewall 和 cgroup 仍由 Host 部署环境准备，Binding
Manifest 不能创建或修改它们。

### 4.3 编译、安装与启动证明

Host 在 worker spawn 前生成：

```text
EffectiveBindingPolicies
EffectiveSandboxProfiles
LandlockPathRules
SandboxProfileDigest
```

多个 Binding 的 Profile 和 Path Rule 取并集，但每个 Runtime 的 Native Policy
保持独立。

Worker startup 顺序调整为：

1. 接收并校验 HELLO。
2. 接收零个或多个 Binding Descriptor 和 App Bundle，只保存有界字节，不执行
   JavaScript。
3. 重新 Canonicalize Sandbox Requirements，并与 Host Digest 比对。
4. 进入配置的 namespace，设置 rlimit 和 `no_new_privs`。
5. 安装 Landlock 和 Seccomp TSYNC Filter。
6. 执行无副作用的负向 Sandbox Probe。
7. 根据 Binding 数量创建一个或两个 Runtime并加载代码。
8. READY 返回 Sandbox Proof。

READY v4 增加：

```text
sandbox_profile_digest
seccomp_mode
landlock_abi
applied_feature_bits
network_namespace_identity（配置时）
```

Host 必须逐项比对，不一致或缺少要求时拒绝 worker。安装 syscall 成功和启动证明
只能证明正确 Profile 已安装，不能证明 Binding 所有业务路径都已覆盖；业务兼容性
必须由 Profile Conformance Test 和 Binding 集成测试保证。

没有 Binding 的 worker 不合并任何 Binding Profile，READY 中保持现有 Sandbox
基线身份。

## 5. 双 Runtime 与异步 RPC

### 5.1 Event Loop

有 Binding 时，TJS Overlay 需要支持：

- 由 Capsid 拥有的共享 `uv_loop_t`。
- 两个独立 TJS/QuickJS Runtime 挂载到同一个 Loop。
- 独立 Pump 每个 Runtime 的 Promise Jobs。
- Runtime 关闭自身 Handle但不关闭共享 Loop。
- Scheduler 轮流 Pump User 和 Binding Runtime，且同一时刻只进入一个 Runtime。

不创建第二线程；现有 `clone/clone3` 禁止规则保持不变。必须移除 Binding 路径对
`WorkerRuntime::g_worker` 等进程全局单例的依赖，通过 Runtime/Context Opaque
找到所属状态。

Binding Async Context 同时携带：

```text
BindingToken
Optional BindingCallToken
```

Binding Token 决定能力和 Native Handle Owner；Call Token 决定 Request、
Deadline、Abort 和结果接收者。连接池可跨请求保留 Binding Token。

### 5.2 调用队列

C++ 维护 `user_to_binding` 和 `binding_to_user` 两个队列。调用流程为：

1. User Proxy 创建 Promise。
2. 校验活跃 Request、Binding、Method 和队列配额。
3. 输入克隆为 C++ Neutral Value。
4. 调用加入 `user_to_binding`，立即返回 Promise。
5. Scheduler 在 Binding Runtime 调用对应 Method。
6. 使用 `Promise.resolve()` 统一同步和异步返回。
7. 结果或错误克隆并加入 `binding_to_user`。
8. Scheduler 回到 User Runtime resolve/reject 原 Promise。

禁止从 Binding 调用栈同步重入 User Runtime，也禁止 Binding 导入并调用另一个
Binding。

### 5.3 Structured Clone

允许：

- `undefined`、`null`、Boolean、Number、BigInt、String、Date。
- ArrayBuffer（复制而不 Transfer/Detach）、Uint8Array。
- Array、普通 Object 和 null-prototype Object。

拒绝：

- Function、Promise、Symbol。
- Getter、Setter；检查属性时不得执行 Getter。
- Cycle、Map、Set、Weak Collection。
- Error、RegExp、自定义 Class Instance。
- 无法安全枚举的 Proxy。
- Socket、File、SQLite、WASI、Stream 及所有 Native Handle。

固定限制：

- 每方向单值编码后最大 1 MiB。
- 最大深度 64。
- 最大节点/属性总数 10,000。
- 每 Request 最多 64 个未完成 Binding Call。
- 每 worker 最多 1024 个未完成 Binding Call。
- Binding Runtime Heap 默认 64 MiB，仅在至少一个 Binding 时分配和计入 Worker
  Memory Commitment。

Request Cancel/Deadline 会 Abort 相关 Call。尚未派发的调用直接取消，已派发
调用的晚到结果丢弃，但不强制销毁同 Binding 的共享连接池。普通 Binding
throw/reject 只失败当前调用；Binding Runtime OOM、不可中断循环或 Runtime Fatal
Error poison 整个 worker，并交给现有 Recovery 替换。

## 6. ABI、协议与 Generation

保持现有 `capsid_worker_config` Layout 和 ABI v7 兼容，Binding 使用 additive、
带版本 Descriptor 的新 API：

```c
typedef struct capsid_binding_descriptor {
    uint32_t struct_size;
    uint32_t version;
    const char *binding_name;
    capsid_bytes source;
    capsid_bytes config_json;
    const capsid_binding_secret *secrets;
    uint32_t secret_count;
    const capsid_binding_policy *policy;
    const capsid_sandbox_requirements *sandbox;
} capsid_binding_descriptor;

capsid_result capsid_worker_load_binding(
    capsid_worker *worker,
    const capsid_binding_descriptor *binding);
```

Descriptor、Source、Config、Secret 和 Policy 在调用返回前完成复制。
`capsid_worker_load_binding()` 只能在 App Bundle 之前调用。

Worker Protocol 升级为 v4：

```text
HELLO
LOAD_BINDING(mongo...)
LOAD_BINDING(redis...)
LOAD_BUNDLE(app...)
READY + Sandbox Proof
```

`LOAD_BUNDLE` 开始即封存 Binding 列表。零个 `LOAD_BINDING` 必须走现有单
Runtime 路径。

Generation Identity v2 增加按 Binding ID 排序的 `binding_set_digest`，包含：

- Binding ID。
- Manifest 和 Source Digest。
- Canonical Config Digest。
- Effective Permission Digest。
- Sandbox Requirements/Profile Digest。
- Secret Key ID 和不透明 Secret Revision。
- Binding Runtime Compatibility Version。

Secret Value 不进入 Digest、Disk、Log 或 Audit。Generation 提交不可变 Binding
Snapshot；worker replacement 和 Host 恢复使用提交快照，不依赖后来变化的
`bindingsRoot`。

## 7. TDD 实施顺序

实现必须遵循 Red → Green → Refactor。每一阶段先增加最小失败测试并确认失败
原因，再写最小生产实现；阶段结束运行相关测试、完整 `ctest` 和适用的
Sanitizer。

### 7.1 Schema 与安全扫描

先覆盖：

- `bindings.mongo` 和 `capsid:binding/mongo` 的一一映射。
- `provider/alias/instance` 字段严格拒绝。
- Host/App v1 行为不变。
- Symlink、Hard Link、Owner/Mode、额外文件和大小限制。
- Manifest Profile、模块、权限一致性和确定性 Digest。

### 7.2 零 Binding 回归

先证明：

- 零 `LOAD_BINDING` 不创建 Binding Runtime。
- 不增加 Binding Heap、Scheduler、Sandbox Profile 或 Landlock Path。
- 未声明却 import Binding 时失败且不触发懒加载。
- 当前单 Runtime、ABI v7 和性能路径保持不变。

### 7.3 权限与 Origin 隔离

先覆盖：

- Binding FS Write 不给 User 增加 Write 能力。
- Landlock 包含 Binding 目录时，User Native Gate仍拒绝访问。
- User 声明 `capsid:fs` 仍不能 import TJS Core/FS。
- 伪造 Runtime Origin、跨 Binding Handle、跨 Runtime Handle 全部失败。
- Binding Socket/File/SQLite Handle作为返回值时 Clone 拒绝。

### 7.4 Sandbox Profile

先覆盖：

- 未知、缺失或与权限不一致的 Profile 失败。
- Mongo TCP/TLS 在 `network-client` 下工作。
- Server、bind/listen/accept/raw/unix 永久失败。
- FS Write 只在授权目录成功，目录逃逸失败。
- SQLite 正常读写、锁和同步，以及未授权路径失败。
- READY Digest、Seccomp Mode、Landlock ABI 和 namespace identity 比对。
- 无 Binding 时 Sandbox 身份保持当前基线。

### 7.5 双 Runtime

先覆盖：

- Heap、Global、Module Cache 和 Job Queue 分离。
- Shared Loop 同时驱动两个 Runtime且公平调度。
- 禁止跨 Runtime `JSValue`。
- Async Context正确传播 Binding ID 和 Call Token。
- ASan/TSan 下反复创建、取消和销毁无泄漏或竞态。

### 7.6 RPC 与 Structured Clone

先覆盖所有允许值的 Round-trip，再覆盖 Getter 不执行、Cycle、Function、Promise、
Class 和 Native Handle 拒绝。增加并发、Backpressure、Cancel、Deadline、晚到
结果、普通 Error 和 Runtime Poison 测试。

### 7.7 Egress 与原生模块

对 DNS、TCP、TLS、UDP、Fetch Redirect、WebSocket Client、Posix Socket 和连接
池重连逐个增加“允许目标成功、未授权目标在 syscall 前失败”测试。未经测试覆盖
的 Native Client 入口不得进入可授权列表。

### 7.8 Generation 与恢复

证明 Manifest、Source、Config、Policy、Profile 或 Secret Revision 变化均改变
Generation Digest；Secret Value 不落盘；worker replacement 使用提交快照；
Binding Fatal Error进入现有 Crash Budget。

### 7.9 Linux 特权门禁

- 在支持 Namespace、cgroup、Seccomp 和 Landlock 的特权 Linux 环境运行真实
  Profile Conformance Test。
- Sandbox Test 返回 Skip 77 在 Hosted Validity Workflow 中视为失败。
- 每个 Profile 必须同时拥有真实正向工作负载和永久禁止项反向测试。
- TJS Vendor 升级必须重新运行 syscall trace、Profile Test 和 Sandbox Digest
  Golden Test；Trace 仅用于审计，不生成 Allowlist。

## 8. 验收标准

- App 能通过 `capsid:binding/<binding-id>` 异步调用 Host Binding。
- 配置、目录和 import 使用同一个 Binding ID，不存在 Provider Alias。
- 无 Binding worker 不创建 Binding Runtime，也不增加 Sandbox 或资源开销。
- User 与 Binding 不共享 Heap、Global、Module Cache、JS 对象或 Native Handle。
- Manifest 是 Host 最大授权，App 只能缩小资源范围。
- Package 显式声明 Capsid 内置 Sandbox Profile，不配置或自动推断 syscall。
- Seccomp/Landlock 使用进程级并集；Native Gate始终按 Runtime Origin和 Binding ID
  做细粒度检查。
- Binding FS Write 不能扩大 User FS API或 User Path权限。
- 所有 Client Egress 入口都经过 Binding Policy；Server、Raw Socket、FFI、
  Process 和 Worker 能力无条件拒绝。
- READY 提供可由 Host 比对的 Sandbox Proof。
- Binding Package、Policy、Profile、Config 和 Secret Revision进入不可变
  Generation Identity，Secret Value从不落盘。
- 现有 ABI v7、无 Binding 运行路径和完整测试套件保持通过。

### 8.1 需求到测试的验收矩阵

以下测试必须命中真实 Native/Host 边界；只验证 Parser 或 Mock 不单独构成验收
证据。

| 验收边界 | 主要自动化证据 |
| --- | --- |
| Binding ID 在目录、App 和 import 中一一对应；拒绝 Provider Alias | `host_binding_config`、`host_binding_manifest`、`host_binding_registry`、`host_binding_compile` |
| FD-relative Package 扫描、rename/replace、Owner/Mode/Link/大小和不可变恢复快照 | `host_binding_registry`、`host_binding_compile`、`host_secret_snapshot` |
| 零 Binding 不创建第二 Runtime、不加载模块、不改变 READY/Sandbox 基线 | `worker_zero_binding_regression` |
| 双 Runtime Heap/Global/Module/Job 隔离和只通过异步队列调用 | `worker_zero_binding_regression`、`binding_rpc` |
| Structured Clone 类型、Getter/Proxy/Cycle/Native Handle 拒绝及配额 | `worker_zero_binding_regression`、`binding_rpc` |
| Factory 无 Native Token，Config/Secret 深冻结 null-prototype，方法发现不执行 Getter | `worker_zero_binding_regression`、`host_binding_compile` |
| User/Binding/Binding 间 Native Owner 不可跨越；User import 间接能力失败 | `capability_policy`、`worker_zero_binding_regression`、`worker_sandbox_enforcement` |
| DNS/TCP/TLS/UDP/HTTP Redirect/WS/FS/SQLite/WASI 操作级 Gate；Posix Socket 不可授权 | `egress_policy`、`worker_fetch_direct_egress`、`worker_zero_binding_regression`、`txiki_vendor_patch_integrity` |
| RPC 64-bit ID、Deadline、Cancel/Abort、Late Result、Quota 回收和 Poison | `binding_rpc`、`worker_zero_binding_regression` |
| READY 完整比对 Profile Digest、Feature Bits、Seccomp、Landlock 和 Namespace | `host_binding_compile`、`host_worker_executor_contract`、`worker_sandbox_network_namespace`、`worker_zero_binding_regression` |
| Secret Revision/Runtime Compatibility 进入 Digest，Secret Value 不进入快照/日志 | `host_binding_compile`、`host_secret_snapshot`、`worker_zero_binding_regression` |
| Binding Log 严格帧、Host-owned App/Generation、JSON Fields 和 Secret 脱敏 | `structured_log_emits_single_line_json`、`host_worker_executor_contract`、`worker_zero_binding_regression` |
| 每个 Linux Profile 真实正向操作且永久拒绝 fork/AF_UNIX/raw/bind/executable mmap | `worker_binding_sandbox_{read,write,watch,sqlite,network,wasi,union}`、`worker_sandbox_namespaces` |
| TJS Overlay 版本、Patch 顺序、审计锚点和升级基线一致 | `txiki_async_context_inventory_audit`、`txiki_vendor_patch_integrity`、`txiki_overlay_audit_negative_controls` |

### 8.2 2026-08-16 审计执行记录

- macOS Debug 核心回归：Registry、Compile、Secret Snapshot、IPC、Capability、
  Host Log/Executor、RPC、Sandbox 和零 Binding，共 10/10 通过。
- macOS ASan（`detect_leaks=0`，Darwin 的 LeakSanitizer 不可用）：上述 Host/RPC/
  双 Runtime 核心集合 8/8 通过，`halt_on_error=1`。
- Linux 6.8/GCC 13 TSan：关闭 ASLR 后 Host/IPC/Policy/RPC/Sandbox/双 Runtime
  核心集合 8/8 通过，`halt_on_error=1`；这是 sanitizer 证据，不替代下述真实
  Seccomp/Landlock Profile Probe。
- Linux 6.8/GCC 13 特权容器：Host Compile/Registry、IPC、Capability、RPC、零
  Binding、Namespace 和 7 个真实 Binding Profile Probe，共 16/16 通过，0
  skip；最终 WebSocket 补丁后又重跑 Sandbox Enforcement、7 个 Profile 与零
  Binding 综合回归，9/9 通过，0 skip。Profile 结果不是 macOS 的 Skip 77
  替代品。
- WebSocket Client 使用真实 `101 Switching Protocols` 服务端覆盖：无授权规则在
  `connect(2)` 前拒绝、显式允许目标握手成功且 `onopen` 恢复 Binding ID、句柄传给
  另一 Binding 后拒绝操作；非 `ws/wss` scheme、非法端口和缺失 URL 同样关闭。
- macOS 全量串行回归（排除明确要求外部 WPT 配置、以及要求 clean worktree 的两项
  环境门禁，并把耗时打包复现项单独运行）235/235 通过；
  `worker_package_reproducibility` 单独通过。Darwin 的 Profile/CPU Affinity/AB
  Evidence skip 已由上述 Linux 真实结果覆盖，不被计作 Linux 证据。
- Hosted Validity 的最终持续门禁位于
  `.github/workflows/testing-validity.yml`：Release 完整 `ctest`、root
  `ctest -L sandbox`（出现 `Skipped` 即失败）、delegated cgroup/netns，以及
  ASan/UBSan/TSan 矩阵。

此记录是一次审计结果，不替代 CI。TJS、Profile、Manifest Schema、协议或 Native
入口发生变化后，必须重新执行 §7.9 与本矩阵，不能沿用旧结果。
