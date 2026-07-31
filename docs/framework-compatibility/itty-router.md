# itty-router 兼容性

## 状态

Capsid Runtime 验证固定的 **itty-router 5.0.24**。版本和 artifact integrity
由 `examples/itty-router-reference/package.json` 与 lockfile 固定，不自动
覆盖其他 v5 或未来版本。

AutoRouter、Router 和手工 IttyRouter pipeline 分别构建为单文件、零 external
import ESM。构建审计拒绝 source map、Node built-in、file URL、dynamic
import、`require`、`globalThis.tjs` 和 platform-global 定义；每个 bundle
必须小于 96 KiB。

## 构建与验证

```sh
npm ci --ignore-scripts --prefix vendor/txiki.js
npm ci --ignore-scripts --prefix examples/itty-router-reference

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --target \
  test-itty-router-worker-driver test-module-denial
ctest --test-dir build -L itty-router --output-on-failure
```

96 个确定性 request vector 分别对未修改的 Node reference 和真实 worker
执行；同一向量集独立覆盖三种 router variant，并带独立绝对断言。

## 已验证

- 默认 router、`{ fetch }`、named `fetch` 与手工 pipeline；
- GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS/all/PURGE；
- fixed/named/optional/file/wildcard/greedy route、base 与优先级；
- 单值、空值、编码和重复 query，以及跨请求隔离；
- linear handler 和 before/route/catch/finally 顺序；
- JSON/text/urlencoded/multipart/File、body clone 和跨 credit upload；
- JSON/text/HTML/image/binary/Blob/stream、204/304、status/header；
- CORS、嵌套路由、request context；
- direct fetch、并发、三类 cancel、异步/CPU timeout 与复用；
- process、Buffer、Deno、Bun 和 `globalThis.tjs` 继续缺席。

差分比较 status、排序后的 lowercase header、精确 body、params/query、
middleware trace、error classification 和 CORS。`Date` 可归一化；stream
transport chunk 数不比较，但总字节和 checksum 必须一致。

## 支持与排除

应用可以把已验证的 v5 router variant 和 Web-standard helper 打包成一个 ESM。
它不能依赖 provider ambient binding。

排除 Cloudflare binding/ExecutionContext/cache/Durable Object、Node/Bun server
adapter、WebSocket server/upgrade、文件系统静态服务、external/remote/file
module、process/Worker 和 txiki 私有 API。七个 `itty-router-excluded` 测试
验证这些边界。

## 升级流程

更新精确依赖和 lockfile 后，重建/审计三个入口，并运行 differential、
lifecycle、excluded-import、global-surface、P0 和 sanitizer 矩阵。向量或
排除变化必须显式审查。
