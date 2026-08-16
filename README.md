# Capsid

[![Testing validity](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml/badge.svg)](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml)
[![Release](https://img.shields.io/github/v/release/ErosZy/capsid?label=release)](https://github.com/ErosZy/capsid/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

进程隔离的 JavaScript 运行时：宿主通过 `libcapsid_runtime` 管理
`capsid-worker` 进程，每个 worker 只加载一个自包含 ESM，并通过流式 FetchRPC
服务 HTTP 请求。Runtime 不监听端口、不终止 TLS、不管理路由；这些属于宿主。

> **状态**：`0.1.x`，ABI v7。第一方 `capsid-host` 是开发/benchmark 入口，
> 不是生产部署接口；生产隔离只承诺 Linux strict sandbox。

## 为什么用 Capsid

- 把不可信/AI 生成的 Fetch handler 放进独立 worker，用能力白名单、资源上限
  与审计事件约束行为；
- 进程级故障边界：崩溃、超时、回收由宿主控制；
- 最小权限：模块、env、fs、storage、stdio 与出站网络全部显式授权，默认拒绝；
- 宿主数据面：C ABI / C++11 RAII、非阻塞 IPC、credit 背压、取消与 streaming；
- 高性能：4 核基准下 2 workers 约 **6,800 QPS**，约为同机 Flask 的 1.5 倍、
  Slim 的 3.7 倍；
- 冷启动快：小 bundle 约 **8–10 ms**；约 1 MB bundle 使用可信字节码约 **42 ms**；
- 低常驻：Host + 2 workers 空闲 PSS 约 **12.3 MB**；
- 可复核：固定 WPT、框架差分、sanitizer、fuzz 与带身份的性能证据。

## 快速开始

### 1. 应用

```js
export default {
  async fetch(request) {
    return Response.json({
      message: "hello from Capsid",
      path: new URL(request.url).pathname,
    });
  },
};
```

### 2. 构建

Linux / macOS：

```sh
git submodule update --init --recursive
npm ci --ignore-scripts --prefix vendor/txiki.js
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DCAPSID_BUILD_HOST=ON
cmake --build build-release --parallel
```

Windows（PowerShell + MSVC + vcpkg）：

```powershell
vcpkg install openssl boost-system boost-asio boost-beast --triplet x64-windows-static
cmake -S . -B build-release -G Ninja `
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_BUILD_TYPE=Release -DCAPSID_BUILD_HOST=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-release --parallel
```

### 3. 打包并运行

单机跑通建议显式使用最小权限的 `capsid.json`：文件缺失时是 deny-all 基线，
需要 `capsid:*` 模块或出站 `fetch` 时再逐项加 allow。

```json
// capsid.json
{
  "apiVersion": "capsid/app-v1",
  "permissions": {
    "modules": [],
    "fetch": { "allow": [] }
  },
  "pool": { "minReady": 1, "maxWorkers": 1 }
}
```

```sh
npx esbuild app.js --bundle --format=esm \
  --platform=neutral --target=esnext --outfile=app.bundle.js

./build-release/capsid-host --mode single-worker \
  --worker ./build-release/capsid-worker \
  --source-bundle app.bundle.js \
  --source-name "file://$PWD/app.bundle.js" \
  --application orders --listen 127.0.0.1:8080 \
  --routing path --public-scheme http \
  --capsid-json ./capsid.json
```

```sh
curl http://127.0.0.1:8080/@capsid/orders/
# {"message":"hello from Capsid","path":"/"}
```

`capsid-host` 支持 `single-worker`、`static-pool`、`managed`；权限字段的
逐步配置见 [capsid.json 教程](docs/capsid-json.md)。

## 配置引导

应用权限写 `capsid.json`；`managed` 模式再加一份 Host 权威配置 `host.json`。

```json
// capsid.json —— 应用申请什么能力
{
  "apiVersion": "capsid/app-v1",
  "permissions": {
    "modules": ["capsid:env"],
    "fetch": { "allow": ["api.example.com"] }
  },
  "pool": { "minReady": 1, "maxWorkers": 1 }
}
```

```json
// host.json —— managed 模式：Host 允许什么、数据放哪
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/applications",
  "stateRoot": "/srv/capsid/state",
  "secretRootTemplate": "/srv/capsid/secrets/{application}",
  "admin": { "unix": "/run/capsid/admin.sock", "mode": "0600" }
}
```

`capsid.json` 教程见 [docs/capsid-json.md](docs/capsid-json.md)，`host.json`
字段见 [docs/host-config.md](docs/host-config.md)。

## 集成模型

宿主链接 `libcapsid_runtime`，自行管理 listener、TLS、路由与池生命周期：

```c
#include <capsid/runtime.h>

capsid_worker_config config;
capsid_worker_config_init(&config);
config.worker_path = "/opt/capsid/bin/capsid-worker";
config.request_timeout_ms = 5000;

capsid_worker *worker = NULL;
capsid_result result = capsid_worker_spawn(&config, &worker);
```

安装头文件 `<capsid/runtime.h>` 与 C++11 封装 `<capsid/runtime.hpp>`：

```sh
cmake --install build-release --prefix "$PWD/dist"
```

或嵌入宿主构建：

```cmake
add_subdirectory(path/to/capsid EXCLUDE_FROM_ALL)
target_link_libraries(my_gateway PRIVATE capsid::runtime)
```

完整 READY/credit/streaming/cancel 契约见
[宿主嵌入规范](docs/host-integration.md)。

## 权限与安全

默认最小权限：没有 capability policy 不能导入 `capsid:*` 模块，
`egress_policy == NULL` 时出站 Fetch 全部拒绝。`strict_sandbox` 默认是关闭的，
默认配置只适合受信任代码。

授权三层门禁：构建期能力 → 模块白名单 → 资源 allow/deny 规则；Host 上限与
应用申请取交集。

当前公共模块（每个都需显式授权）：

- 受策略约束：`capsid:env`、`capsid:fs`、`capsid:stdio`、`capsid:storage`、
  `capsid:system`
- 权限查询：`capsid:permissions`
- 纯工具：`capsid:assert`、`capsid:getopts`、`capsid:hashing`、`capsid:ipaddr`、
  `capsid:utils`、`capsid:uuid`

`tjs:*` 不能通过配置开放。Linux 生产环境必须显式启用 strict sandbox 并验证
`CAPSID_EVENT_READY.flags` 必须包含部署要求的 sandbox feature。详见
[Linux 严格沙箱](docs/linux-sandbox.md)、
[能力策略](docs/capability-policy.md)、[安全策略](SECURITY.md)。

## 性能

4 核基准（Ryzen 3300X，Alpine v3.24/WSL2）：

| 维度 | Capsid | 对照 |
| --- | ---: | ---: |
| JSON 1 KiB 吞吐 | **6,820 QPS** | Flask 4,625 · Slim 1,826 |
| 小 bundle 冷启动 | **8–10 ms** | Node 110 ms · Deno 39 ms |
| 1 MB 可信字节码冷启动 | **42 ms** | Node 149 ms · Deno 53 ms |
| Host + 2 workers 空闲 PSS | **12.3 MB** | Python 3 栈 62.6 MB |

完整口径、12 组负载与证据规则见
[performance-benchmarks.md](docs/performance-benchmarks.md)。

## 平台支持

- **Linux**：完整支持。`single-worker` / `static-pool`（多 shard） /
  `managed` 可用；strict sandbox、`capsid:fs` 完整。**生产运行不可信代码只用
  Linux。**
- **macOS**：开发可用。Runtime、worker、字节码编译器与
  single/static-pool Host 可用；`capsid:fs` 有损可用（symlink 拒绝）；
  strict sandbox 与 managed 不可用，`--mode managed` 运行时提示并退出。
- **Windows**：开发可用（MSVC，自 v0.1.2）。Runtime、worker、字节码编译器
  与 single/static-pool Host 可用；多 shard static-pool 由池级 acceptor 分发；
  `capsid:fs` 有损可用（仅 `C:/...` 路径，reparse point 拒绝）；strict
  sandbox 与 managed 不可用，`--mode managed` 运行时提示并退出。

完整矩阵与构建要求见 [docs/platform-support.md](docs/platform-support.md)。

## 文档导航

| 主题 | 入口 |
| --- | --- |
| 架构与边界 | [architecture.md](docs/architecture.md) |
| 平台差异 | [platform-support.md](docs/platform-support.md) · [windows.md](docs/windows.md) |
| 嵌入宿主 | [host-integration.md](docs/host-integration.md) |
| 配置与权限 | [host-config.md](docs/host-config.md) · [capsid-json.md](docs/capsid-json.md) |
| 安全与沙箱 | [capability-policy.md](docs/capability-policy.md) · [linux-sandbox.md](docs/linux-sandbox.md) |
| 兼容性 | [conformance.md](docs/conformance.md) · [framework-compatibility/](docs/framework-compatibility/README.md) |
| 质量与性能 | [testing.md](docs/testing.md) · [performance-benchmarks.md](docs/performance-benchmarks.md) |

完整任务索引见 [docs/README.md](docs/README.md)。

## 开发与验证

```sh
for d in examples/hono-reference examples/itty-router-reference examples/h3-v2-reference; do
  npm ci --ignore-scripts --prefix "$d"
done
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DCAPSID_BUILD_HOST=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

完整 CI 矩阵见 [testing.md](docs/testing.md)；贡献规范见
[CONTRIBUTING.md](CONTRIBUTING.md)。

## License

[MIT](LICENSE) © Capsid contributors
