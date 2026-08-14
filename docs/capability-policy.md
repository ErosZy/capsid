# 宿主能力策略

公开 ABI 提供可选且不可变的 `capsid_capability_policy`。它是 worker 启动时
由宿主给出的策略快照，不是 JavaScript permission prompt。

## 三层门禁

能力按以下顺序独立判断：

1. module 或 operation 必须存在于 restricted build；
2. module 必须出现在 `allowed_modules`；
3. 具体资源必须匹配 allow rule，且不能匹配任何 deny rule。

未知 descriptor、`allowed_modules` 中的未知模块、重复或为零的 rule ID、
非 canonical resource 和不支持的 policy version 都会令
`capsid_worker_spawn()` 返回 `CAPSID_INVALID_ARGUMENT`。spawn 在返回前同步校验并
复制全部嵌套字符串和规则，因此策略变更必须新建 worker。

`allowed_modules` 只接受已知的 `capsid:*` 公共名称。所有 `tjs:*` 和
`tjs:internal/*` 都是永久禁止的实现命名空间，不能通过策略开放；把它们写入
allowlist 会使 spawn 失败。完整 specifier 判定和
API→permission 映射见
[JavaScript 模块与权限参考](module-permissions.md)。

## 当前可用范围

当前 restricted build 提供十二个可显式授权的模块：

- `capsid:permissions`：只读查询宿主能力策略；
- `capsid:env`：读取宿主显式提供、逐键授权的不可变环境快照；
- `capsid:system`：只读取编译期 runtime version 与 feature flags；
- `capsid:storage`：按 namespace 授权、只存活于单个 worker 的内存键值存储；
- `capsid:stdio`：把获准的 stdout/stderr 写入转换为有界宿主日志事件；
- `capsid:fs`：读取获准的 canonical host path，不提供写入或 watcher；
- `capsid:assert`、`capsid:getopts`、`capsid:hashing`、`capsid:ipaddr`、
  `capsid:utils`、`capsid:uuid`：无 ambient authority 的纯 utility。

utility 模块仍必须逐个列入 `allowed_modules`；“已构建”不等于“应用可导入”。
它们没有资源操作，因此不会绕过第三层 operation gate，也不会获得
`globalThis.tjs`、process、文件系统、环境变量或网络能力。
`capsid:hashing` 只允许 loader 代它解析一次受信任的
`tjs:internal/core` 哈希原语；应用直接导入该 internal module 始终被拒绝，
包括它已经进入 module cache 的情况。

标准 `fetch()` 通过 `net_policy` 接入 operation gate；`capsid:env.get()`
、`capsid:system.get()`、`capsid:storage` 和 `capsid:stdio.write()` 操作分别通过
`CAPSID_PERMISSION_ENV`、`CAPSID_PERMISSION_SYS` 与
`CAPSID_PERMISSION_STORAGE`、`CAPSID_PERMISSION_STDIO` rule 接入同一门禁。

`fetch()` 的域名规则是授权边界：域名及端口在 host 阶段获准后，该域名由
客户端 DNS 解析出的地址（包括私网、loopback 和 link-local 地址）也视为获准，
调用方无需、也不应枚举会随 DNS 变化的 IP。解析地址仍会匹配显式 IP/CIDR
规则，任何显式 deny 都优先于域名 allow。直接以数字 IP 发起请求则继续执行
protected-range 防护；私网、loopback、link-local 等地址必须有显式 IP/CIDR
allow 才能以数字 IP 直接访问。因此，不论 `internal-api.example:443` 被视为
内网域名还是公网域名，只要它已获授权，就可以访问它实际解析到的全部地址，
包括 `10/8` 等 protected range；这一判断不依赖域名的“公网/内网”分类。

被拒绝的 `fetch()` 错误会区分三种原因：host/端口没有匹配授权规则、地址位于
protected range 且未被显式授权，以及命中了显式 deny 规则。错误文本仅用于
诊断；策略判断仍以规则和 deny 优先级为准，应用不应解析错误字符串来实施授权。

`write`、`ffi`、`rawSocket` 和 `engine`
matcher 已能解析和测试，但对应操作未构建，JavaScript 查询返回
`unavailable`。`read`、`env` 与 `storage` 已构建；`stdio` 只提供 stdout/stderr
输出，stdin 仍为 `unavailable`；`sys` 只有 `runtimeVersion` 与
`featureFlags` 可用，其他资源仍为 `unavailable`。

capability policy 当前版本是 2；解码器仍接受没有环境快照字段的版本 1，
并保持原有语义。未知版本、版本 1 携带环境数据或版本 2 缺失快照段都会
fail closed。

机器可读的权威清单是
[`capability-manifest.json`](capability-manifest.json)。CMake 在 configure
阶段计算 SHA-256，并把该值写入每条 audit record。

process、worker、HTTP/WebSocket server、WASI、内部 runtime module、远程
import 和 file/path import 永久禁止，不能通过 capability descriptor 开启。
`capsid:path` 暂不构建，因为其完整上游 API 的 `resolve()`/`relative()` 会隐式
读取 `tjs.cwd`；只有引入 capability-scoped virtual cwd 后才可重新评估。

其余已知扩展继续 fail closed，原因和重开条件也写入机器清单：

- `capsid:net` 不复用上游 POSIX socket；它绕过 resolved-address egress hook。
  HTTP(S) 客户端需求使用已经覆盖 hostname、每个 DNS 地址和每次 redirect 的
  标准 `fetch()`；
- `capsid:websocket` 要等 client-only 路径同样完成 DNS/地址复核、队列上限、
  cancel 和 request ownership，server/upgrade 不在产品范围；
- `capsid:sqlite` 的 benchmark-only 固定只读数据库不等于产品 API。正式开放
  前必须禁用 extension loader，加入 SQL authorizer、内存/行数/执行时间 quota，
  并只允许内存库或经 path capability 授权的文件；
- `capsid:readline` 依赖已明确关闭的 terminal stdin；请求输入应由宿主通过
  FetchRPC 提供；
- `capsid:fs.write` 会重开 seccomp/Landlock 写边界，`capsid:fs.watch` 会引入
  跨请求回调；两者在各自 mutation/ownership 设计完成前保持 unavailable。

这些条目不是“允许但尚未写文档”，而是由 manifest 审计、逐模块启动拒绝测试
和最终二进制负控共同保证的显式不提供结论。

## C 嵌入示例

```c
capsid_egress_rule net_rule;
capsid_egress_rule_init(&net_rule);
net_rule.action = CAPSID_EGRESS_ALLOW;
net_rule.target = "api.example.com";
net_rule.port_start = net_rule.port_end = 443;
net_rule.rule_id = 1001;

capsid_egress_policy net;
capsid_egress_policy_init(&net);
net.rules = &net_rule;
net.rule_count = 1;

const char *modules[] = {
    "capsid:permissions",
    "capsid:env",
    "capsid:hashing"
};
capsid_permission_rule env_rule;
capsid_permission_rule_init(&env_rule);
env_rule.action = CAPSID_PERMISSION_ALLOW;
env_rule.permission = CAPSID_PERMISSION_ENV;
env_rule.resource = "APP_MODE";
env_rule.rule_id = 1002;

capsid_env_entry environment;
capsid_env_entry_init(&environment);
environment.name = "APP_MODE";
environment.value = "production";

capsid_capability_policy capability;
capsid_capability_policy_init(&capability);
capability.application_identity = "tenant-a";
capability.allowed_modules = modules;
capability.allowed_module_count = 3;
capability.rules = &env_rule;
capability.rule_count = 1;
capability.env_entries = &environment;
capability.env_entry_count = 1;
capability.net_policy = &net;

capsid_worker_config config;
capsid_worker_config_init(&config);
config.worker_path = "/path/to/capsid-worker";
config.capability_policy = &capability;
```

C++11 头文件提供 `capsid::CapabilityPolicyBuilder`，用于在
`capsid_worker_spawn()` 调用前持有临时字符串和 descriptor；环境快照用
`.environment("APP_MODE", "production")` 添加。

如果同时配置 `capsid_worker_config.egress_policy` 与
`capsid_capability_policy.net_policy`，有效策略是两者交集：请求 hostname、
DNS 解析后的每个地址和每次 redirect 都必须同时获准。native HTTP client
仍在 worker 内，不会引入宿主 HTTP broker。

## JavaScript 查询

获准导入的 bundle 只能查询不可变的有效状态：

```js
import { permissions } from "capsid:permissions";

permissions.query({
  name: "net",
  host: "api.example.com",
  port: 443,
}); // "granted"、"denied"、"partial" 或 "unavailable"
```

导出对象被冻结，不提供 `request()`、`revoke()`、prompt 或 mutation API。
import 错误会区分：

- `module is forbidden`：该类别永久不能启用；
- `module is unavailable`：类别已知，但当前 build 没有实现；
- `module is not authorized`：模块存在，但不在 `allowed_modules`。

## 环境快照

```js
import { env } from "capsid:env";

env.get("APP_MODE"); // "production"、"" 或 undefined
```

worker 进程环境始终清空；运行时不会调用 `getenv()`，也不会枚举宿主环境。
只有 `capsid_env_entry` 中显式提供、且同时被有效 allow rule 覆盖的键才能进入
HELLO。deny rule 优先；越权键、重复键、通配符键名、空指针、超限值和未授权
`capsid:env` 的快照都会使 spawn 失败。值在 spawn 返回前完成深复制，因此
调用方随后修改原始缓冲不会改变 worker；不同 worker 持有独立快照。

`env.get()` 对每次访问重新执行 operation gate：已授权但未提供的键返回
`undefined`，被拒绝的键抛错，两者都不会回退到宿主 ambient environment。

## 运行时元数据

```js
import { system } from "capsid:system";

system.get("runtimeVersion"); // "0.1.1"
system.get("featureFlags");   // 冻结的编译期能力对象
```

该模块不调用 uname/gethostname，不读取用户、网络接口、负载、uptime 或内存
状态。即使策略中存在这些 sys allow rule，operation 仍返回
`unavailable`；这使“规则解析器认识某个资源”不会被误解成“build 已实现它”。

## Worker 内存存储

```js
import { storage } from "capsid:storage";

storage.set("tenant-a", "session", "value");
storage.get("tenant-a", "session"); // "value" 或 undefined
storage.keys("tenant-a");           // 冻结且按 key 排序的数组
storage.delete("tenant-a", "session");
storage.clear("tenant-a");
```

每次操作都重新校验精确 namespace rule；namespace 只允许
ASCII 字母、数字、`_`、`-`、`.`，长度最多 128 bytes，不支持通配符。
key 必须非空、最多 256 UTF-8 bytes 且不能包含 NUL；value 最多
16 KiB。每个 namespace 最多 256 项、key 与 value 合计最多 64 KiB。
越权、非法输入、单值超限和 quota 拒绝都会产生 operation deny audit。
成功访问只在每个 worker 首次使用该 namespace 时记录 allow，避免正常键值操作
淹没后续拒绝事件。

状态只保存在 `capsid-worker` 的私有内存中：同一 worker 的后续请求可见，
不同 worker 不共享，worker 销毁即清空。模块不打开文件、不读取路径，也不复用
txiki.js 基于 SQLite 的 localStorage，因此没有隐含的磁盘或目录权限。

## 有界标准输出

```js
import { stdio } from "capsid:stdio";

stdio.write("stdout", "started");
stdio.write("stderr", "warning");
```

`write()` 不接触 worker 的真实 fd。它把 stream 名和最多 16 KiB 的字符串
编码为带当前 request ID 的 `CAPSID_EVENT_LOG`，由宿主决定是否以及如何落盘。
stdout 与 stderr 必须分别有精确 allow rule；stdin 即使配置 allow 也保持
`unavailable`。非法 stream、超限消息和越权写入都 fail closed 并产生审计。
成功 stream 每个 worker 只记录一次 allow，避免日志循环挤掉拒绝事件。

消息按 UTF-8 bytes 计量并保留内嵌 NUL。输出还受 worker 的有界 IPC queue
限制；队列满时同步抛错，不会阻塞进程或绕到真实标准输出。

## 只读文件系统

```js
import { fs } from "capsid:fs";

fs.readText("/srv/app/config.json");
fs.stat("/srv/app/config.json"); // 冻结的 { type, size }
fs.list("/srv/app/assets");      // 冻结且排序的名称数组
```

只接受 canonical absolute path，并在每次调用时执行 `CAPSID_PERMISSION_READ`
allow/deny 匹配。strict sandbox 会把有效 allow 根同步加入只读 Landlock
规则；配置根若是 symlink，worker 在执行 bundle 前即启动失败。实际打开使用
`openat2(RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS)`，因此末端和中间路径的
symlink 都不会被跟随，也不存在先检查再打开的竞态窗口。

`readText()` 只接受普通文件且最多读取 1 MiB；`stat()` 只报告普通文件或目录的
类型与大小；`list()` 最多返回 1024 项，不跟随目录项。所有 API 同步、有界，
返回对象被冻结。写入、删除、rename、mkdir 和 fswatch 不在该模块中；
`CAPSID_PERMISSION_WRITE` 继续查询为 `unavailable`，Landlock 也保持全局拒写。

## 审计事件

能力判定通过 `CAPSID_EVENT_AUDIT` 交给宿主，用
`capsid_audit_record_decode()` 解码。记录包含 worker、application identity、
request ID、stage、decision、rule ID、policy version、module、capability、
规范化资源和 capability manifest SHA-256。

audit view 指向事件缓冲，只在同一 worker 的下一次 event API 调用前有效。
worker 对相同的连续非 allow 判定最多记录 8 次，并把总速率限制为每秒 64 条。
宿主必须持续排空事件，不能把该通道当成无界日志。

capability rule 是应用授权边界；seccomp、Landlock、namespace、cgroup 和
宿主 firewall 是独立且更强的进程边界。未来若开放 FFI 或 raw socket，宿主
必须接受它们可能绕过普通 JavaScript policy。

### 逃逸级能力门禁

`capsid:ffi` 与 `capsid:raw-socket` 不属于普通 capability。它们可以绕过路径、
DNS、redirect 和逐操作授权，因此当前安全结论是"不提供"，而不是"依赖规则
谨慎开放"。

- `CAPSID_ENABLE_FFI_CAPABILITY` 与
  `CAPSID_ENABLE_RAW_SOCKET_CAPABILITY` 明确存在且默认 `OFF`；
  任一开关设为 `ON` 都在 configure 阶段 fail closed，因为项目尚无独立 ABI、
  OS sandbox profile 与完整负控；
- 直接传入 txiki 的 `BUILD_WITH_FFI=ON` 同样被顶层配置拒绝，不能绕过 Capsid
  开关；restricted txiki overlay 不打包 FFI、POSIX socket 或相关 bytecode，
  最终 worker 还必须通过符号、translation unit 和 module specifier 审计。

自动化证据：`escape_capability_defaults`（两开关默认 OFF 且 txiki FFI 未暗中
启用）、`escape_capability_configure_negative_controls`（开启即 configure
失败）、`worker_binary_audit` 及其负控（危险 initializer/translation unit/
loader specifier 未进入最终 worker，且审计器能捕获注入）、
`worker_sandbox_enforcement`（真实进程 strict seccomp/Landlock，含 raw socket
拒绝）和 capability manifest 拒绝矩阵（应用导入这两个模块得到
`unavailable`）。

将来若产品确实需要其中任一能力，应新开安全设计和 ABI 版本，至少覆盖库路径
与符号约束、socket family/type/protocol、DNS/redirect 绕过、fd 传递、资源
配额、跨请求/跨租户隔离和独立 OS sandbox。不能把当前 fail-closed 开关改成
"实验性可用"来规避这些前置条件。
