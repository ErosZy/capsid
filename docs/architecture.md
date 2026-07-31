# 架构与产品边界

Capsid Runtime 是基于固定 txiki.js vendor 树构建的、进程隔离的 JavaScript
运行时。它面向嵌入式 HTTP 宿主，提供版本化 Minimum Common Web API 子集，
不宣称完整 ECMA-429 conformance。Capsid 是唯一产品名称；外部组织名称仅用于说明
标准来源和历史内部实现。

## 交付物与应用模型

项目生成两个主要产物：

- `capsid-worker`：可进入沙箱的持久 JavaScript 子进程；
- `libcapsid_runtime`：稳定 C ABI，以及头文件中的 C++11 RAII 封装。

每个 worker 从内存加载一个自包含 ESM bundle。常规路径加载源码；宿主也可加载
由完全相同 Capsid/QuickJS 构建生成并校验过的可信字节码。字节码不是租户输入
格式，也不能作为 sandbox 边界。bundle 不得依赖外部、远程或
文件模块，并导出以下任一形式：

```js
export default { fetch(request) { /* ... */ } }
```

```js
export function fetch(request) { /* ... */ }
```

运行时不提供 HTTP server、TLS 终止、路由、worker 池或租户调度。这些由
Rust/C++ 等宿主负责。

它也不是通用 POSIX 应用容器：terminal readline、任意 TCP、长期 fswatch 和
WebSocket server 不属于 request-worker 产品面。需要持续连接或后台 watcher
时，由宿主拥有生命周期；worker 只接收有界、可取消、可归属到请求的输入。

## 进程与数据路径

```text
宿主 HTTP/TLS、路由、worker 池、审计
                  │
                  │ FetchRPC v1 / Unix socketpair
                  ▼
capsid-worker
  QuickJS-ng + libuv
  Capsid Web API bootstrap
  内存中的应用 bundle
  restricted native core
    ├─ timers / encoding / URL / streams / crypto / compression
    ├─ WAMR
    └─ DNS + TLS + HTTP client（仅供标准 fetch）
```

入站请求和应用响应经过带长度前缀的 FetchRPC。应用调用的出站 `fetch()` 直接
使用 worker 内部的 txiki.js HttpClient/libwebsockets，不经过宿主 HTTP
代理或 FetchRPC broker。

## JavaScript 表面

profile 名称为 `CAPSID-MIN-2025-subset-v0`。主要包含：

- Event、Abort、timers、microtask 和错误/rejection reporting；
- Encoding、URL/URLSearchParams/URLPattern；
- Blob、File、FormData、Fetch、Streams、Compression；
- Web Crypto、Console、Performance；
- MessageChannel/MessagePort；
- WebAssembly Module、Instance、Memory、Table、Global 与
  compile/instantiate/validate（含 streaming 版本）；
- `navigator.userAgent`。

正式偏差和资源上限见
[合规偏差](conformance-deviations.md)。txiki.js 的 `globalThis.tjs`、
`tjs:internal/*`、process/child process、server、WASI、外部模块加载、REPL、
文件执行和宿主 IPC 控制永久不暴露。

框架只是普通 bundle。当前验证 Hono 4.12.32、itty-router 5.0.24 和
H3 2.0.1-rc.26；运行时源码没有框架探测或专用分支。

## 受限构建

当前 overlay 在 restricted profile 下直接排除 txiki.js 的通用 core
bootstrap，以及 FFI、path、POSIX socket、readline、SQLite、WASI 等危险
builtin bytecode；其余 native translation unit 即使进入静态 archive，也必须
经链接裁剪和最终二进制正/负控审计证明没有进入 `capsid-worker`。因此安全声明
同时由编译条件、module loader 和最终产物审计支撑，不能只依赖“运行时不可达”。

最终产物保留 QuickJS-ng、libuv、WAMR、Web API 实现及标准 `fetch()` 所需的
DNS/TLS/HTTP client。mimalloc 可选且默认关闭。vendor 不原地修改：
CMake 在构建目录创建 overlay，并按顺序应用 `patches/txiki/`。

## 安全边界

安全策略分为互相独立的层：

1. 构建层决定能力是否存在；
2. module loader 决定 bundle 能否导入；
3. capability/egress policy 决定具体资源操作；
4. Linux seccomp、Landlock、namespace、cgroup 和宿主 firewall 提供进程边界。

JavaScript 不能自行申请或扩大权限。当前可构建模块包括只读的
`capsid:permissions`，以及六个无 ambient authority 的纯 utility：
`capsid:assert`、`capsid:getopts`、`capsid:hashing`、`capsid:ipaddr`、`capsid:utils`、
`capsid:uuid`；`capsid:env` 只读取宿主显式提供、逐键授权且按 worker 隔离的
不可变快照，不读取进程环境；`capsid:system` 只返回编译期版本与 feature
flags，不采集宿主系统信息；`capsid:storage` 提供按 namespace 授权、带固定
quota、仅存活于单 worker 的内存键值存储，不接触磁盘；`capsid:stdio` 只把
获准的 stdout/stderr 消息送入有界 IPC 日志事件，不暴露真实 fd 或 stdin。
`capsid:fs` 只提供经 path rule、Landlock 与 `openat2` 三重约束的有界读取，
拒绝所有 symlink 和写操作。
每个模块仍需宿主显式授权；未列入机器清单可用集合的扩展保持不可用。具体契约见
[宿主能力策略](capability-policy.md)和
[Linux 严格沙箱](linux-sandbox.md)。

## 资源策略

- 单个 Wasm linear memory 最多 256 pages（16 MiB）；
- 单个 Wasm table 最多 1024 elements；
- IPC frame、headers、bundle、并发请求、队列和逐请求缓冲均有显式上限；
- `capsid:stdio` 单条消息最多 16 KiB，且受同一 IPC queue 上限约束；
- `capsid:fs` 单文件最多 1 MiB、单次目录枚举最多 1024 项；
- request/response body 使用逐 request ID 的 credit window；
- 同步 CPU timeout 会令 worker 不再可复用；异步 timeout 只取消对应请求；
- 销毁按 graceful shutdown → SIGTERM → SIGKILL 有界升级。

部署者还可配置 JS heap、进程地址空间、fd、cgroup CPU/内存/PID、出站 body
大小和网络策略。宿主负责根据工作负载公布实际资源与隔离策略。

## vendor 更新原则

`vendor/txiki.js` 与递归 submodule 必须固定版本且保持 clean。升级时必须：

1. 更新固定版本并重新生成 overlay；
2. 逐个验证 patch 可应用；
3. 审查 native module、全局对象和最终二进制差异；
4. 运行完整 contract、WPT、framework、sandbox 与负控矩阵；
5. 更新合规偏差和升级报告。

overlay key、stamp、实际内容 manifest 和 configure dependencies 均采用
fail-closed 校验，不能复用来源不明或被篡改的构建树。
