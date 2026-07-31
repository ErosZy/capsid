# 框架兼容性

框架以普通、自包含 ESM 应用运行，不属于 Capsid Web API profile，也不会进入 runtime
ABI 或 native capability policy。运行时没有框架探测、专用分支或修改后的 npm
源码。

| 框架 | 固定版本 | 差分规模 | 说明 |
| --- | --- | ---: | --- |
| [Hono](hono.md) | 4.12.32 | 68 vectors | 核心路由、中间件、streaming 与生命周期 |
| [itty-router](itty-router.md) | 5.0.24 | 96 vectors × 3 variants | AutoRouter、Router、IttyRouter |
| [H3 v2](h3-v2.md) | 2.0.1-rc.26 | 129 vectors | Core、middleware/hooks、部分 Web-standard utilities |

共同验证路径：

```text
spawn worker
  → LOAD_BUNDLE（单个已审计 ESM）
  → READY
  → FetchRPC request/credit
  → exported fetch(Request)
  → FetchRPC response/credit
```

reference 与真实 worker 的差分只是证据的一部分。每个含 `expect` 的向量还有
独立绝对断言，避免 reference 与 runtime 同时出错仍显示通过。

共同排除 Node/Deno/Bun/Cloudflare adapter、server/listener、文件系统静态服务、
WebSocket server、外部/远程/`file:` import 和 provider ambient binding。
这些边界由单独的 expected-rejection 测试维护，不算框架核心不兼容。
