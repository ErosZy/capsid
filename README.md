# Capsid Runtime

[![Testing validity](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml/badge.svg)](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml)
[![Release](https://img.shields.io/github/v/release/ErosZy/capsid?label=release)](https://github.com/ErosZy/capsid/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Capsid 是面向 HTTP 网关、应用服务器与 worker pool 的**进程隔离 JavaScript
运行时**。宿主通过 `libcapsid_runtime` 把 HTTP 请求转换为 Fetch 请求；每个
`capsid-worker` 只加载一个自包含 ESM 应用，并以流式事件返回响应。

它不是 Node/Deno 的替代品，也不是直接运行 `.js` 的命令行服务器。Runtime
不监听端口、不终止 TLS、不管理路由或 worker pool；这些职责属于嵌入它的宿主。
仓库同时提供第一方 `capsid-host`，用于开发、集成和基准验证。

> **项目状态：** 当前版本为 `0.1.x`。Runtime ABI 为 v7，第一方 Host 仍是
> 非生产部署接口。Linux strict sandbox 是 v1 的生产隔离目标；未启用严格沙箱
> 时，只能运行受信任代码。

---

## 目录

- [平台支持](#平台支持)
- [为什么是 Capsid](#为什么是-capsid)
- [快速开始](#快速开始)
- [集成模型](#集成模型)
- [权限与安全边界](#权限与安全边界)
- [性能概览](#性能概览)
- [文档导航](#文档导航)
- [开发与验证](#开发与验证)
- [贡献、安全与许可证](#贡献安全与许可证)

---

## 平台支持

平台支持分为**原生开发**与**生产隔离**两个独立承诺：能在某平台构建和运行，
不等于该平台具备 Linux strict sandbox 同等的生产隔离。三平台差异是本项目
最重要的部署决策因素，请在选型时先阅读[平台支持总览](docs/platform-support.md)。

### Linux（x86-64 / AArch64）

- **原生开发**：完整 ✅
- **生产隔离**：完整 ✅ —— 唯一具备 strict sandbox（seccomp、Landlock、
  namespace、cgroup）的平台，是 v1 的生产发布目标
- 详见 [Linux 严格沙箱](docs/linux-sandbox.md)

### macOS

- **原生开发**：完整 ✅ —— Runtime、worker、字节码编译器与
  single-worker/static-pool Host 可用（single 与 multi shard 行为与 Linux
  一致，multi shard 使用 SO_REUSEPORT）
- **生产隔离**：无 ❌ —— 没有等价隔离，生产请使用 Linux 容器或 VM
- `capsid:fs` 可用（与 Linux 相同的 no-symlink 读取语义）；`strict_sandbox`
  不可用，`--mode managed` 运行时直接失败并提示（与 Windows 一致）

### Windows（x86-64，MSVC）

- **原生开发**：自 v0.1.2 起 ✅ —— Runtime、worker、字节码编译器与
  single-worker/static-pool Host 可用；static-pool 支持 single 与 multi
  shard（multi shard 由池级共享 acceptor 分发，与 Linux/macOS 对外行为一致）
- **生产隔离**：无 ❌ —— 生产请使用 Linux 容器或 WSL2
- managed Host、`capsid:fs` 与 strict sandbox 不可用
- 详见 [Windows 构建与平台能力](docs/windows.md)

**统一结论：**

- **single-worker 与 static-pool（single/multi shard）是三平台统一行为**：
  Linux、macOS 使用内核 SO_REUSEPORT 共享端口，Windows 使用池级共享
  acceptor 轮询分发，对客户端和测试都是同一公开端口、同一 READY/请求/
  drain 契约；
- 开发、联调、benchmark：三平台均可；
- 运行不可信代码的生产隔离：只承诺 Linux；
- macOS/Windows 上的生产一致性：在 Linux 容器或 VM 中运行。

完整能力矩阵、构建前置条件和测试覆盖差异见
[平台支持总览](docs/platform-support.md)。

---

## 为什么是 Capsid

- **AI 生成服务端代码的受控执行**：把 AI 生成的 Fetch handler 视为不可信输入，
  放入独立 worker，并以能力白名单、Linux strict sandbox、资源上限和审计事件约束行为。
- **进程级故障边界**：应用独占 worker，崩溃、超时和回收由宿主控制。
- **Fetch 原生应用模型**：支持原生 handler，以及可打包为自包含 ESM、以 Fetch
  handler 为入口的轻量 Web 框架；Hono、itty-router 和 H3 v2 已通过兼容性验证。
- **最小权限能力面**：模块、文件、环境变量、存储、stdio 和出站网络均显式授权。
- **宿主拥有数据面**：C ABI、C++11 RAII、非阻塞 IPC、credit 背压、取消、
  streaming 与审计事件可接入现有 reactor。
- **高吞吐 Fetch 执行**：当前 4 核基准测试中，2 个 workers 每秒完成约
  **6,800 个 JSON 请求**，约为同机 Flask 的 **1.5 倍**、Slim 的 **3.7 倍**。
- **低常驻资源**：同一环境下，Host + 2 workers 空闲 PSS 为 **12.3 MB**，
  适合需要进程隔离又关注部署密度的场景。
- **快速冷启动**：小型 bundle 冷启动约 **8–10 ms**；约 1 MB bundle 使用可信
  字节码约 **42 ms**。
- **可复核的正确性**：固定 WPT revision、框架差分、sanitizer、fuzz、沙箱门禁
  和带身份的 benchmark artifact。

测试环境、对照组和限制见[性能概览](#性能概览)。

```text
客户端 ──HTTP/TLS──▶ 宿主网关 / capsid-host
                         │ libcapsid_runtime / FetchRPC
                         ├── capsid-worker：应用 A
                         ├── capsid-worker：应用 A
                         └── capsid-worker：应用 B
```

在 AI 代码执行场景中，推荐流程是：生成 Fetch handler → 打包为自包含 ESM →
由宿主固定权限与资源上限 → worker 在 strict sandbox 中加载 → 验证 READY 后进入调度。
生成代码不能自行申请权限，也不能直接访问 Node 内置模块、进程、raw socket 或任意
文件；它只能使用明确授权的环境变量、文件、存储、stdio 和出站目标。协议错误、超时
或崩溃时，宿主可摘除并替换对应 worker。Capsid 提供的是可审计、可回收的纵深执行
边界，不是对任意生成代码“绝对安全”的承诺。

Capsid 实现固定的 Minimum Common Web API 子集
`CAPSID-MIN-2025-subset-v0`，不宣称完整 ECMA-429 或全部 WPT conformance。

---

## 快速开始

### 1. 编写 Fetch 应用

```js
// app.js
export default {
  async fetch(request) {
    return Response.json({
      message: "hello from Capsid",
      path: new URL(request.url).pathname,
    });
  },
};
```

### 2. 构建 Runtime 与第一方 Host

前置依赖：CMake 3.18+、C/C++ 工具链、Node.js/npm、OpenSSL；构建 Host 还需要
Boost。各平台差异见[平台支持总览](docs/platform-support.md)。

<details>
<summary><b>Linux / macOS</b></summary>

```sh
git submodule update --init --recursive
npm ci --ignore-scripts --prefix vendor/txiki.js

cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DCAPSID_BUILD_HOST=ON \
  -DCAPSID_ENABLE_LTO=ON \
  -DCAPSID_USE_MIMALLOC=OFF
cmake --build build-release --parallel
```

</details>

<details>
<summary><b>Windows（PowerShell + MSVC + vcpkg）</b></summary>

```powershell
git submodule update --init --recursive
npm ci --ignore-scripts --prefix vendor/txiki.js

vcpkg install openssl boost-system boost-asio boost-beast --triplet x64-windows-static

cmake -S . -B build-release -G Ninja `
  -DCMAKE_C_COMPILER=cl `
  -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_BUILD_TYPE=Release `
  -DCAPSID_BUILD_HOST=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static

cmake --build build-release --parallel
```

完整前置条件、不可用配置与 LTO/静态 CRT 说明见
[Windows 构建与平台能力](docs/windows.md)。

</details>

### 3. 打包并运行

应用及依赖必须在发布阶段打成单个 ESM；运行时不会从磁盘、URL 或 npm 解析依赖。

```sh
vendor/txiki.js/node_modules/.bin/esbuild app.js \
  --bundle \
  --format=esm \
  --platform=neutral \
  --target=esnext \
  --outfile=app.bundle.js

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

另一个终端中请求：

```sh
curl http://127.0.0.1:8080/@capsid/orders/
# {"message":"hello from Capsid","path":"/"}
```

`capsid-host` 还提供固定 worker 池的 `static-pool` 模式（三平台均支持
single/multi shard；Windows 通过池级共享 acceptor 实现），以及由 `host.json`、Admin API 和 durable active state 驱动的 `managed` 模式
（仅 Linux）。本地 `./capsid.json` 可直接授权 single-worker / static-pool 的
模块、env、fs、fetch、storage 与 stdio，详见 [capsid.json 教程](docs/capsid-json.md)。

生产集成应先阅读[宿主嵌入规范](docs/host-integration.md)与
[配置参考](docs/host-config.md)。

---

## 集成模型

生产宿主通常链接 `libcapsid_runtime`，并自行管理 listener、TLS、路由、worker
池和发布生命周期：

```c
#include <capsid/runtime.h>

capsid_worker_config config;
capsid_worker_config_init(&config);
config.worker_path = "/opt/capsid/bin/capsid-worker";
config.request_timeout_ms = 5000;

capsid_worker *worker = NULL;
capsid_result result = capsid_worker_spawn(&config, &worker);
```

`capsid_worker_fd()` 在 POSIX 上返回非阻塞 Unix socket，在 Windows 上返回
CRT fd（底层为 loopback TCP socket），可接入 epoll、kqueue、IOCP 或现有
reactor。`CAPSID_WOULD_BLOCK` 表示正常背压；事件 payload 只保证有效到下一次
`capsid_worker_next_event()`，异步使用前必须复制。完整的 READY、request credit、
response credit、streaming、取消与回收契约见[宿主嵌入规范](docs/host-integration.md)。

项目安装公开 C 头 `<capsid/runtime.h>` 与 C++11 封装
`<capsid/runtime.hpp>`：

```sh
cmake --install build-release --prefix "$PWD/dist"
```

也可从宿主 CMake 项目直接使用：

```cmake
add_subdirectory(path/to/capsid EXCLUDE_FROM_ALL)
target_link_libraries(my_gateway PRIVATE capsid::runtime)
```

---

## 权限与安全边界

默认配置遵循最小权限：没有 capability policy 时不能导入 `capsid:*` 模块，
`egress_policy == NULL` 时出站 Fetch 全部拒绝。但 `strict_sandbox` 默认是关闭的，
所以默认配置仅适合开发与受信任代码，不能作为不可信代码的生产隔离方案。

授权由三层门禁共同决定：能力必须构建进 restricted runtime；模块必须在
`allowed_modules` 中；具体资源必须命中 allow rule 且未命中 deny rule。
Host 上限与应用 `capsid.json` 申请取交集，应用不能扩大 Host 权限。

当前公共模块如下；每一个都必须显式授权：

- **受策略约束**：`capsid:env`、`capsid:fs`、`capsid:stdio`、
  `capsid:storage`、`capsid:system`
- **权限查询**：`capsid:permissions`
- **纯工具**：`capsid:assert`、`capsid:getopts`、`capsid:hashing`、
  `capsid:ipaddr`、`capsid:utils`、`capsid:uuid`

### `tjs:*` 不能通过配置开放

应用只能导入 `capsid:*` 公共模块；`tjs:*` 与 `tjs:internal/*` 永久禁止。全局
`fetch()` 则由目标域名、端口和必要 IP/CIDR 的出站规则控制，每次 DNS 解析和
redirect 都会重新检查。权威能力清单见
[`capability-manifest.json`](docs/capability-manifest.json)。

Linux 生产环境必须显式启用 strict sandbox，并根据部署要求验证实际结果：
`CAPSID_EVENT_READY.flags` 必须包含部署要求的 sandbox feature。cgroup、network
namespace/firewall、网关侧请求限制与日志持久化仍由宿主负责。macOS/Windows
不提供 strict sandbox，只能运行受信任代码。详见
[Linux 严格沙箱](docs/linux-sandbox.md)、[平台支持总览](docs/platform-support.md)
与[安全策略](SECURITY.md)。

---

## 性能概览

以下是 2026-08-14 在 Ryzen 3300X 4C/8T、Alpine v3.24（WSL2）上的代表性结果。
吞吐测试使用 2 workers、64 connections、12 种负载各 3 轮；冷启动和内存采用各自
独立口径，不能与吞吐数字混合解读。

| 维度 | Capsid 结果 | 对照与说明 |
| --- | ---: | --- |
| JSON 1 KiB 吞吐 | **6,820 QPS** | Python 3 + Flask 4,625；PHP 8 + Slim 1,826 |
| 约 10 kB 冷启动 | **9.5 ms** 源码 / **8.2 ms** 可信字节码 | Node 24：110 ms；Deno 2.9：39 ms |
| 约 1 MB 冷启动 | 141 ms 源码 / **42 ms** 可信字节码 | Node 24：149 ms；Deno 2.9：53 ms |
| 双 worker 空闲 PSS | **12.3 MB** | Host + 2 workers；Python 3 栈为 62.6 MB |

Capsid 在该矩阵的常规 JSON 负载中领先；大字节流并非全部领先，例如
`stream 32k` 低于 Python 3 栈。冷启动的可信字节码只接受由完全相同构建生成并经
宿主校验的产物，不能加载不可信输入。完整的 12 组吞吐结果、PSS/RSS 口径、原始
样本位置和证据门槛见[性能：证据规则与当前形态](docs/performance-benchmarks.md)。

---

## 文档导航

| 目标 | 文档 |
| --- | --- |
| 了解系统边界与进程模型 | [架构与产品边界](docs/architecture.md) |
| 三平台差异与选型 | [平台支持总览](docs/platform-support.md) · [Windows](docs/windows.md) · [Linux 沙箱](docs/linux-sandbox.md) |
| 嵌入 C/C++ 宿主 | [宿主嵌入与集成规范](docs/host-integration.md) |
| 部署第一方 Host | [host.json / capsid.json 参考](docs/host-config.md) |
| 配置应用权限 | [capsid.json 教程](docs/capsid-json.md) · [模块与权限参考](docs/module-permissions.md) |
| 验证安全隔离 | [能力策略](docs/capability-policy.md) · [Linux 严格沙箱](docs/linux-sandbox.md) |
| 了解兼容性 | [标准与合规](docs/conformance.md) · [框架兼容性](docs/framework-compatibility/README.md) |
| 复现测试与性能 | [测试门禁](docs/testing.md) · [性能证据](docs/performance-benchmarks.md) |

完整导航及事实来源优先级见[文档中心](docs/README.md)。

---

## 开发与验证

初始化三个框架 fixture 后运行 Release 测试：

```sh
for directory in \
  examples/hono-reference \
  examples/itty-router-reference \
  examples/h3-v2-reference
do
  npm ci --ignore-scripts --prefix "$directory"
done

cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DCAPSID_BUILD_HOST=ON \
  -DCAPSID_ENABLE_LTO=ON \
  -DCAPSID_USE_MIMALLOC=OFF
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

Windows 构建请使用 MSVC + vcpkg 命令（见[快速开始](#快速开始)），并以
`ctest --test-dir build-release --output-on-failure -E '^(wpt_conformance_not_configured|worker_package_.*)$'`
运行平台中立矩阵。

未配置固定 WPT checkout 时，CTest 会登记失败哨兵，避免合规测试静默空跑。
sanitizer、fuzz、WPT 与 delegated sandbox 的完整命令见[测试门禁](docs/testing.md)。

提交代码或文档前，请阅读[贡献指南](CONTRIBUTING.md)。性能数字只在满足证据规则
时进入项目文档，最新结果与原始样本位置见[性能证据](docs/performance-benchmarks.md)。

---

## 贡献、安全与许可证

- **贡献**：见 [CONTRIBUTING.md](CONTRIBUTING.md)。
- **安全报告**：见 [SECURITY.md](SECURITY.md)；安全漏洞请勿公开披露。
- **许可证**：[MIT](LICENSE) © Capsid contributors
