# Capsid Runtime

[![Testing validity](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml/badge.svg)](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Capsid 是面向 HTTP 网关、应用服务器与 worker pool 的进程隔离 JavaScript
运行时。宿主通过 `libcapsid_runtime` 把 HTTP 请求转换为 Fetch 请求；每个
`capsid-worker` 只加载一个自包含 ESM 应用，并以流式事件返回响应。

它不是 Node/Deno 的替代品，也不是直接运行 `.js` 的命令行服务器。Runtime
不监听端口、不终止 TLS、不管理路由或 worker pool；这些职责属于嵌入它的宿主。
仓库同时提供第一方 `capsid-host`，用于开发、集成和基准验证。

> **项目状态：** 当前版本为 `0.1.x`。Runtime ABI 为 v7，第一方 Host 仍是
> 非生产部署接口。Linux strict sandbox 是 v1 的生产隔离目标；未启用严格沙箱
> 时，只能运行受信任代码。

## 为什么是 Capsid

- **进程级故障边界**：应用独占 worker，崩溃、超时和回收由宿主控制。
- **Fetch 原生应用模型**：支持原生 handler，以及打包后的 Hono、itty-router
  和 H3 v2 应用。
- **最小权限能力面**：模块、文件、环境变量、存储、stdio 和出站网络均显式授权。
- **宿主拥有数据面**：C ABI、C++11 RAII、非阻塞 IPC、credit 背压、取消、
  streaming 与审计事件可接入现有 reactor。
- **可复核的正确性**：固定 WPT revision、框架差分、sanitizer、fuzz、沙箱门禁
  和带身份的 benchmark artifact。

```text
客户端 ──HTTP/TLS──▶ 宿主网关 / capsid-host
                         │ libcapsid_runtime / FetchRPC
                         ├── capsid-worker：应用 A
                         ├── capsid-worker：应用 A
                         └── capsid-worker：应用 B
```

Capsid 实现固定的 Minimum Common Web API 子集
`CAPSID-MIN-2025-subset-v0`，不宣称完整 ECMA-429 或全部 WPT conformance。

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

需要 CMake、C/C++ 工具链、Node.js/npm、OpenSSL，以及构建 Host 所需的 Boost。

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

`capsid-host` 还提供固定 worker 池的 `static-pool` 模式，以及由 `host.json`、
Admin API 和 durable active state 驱动的 `managed` 模式。生产集成应先阅读
[宿主嵌入规范](docs/host-integration.md)与[配置参考](docs/host-config.md)。

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

`capsid_worker_fd()` 返回非阻塞 Unix socket，可接入 epoll、kqueue 或现有
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

## 权限与安全边界

默认配置遵循最小权限：没有 capability policy 时不能导入 `capsid:*` 模块，
`egress_policy == NULL` 时出站 Fetch 全部拒绝。但 `strict_sandbox` 默认是关闭的，
所以默认配置仅适合开发与受信任代码，不能作为不可信代码的生产隔离方案。

授权由三层门禁共同决定：能力必须构建进 restricted runtime；模块必须在
`allowed_modules` 中；具体资源必须命中 allow rule 且未命中 deny rule。
Host 上限与应用 `capsid.json` 申请取交集，应用不能扩大 Host 权限。

当前公共模块如下；每一个都必须显式授权：

| 类别 | 模块 |
| --- | --- |
| 受策略约束 | `capsid:env`、`capsid:fs`、`capsid:stdio`、`capsid:storage`、`capsid:system` |
| 权限查询 | `capsid:permissions` |
| 纯工具 | `capsid:assert`、`capsid:getopts`、`capsid:hashing`、`capsid:ipaddr`、`capsid:utils`、`capsid:uuid` |

### `tjs:*` 不能通过配置开放

应用只能导入 `capsid:*` 公共模块；`tjs:*` 与 `tjs:internal/*` 永久禁止。全局
`fetch()` 则由目标域名、端口和必要 IP/CIDR 的出站规则控制，每次 DNS 解析和
redirect 都会重新检查。权威能力清单见
[`capability-manifest.json`](docs/capability-manifest.json)。

Linux 生产环境必须显式启用 strict sandbox，并根据部署要求验证实际结果：
`CAPSID_EVENT_READY.flags` 必须包含部署要求的 sandbox feature。cgroup、network
namespace/firewall、网关侧请求限制与日志持久化仍由宿主负责。详见
[Linux 严格沙箱](docs/linux-sandbox.md)与[安全策略](SECURITY.md)。

## 文档

| 目标 | 文档 |
| --- | --- |
| 了解系统边界 | [架构与产品边界](docs/architecture.md) |
| 嵌入 C/C++ 宿主 | [宿主嵌入与集成规范](docs/host-integration.md) |
| 部署第一方 Host | [host.json / capsid.json 参考](docs/host-config.md) |
| 配置应用权限 | [capsid.json 教程](docs/capsid-json.md) · [模块与权限参考](docs/module-permissions.md) |
| 验证安全隔离 | [能力策略](docs/capability-policy.md) · [Linux 严格沙箱](docs/linux-sandbox.md) |
| 了解兼容性 | [标准与合规](docs/conformance.md) · [框架兼容性](docs/framework-compatibility/README.md) |
| 复现测试与性能 | [测试门禁](docs/testing.md) · [性能证据](docs/performance-benchmarks.md) |

完整导航及事实来源优先级见[文档中心](docs/README.md)。

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

未配置固定 WPT checkout 时，CTest 会登记失败哨兵，避免合规测试静默空跑。
sanitizer、fuzz、WPT 与 delegated sandbox 的完整命令见[测试门禁](docs/testing.md)。

提交代码或文档前，请阅读[贡献指南](CONTRIBUTING.md)。性能数字只在满足证据规则
时进入项目文档，最新结果与原始样本位置见[性能证据](docs/performance-benchmarks.md)。

## License

[MIT](LICENSE) © Capsid contributors
