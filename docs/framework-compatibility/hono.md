# Hono 兼容性

## 状态

Capsid Runtime 将固定的 **Hono 4.12.32** 作为普通 bundle 验证。精确版本和
完整性由 `examples/hono-reference/package.json` 与 lockfile 固定；该结论
不会自动覆盖其他 4.x 或未来版本。

三个入口 bundle 都必须是单文件 ESM、零 external import，并通过构建审计。
运行时不增加 Hono global、platform adapter 或框架专用分支。

## 构建与验证

```sh
npm ci --ignore-scripts --prefix vendor/txiki.js
npm ci --ignore-scripts --prefix examples/hono-reference

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --target test-hono-worker-driver test-module-denial
ctest --test-dir build -L hono --output-on-failure
```

差分套件包含 68 个确定性向量，其中 11 个有独立绝对断言。reference 侧调用
未修改 Hono，runtime 侧通过真实 `capsid-worker`、FetchRPC 和同一应用逻辑。

## 已验证

- `app.fetch()`、默认 `{ fetch }` 和 named `fetch` 入口；
- method/path routing、params/query、404/405、base path；
- middleware 顺序、context、header/cookie、异常处理；
- JSON/text/HTML/binary/streaming response；
- request body、FormData、AbortSignal；
- 并发隔离、handler/body/response-stream cancel；
- 异步 timeout、同步 CPU timeout 和 worker 复用；
- 由正常 egress policy 控制的 txiki.js direct `fetch()`；
- Node/Deno/Bun/txiki/platform global 继续缺席。

response 比较 status、规范化 header 和精确 body bytes；只归一化动态 `Date`
等明确定义的非语义字段。stream transport chunk 边界不属于 Web Streams
语义，因此比较总长度和内容，不比较底层 chunk 次数。

## 支持边界

应用可以把上述 Web-standard Hono Core 路径打包成一个 ESM，并导出 Capsid
Runtime 的正常 fetch contract。宿主仍负责 HTTP/TLS、worker 池、timeout、
网络和 sandbox 策略。

明确排除：

- `@hono/node-server`、Node built-in 与 Node/Bun/Deno/Cloudflare adapter；
- HTTP/WebSocket server 和 upgrade API；
- 依赖文件系统的 static-file adapter；
- Cloudflare binding、`ExecutionContext`、cache、Durable Object；
- context storage/`AsyncLocalStorage`；
- external、remote 和 `file:` module loading。

对应 negative tests 使用 `hono-excluded` 标签。产品能力变化不会自动扩大
这里的固定版本兼容声明。

## 升级流程

1. 更新精确依赖并重建 lockfile；
2. 不修改 `node_modules` 或 Hono 源码；
3. 重建并审计全部 bundle；
4. 运行 differential、lifecycle、excluded-import、global-surface 和
   sanitizer 矩阵；
5. 将 normalization 或 exclusion 变化作为兼容策略变更审查。
