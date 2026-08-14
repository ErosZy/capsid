# Capsid Runtime

Capsid 是给 HTTP 网关、应用服务器和 worker pool 嵌入的进程隔离 JavaScript
运行时。一个 `capsid-worker` 只加载一个自包含 ESM 应用；宿主链接
`libcapsid_runtime`，把 HTTP 请求转换成 Fetch 请求，并以流式事件接收响应。

Capsid **不是** Node/Deno 替代品，也不是一个直接运行 `.js` 文件的命令行
服务器。`libcapsid_runtime` 不监听端口、不终止 TLS，也不管理路由或 worker pool；
部署方可以自行嵌入它。

仓库中的第一方 C++ Host（`capsid-host`）实现了这些宿主职责，支持三种运行模式：

- `--mode single-worker`：一个 worker 进程，单 listener，用于嵌入与开发；
- `--mode static-pool`：固定大小 worker 池（1/2/4/6/8），SO_REUSEPORT 共享端口，
  每个 shard 独立 reactor 线程，线性扩展；
- `--mode managed`：生产部署闭环 —— host.json 权威配置、Admin API
  （Unix socket + peer 凭据授权）、容量配额、durable active-state 持久化、
  崩溃恢复（SIGKILL 矩阵验证）、背压 admission gates（429/503/504）、
  SSE 流式 permit 与 slow-client write deadline。

```text
客户端
  │ HTTP/TLS
  ▼
宿主网关或应用服务器
  │ libcapsid_runtime / FetchRPC
  ├── capsid-worker：应用 A
  ├── capsid-worker：应用 A
  └── capsid-worker：应用 B
```

Capsid 实现固定的 Minimum Common Web API 子集
`CAPSID-MIN-2025-subset-v0`。合规基线是 ECMA-429 和仓库锁定的 WPT revision；
项目不宣称完整 ECMA-429 或全部 WPT conformance。

## 快速开始

### 1. 编写应用

应用必须导出默认对象的 `fetch()`，或导出名为 `fetch` 的函数：

```js
// app.js
export default {
  async fetch(request) {
    const url = new URL(request.url);
    return Response.json({
      message: "hello from Capsid",
      path: url.pathname,
    });
  },
};
```

### 2. 打包成自包含 ESM bundle

Capsid 只加载一个自包含 ESM bundle。应用运行时不能从磁盘、URL 或 npm
解析依赖，因此框架和依赖必须在发布阶段打包：

```sh
vendor/txiki.js/node_modules/.bin/esbuild app.js \
  --bundle \
  --format=esm \
  --platform=neutral \
  --target=esnext \
  --outfile=app.bundle.js
```

### 3. 运行

用第一方 Host 直接跑起来（`capsid-worker` 与 `capsid-host` 来自
[从源码构建](#从源码构建)）：

```sh
./build-release/capsid-host \
  --mode single-worker \
  --worker ./build-release/capsid-worker \
  --source-bundle app.bundle.js \
  --source-name "file://$PWD/app.bundle.js" \
  --application orders \
  --listen 127.0.0.1:8080 \
  --routing path \
  --public-scheme http
```

请求经 `--routing path` 剥离 `/@capsid/<application>/` 前缀后到达应用：

```sh
curl http://127.0.0.1:8080/@capsid/orders/
# {"message":"hello from Capsid","path":"/"}
```

### 4. 嵌入自己的宿主

生产宿主不直接运行 `capsid-host`，而是链接 `libcapsid_runtime`（C ABI 或
C++11 RAII 封装），自己管理 listener、worker 池与生命周期。见
[在宿主中使用](#在宿主中使用)与[宿主嵌入与集成规范](docs/host-integration.md)。

### 5. managed 模式：JSON 配置驱动

`capsid-host --mode managed --host-config /etc/capsid/host.json` 由
`host.json` 权威配置驱动：整机（listener、全局权限上限、容量、恢复策略）
与每个应用版本目录下的 `capsid.json`（权限申请、worker 资源、池大小）两层
取交集，部署经 Unix socket Admin API（`POST /v1/deploy`）蓝绿发布。完整字段
说明、目录布局、secret 文件与运维命令见
[host.json 与 capsid.json 配置参考](docs/host-config.md)。

## 编写和打包应用

框架必须和依赖一起打包进 bundle。bundle 不应包含 Node/Bun/Deno server
adapter。框架入口示例和已验证范围见：

- [Hono](docs/framework-compatibility/hono.md)
- [itty-router](docs/framework-compatibility/itty-router.md)
- [H3 v2](docs/framework-compatibility/h3-v2.md)

## 在宿主中使用

Capsid 的正常生命周期是：

1. 初始化并填写 `capsid_worker_config`；
2. `capsid_worker_spawn()` 创建 worker；
3. 读取 `app.bundle.js`，调用 `capsid_worker_load_bundle_named()`；
4. 持续取事件，等待 `CAPSID_EVENT_READY`；
5. 用非零且未复用的 request ID 发送请求；
6. 按 credit 写请求 body，并按下游实际消费归还响应 credit；
7. 持续处理响应、日志、审计、超时和退出事件；
8. 摘除 worker，执行 shutdown、排空事件并 destroy。

最小启动代码如下。它只演示创建和加载；生产宿主还必须实现非阻塞事件循环和
完整事件处理：

```c
#include <capsid/runtime.h>

capsid_worker_config config;
capsid_worker_config_init(&config);
config.worker_path = "/opt/capsid/bin/capsid-worker";
config.request_timeout_ms = 5000;

capsid_worker *worker = NULL;
capsid_result result = capsid_worker_spawn(&config, &worker);
if (result != CAPSID_OK) {
    /* 记录 capsid_result_string(result)，本次启动失败 */
}

result = capsid_worker_load_bundle_named(
    worker,
    bundle_data,
    bundle_size,
    "app.bundle.js"
);
```

`capsid_worker_fd()` 返回非阻塞 Unix socket，可接入 epoll、kqueue 或宿主自己的
reactor。`CAPSID_WOULD_BLOCK` 是正常背压信号，不是 worker 故障。一个 worker
handle 同一时刻应只由一个宿主线程驱动。

这是当前 ABI v7 的 POSIX 集成面，不是永久的跨平台限制。项目的平台
契约是 Linux 作为 v1 生产 strict-sandbox 目标，macOS 和 Windows 提供
原生开发路径；Windows process/transport/event-source 将在具备真实 Windows
机器或 hosted runner 后实现，当前尚未交付。未隔离的 native-dev mode 只允许
loopback，不得用于不可信代码
的生产执行。详见[平台契约](docs/architecture.md#平台契约)。

请求和响应的关键顺序：

```text
begin_request
  → 收到 REQUEST_CREDIT 后 write_request（可多次）
  → end_request
  → RESPONSE_HEAD
  → RESPONSE_BODY（消费后再 grant_response_credit）
  → RESPONSE_END
```

事件 payload、响应 header 和解码后的 audit view 只保证有效到同一 worker
下一次 `capsid_worker_next_event()`。需要跨线程、排队或异步写出时必须先复制。
完整集成契约（线程模型、SSE/streaming、ABI 版本策略、上线清单）见
[宿主嵌入与集成规范](docs/host-integration.md)。

## 权限配置

部署时，权限在 `capsid.json`（每个应用版本）里声明，沙箱在 `host.json`
（整机）里声明，两边取交集生效——App 申请不能扩大 Host 上限。JavaScript
不能弹出提示、申请权限或动态扩大策略。授权依次经过三道门：

1. 能力是否实际构建进 restricted runtime；
2. 模块是否列入 `allowed_modules`；
3. 当前操作和资源是否命中 allow rule，且没有命中 deny rule。

最简权限配置（capsid.json；`apiVersion` 与 `pool` 是必需项，其余字段可选）：

```json
{
  "apiVersion": "capsid/app-v1",
  "pool": { "minReady": 2, "maxWorkers": 2 },
  "permissions": {
    "modules": ["capsid:env", "capsid:fs", "capsid:storage", "capsid:stdio"],
    "env": { "APP_MODE": { "value": "production" } },
    "fs": { "read": { "allow": ["/srv/capsid/config"] } },
    "fetch": { "allow": ["api.example.com:443"] },
    "storage": { "namespaces": ["session"] },
    "stdio": ["stdout", "stderr"]
  }
}
```

### `tjs:*` 不能通过配置开放

应用的公共模块命名空间只有 `capsid:*`。任意 `tjs:*` 和
`tjs:internal/*` 都是永久禁止项，不是可填写到 `allowed_modules` 的权限名称：

```json
{ "permissions": { "modules": ["tjs:assert"] } }    // 错误：部署失败
{ "permissions": { "modules": ["capsid:assert"] } } // 正确：只开放公共包装
```

`capsid:assert`、`capsid:getopts`、`capsid:hashing`、`capsid:ipaddr`、
`capsid:utils` 和 `capsid:uuid` 内部复用 txiki.js 的纯工具实现，但不会开放
`globalThis.tjs` 或底层 `tjs:*` specifier。`capsid:hashing` 内部加载
`tjs:internal/core` 后，应用直接 import 它仍会被拒绝。

当前主要 API 与权限：

| 模块 | JavaScript API | 还需要的操作规则 |
| --- | --- | --- |
| `capsid:env` | `env.get(name)` | 每个键的 `ENV` allow |
| `capsid:fs` | `fs.readText(path)`、`fs.stat(path)`、`fs.list(path)` | 路径范围的 `READ` allow |
| `capsid:stdio` | `stdio.write(stream, message)` | 每个 stream 的 `STDIO` allow |
| `capsid:storage` | `storage.get/set/delete/clear/keys()` | namespace 的 `STORAGE` allow |
| `capsid:system` | `system.get("runtimeVersion")`、`system.get("featureFlags")` | 对应 kind 的 `SYS` allow |
| `capsid:permissions` | `permissions.query(descriptor)` | 无操作 rule，只查询 |
| `capsid:assert`、`capsid:getopts`、`capsid:hashing`、`capsid:ipaddr`、`capsid:utils`、`capsid:uuid` | 纯工具导出 | 无操作 rule，只有模块 gate |

所有模块，包括纯工具模块，都必须逐个放入 `allowed_modules`。不提供
WebSocket、产品 SQLite、readline、文件写入/监听、FFI、raw socket、process
或 HTTP server。权威清单是
[`docs/capability-manifest.json`](docs/capability-manifest.json)。

全局 `fetch()` 是特例：它不需要 `capsid:net` 模块，而是通过
`permissions.fetch.allow` 声明目标（宿主侧上限是 host.json 的
`permissions.fetchTargets`）。hostname、DNS 解析后的每个地址和每次
redirect 都会重新检查；访问 loopback、私网、link-local 或其他受保护地址
时，还必须显式允许对应 IP/CIDR，只有 hostname allow 不够。不要把默认策略
改成 allow 来图省事，按目标域名、端口和必要 CIDR 建立白名单，并让宿主
网络 namespace/firewall 再做一层限制。

应用侧只能使用已授权内容：

```js
import { env } from "capsid:env";
import { fs } from "capsid:fs";
import { stdio } from "capsid:stdio";

export default {
  async fetch() {
    const mode = env.get("APP_MODE");
    const config = JSON.parse(
      fs.readText("/srv/capsid/config/app.json"),
    );
    stdio.write("stdout", `mode=${mode}`);

    const response = await fetch("https://api.example.com/status");
    return Response.json({ config, upstream: await response.json() });
  },
};
```

worker 不继承宿主环境变量：只有 capsid.json `permissions.env` 显式给出且
被环境规则允许的键才能读取。`capsid:fs` 不跟随 symlink，只接受规范化绝对
路径；strict sandbox 下授权根必须已经存在且自身不能是 symlink。

从最小配置开始逐步加字段、secret 用法与常见错误，见
[capsid.json 怎么写（教程）](docs/capsid-json.md)。完整的 module specifier
判定、API→权限映射与 `permissions.query()` descriptor，见
[JavaScript 模块与权限参考](docs/module-permissions.md)。嵌入宿主直接用
C/C++ 构造 `capsid_capability_policy`（含 egress policy 与 `net_policy`
交集语义）见[宿主能力策略](docs/capability-policy.md)。

## 生产安全配置

默认配置在能力层面是最小权限：没有 capability policy 时应用不能导入
`capsid:` 模块，`egress_policy == NULL` 时出站 Fetch 全部拒绝。但
`strict_sandbox` 默认是关闭的，所以默认配置只适合受信任开发和功能验证，
不能直接作为执行不可信代码的生产隔离方案。

Linux x86-64/AArch64 生产环境用 host.json 显式声明隔离与整机上限（这里只
展示沙箱相关字段；`applicationsRoot`、`stateRoot`、`secretRootTemplate` 与
`admin` 是必需项，完整示例见 [host.json 参考](docs/host-config.md)）：

```json
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/applications",
  "stateRoot": "/srv/capsid/state",
  "secretRootTemplate": "/srv/capsid/secrets/{application}",
  "admin": { "unix": "/run/capsid/admin.sock", "mode": "0600" },

  "isolation": {
    "mode": "strict",
    "required": ["cgroup-v2"],
    "cgroupRoot": "/sys/fs/cgroup/capsid"
  },

  "permissions": {
    "modules": ["capsid:env", "capsid:fs", "capsid:storage", "capsid:stdio"],
    "environmentNames": ["APP_MODE"],
    "fsReadRoots": ["/srv/capsid/config"],
    "fetchTargets": ["api.example.com:443"],
    "storageNamespaces": ["session"],
    "stdioStreams": ["stdout", "stderr"]
  },

  "capacity": { "workersTotal": 16 },

  "maximums": {
    "worker": { "memoryMax": "512MiB" },
    "request": { "maxInflightPerWorker": 128 }
  }
}
```

这些值是配置示例，不是所有 workload 的通用最优值。上线前应根据 bundle、
并发数、响应流和压测结果调整，同时保持硬上限。

需要注意：

- `isolation.mode: "strict"` 会要求 rlimit、`no_new_privs`、Landlock 和
  seccomp 全部安装成功；缺任一项就启动失败；
- cgroup 目录必须由宿主预先创建并委派，父 cgroup 还要启用所需 controller；
  Capsid 不替宿主修改 `cgroup.subtree_control`；
- 可进一步要求 user、mount、IPC、UTS namespace；network namespace 必须由
  宿主配置路由和 firewall 后，通过已打开的 fd 传入；
- `worker.memoryMax` 是进程地址空间边界，cgroup memory 是实际资源边界，
  两者用途不同；
- 出站 Fetch body 限制默认值 `0` 表示不增加总量上限，生产配置应显式填写；
- 入站请求总大小、客户端 deadline、连接数和网关缓冲仍由宿主限制；
- 同步 CPU timeout 后应替换 worker；异步 timeout 只取消对应请求；
- `CAPSID_EVENT_READY.flags` 必须包含部署要求的 sandbox feature，不能只根据配置
  假定隔离已经生效。

strict sandbox 会关闭 worker 的真实 stdin/stdout/stderr。应用日志应通过获准的
`capsid:stdio` 形成 `CAPSID_EVENT_LOG`，由宿主执行脱敏、限速和落盘。

capsid.json 的 `worker`/`request` 字段、secret 与部署三步见
[capsid.json 怎么写（教程）](docs/capsid-json.md)；host.json 完整字段与
Admin API 见 [host.json 与 capsid.json 配置参考](docs/host-config.md)。
嵌入宿主以 C API 设置 `strict_sandbox`、`capsid_resource_limits` 与
`CAPSID_SANDBOX_FEATURE_*` 时，完整 Linux 配置、cgroup 回滚语义和
namespace 前置条件见 [Linux 严格沙箱](docs/linux-sandbox.md)。

## 审计与故障处理

能力判断通过 `CAPSID_EVENT_AUDIT` 交给宿主。使用
`capsid_audit_record_decode()` 可取得：

- application identity、worker ID 和 request ID；
- build/module/operation/query 阶段；
- allow、deny、unavailable 或 partial 决策；
- rule ID、规范化资源和 capability manifest SHA-256。

宿主必须持续排空 audit 和 log 事件。它们是有界通道，不能替代无界日志队列。
建议把稳定 rule ID、应用发布 hash、worker binary hash 和 manifest hash 一起
写入发布记录。

worker 出现协议错误、`CAPSID_EVENT_EXIT` 或同步 CPU timeout 后，不应继续复用；
从调度摘除并创建新 worker。正常发布顺序是：

```text
创建新 worker → 加载 bundle → 验证 READY/flags → 健康检查
→ 加入调度 → 摘除旧 worker → 排空请求 → shutdown → destroy
```

可信 bytecode API 只能加载由**完全相同的 Capsid/QuickJS 构建**生成并由宿主
校验摘要的产物。QuickJS bytecode 不是安全输入格式，绝不能加载租户或其他
不可信来源提供的 bytes。

## 适用场景

适合：

- 在已有 C/C++、Go/cgo 或其他原生网关中运行 JavaScript Fetch 应用；
- 每个应用使用独立进程、固定内存和明确能力策略；
- 运行打包后的 Hono、itty-router、H3 v2 或原生 Fetch handler；
- 需要宿主控制背压、取消、审计、cgroup 和 Linux 沙箱。

不适合：

- 需要 Node 内置模块、npm 运行时加载、`process`、WASI 或任意文件访问；
- 需要 runtime 自己监听 HTTP/WebSocket、管理 TLS 或创建 worker pool；
- 希望直接执行远程模块、目录中的源码树或用户上传的 QuickJS bytecode；
- 把未开启 strict sandbox 的开发配置用于执行不可信代码。

## 性能（4 核对照，2026-08-14）

capsid+hono 与 PHP 8+Slim、Python 3+Flask 的三栈对照。环境：Ryzen 3300X
4C/8T、Alpine v3.24（WSL2）；SUT taskset 0-3 / loadgen 4-7、双进程
（capsid 2 workers / gunicorn 2 / php-fpm max_children=2）、conns=64、
12 种负载 × 3 轮、payload 逐字节对齐，33/36 格满足结论门槛（CV≤7%、
0 错误；3 格 CV 超标按观察样本记录）。capsid 侧为产品默认
initial-stream-window 64K。版本：PHP 8.5.8 + Slim 4.15.2 + nginx
1.26.3；Python 3.14.5 + Flask 3.1.3 + Gunicorn 26.0.0。QPS 如下：

| workload | capsid + hono | PHP 8 + Slim | Python 3 + Flask |
|---|---:|---:|---:|
| json 1k | **6820** | 1826 | 4625 |
| json 8k | **5213** | 1727 | 4683 |
| json 16k | **5304** | 1679 | 4495 |
| json 32k | **4558** | 1592 | 3865 |
| bytes 1k | **4591** | 1727 | 4510 |
| bytes 8k | **4405** | 1641 | 4375 |
| bytes 16k | 3971 | 1557 | **4252** |
| bytes 32k | 3414 | 1572 | **3908** |
| stream 1k | **4593** | 1745 | 4442 |
| stream 8k | **3952** | 1708 | 3570 |
| stream 16k | **3501** | 1652 | 3377 |
| stream 32k | 2886 | 1592 | **3756** |

形态：常规 JSON 全胜（json 1k 为 Python 3 栈的 1.47×、PHP 8 栈的
3.74×）；大字节流载荷（bytes ≥16k、stream 32k）Python 3 栈反超，
stream 32k 掉队成因待查。资源形态
（空闲稳态，PSS 中位数）：capsid 3 进程 12.3MB，为 Python 3 栈 62.6MB
的 1/5；PHP 8 栈 RSS 124MB（docker 跨用户 PSS 不可读，口径含 nginx）。
完整方法、样本与结论见[性能：证据规则与当前形态](docs/performance-benchmarks.md)。

冷启动对照（同一 4 核 cpuset，中位数，ms；fixture 为真实形态 JS 源码，
三端加载同一函数体、仅入口不同）：

| 尺寸 | capsid 源码 | capsid 可信字节码 | Node 24 源码 | Deno 2.9 源码 |
|---:|---:|---:|---:|---:|
| 10k | **9.5** | **8.2** | 110 | 39 |
| 100k | **19.6** | **10.6** | 110 | 40 |
| 1M | 141 | **42** | 149 | 53 |

真实形态下编译成本随 AST 节点数放大：1M 源码 capsid 编译 133ms（总
141ms，与 Node 的 137ms 解析同量级）；可信字节码把解析移到部署时，
1M 总耗时降到 42ms（比 Deno 快 21%、比 Node 快 3.6×）。小 bundle 则
启动基数主导：10k 时 capsid 源码 9.5ms，是 Deno 的 1/4、Node 的 1/11。
方法细节见性能文档冷启动节。

## 从源码构建

需要 CMake、C/C++ 工具链、Node.js/npm，以及 txiki.js 所需的系统开发库。
先取得锁定依赖：

```sh
git submodule update --init --recursive
npm ci --ignore-scripts --prefix vendor/txiki.js
```

构建 Release/LTO 产物：

```sh
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DCAPSID_ENABLE_LTO=ON \
  -DCAPSID_USE_MIMALLOC=OFF
cmake --build build-release --parallel
```

主要产物是：

- `build-release/capsid-worker`：隔离的 JavaScript 子进程；
- `build-release/capsid-host`：第一方 C++ Host（single-worker / static-pool /
  managed 三种模式，`--mode` 选择）；
- `build-release/libcapsid_runtime.a`：宿主链接的静态库；
- `include/capsid/runtime.h`：C ABI；
- `include/capsid/runtime.hpp`：C++11 RAII 和策略构造器。

`capsid-host` 依赖系统 Boost.Asio/Beast（`libboost-system-dev`）；无 Boost 的
平台跳过 host 目标但保留 worker 与库。

安装到独立目录：

```sh
cmake --install build-release --prefix "$PWD/dist"
```

安装结果包含 `dist/bin/capsid-worker`、静态库、公开头文件和 capability
manifest。也可以在宿主的 CMake 项目中直接使用：

```cmake
add_subdirectory(path/to/capsid EXCLUDE_FROM_ALL)
target_link_libraries(my_gateway PRIVATE capsid::runtime)
```

当前 ABI 版本是 7。代码应包含 `<capsid/runtime.h>` 或
`<capsid/runtime.hpp>`，使用 `capsid_*` C 符号、`capsid::` C++ 命名空间和
`CAPSID_*` 构建变量。此次产品统一不提供旧名称兼容别名。

## 验证当前 checkout

初始化参考框架依赖：

```sh
npm ci --ignore-scripts --prefix examples/hono-reference
npm ci --ignore-scripts --prefix examples/itty-router-reference
npm ci --ignore-scripts --prefix examples/h3-v2-reference
```

运行 Release 测试：

```sh
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DCAPSID_ENABLE_LTO=ON \
  -DCAPSID_USE_MIMALLOC=OFF
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

未配置固定 WPT checkout 时，CTest 会登记失败哨兵，防止合规测试静默空跑。
完整测试说明见[测试与持续门禁](docs/testing.md)。测试数量由当前构建生成，不在说明
文档中维护易过期的固定计数。

## 继续阅读

- [文档导航](docs/README.md)
- [架构与产品边界](docs/architecture.md)
- [宿主嵌入与集成规范](docs/host-integration.md)
- [第一方 Host v1 详细设计](docs/host-technical-design-review.md)
- [宿主能力策略](docs/capability-policy.md)
- [JavaScript 模块与权限参考](docs/module-permissions.md)
- [Linux 严格沙箱](docs/linux-sandbox.md)
- [标准与合规](docs/conformance.md)
- [性能：证据规则与当前形态](docs/performance-benchmarks.md)
