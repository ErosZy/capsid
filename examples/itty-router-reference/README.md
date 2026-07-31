# itty-router 参考应用

本目录固定 Capsid Runtime 兼容套件使用的 itty-router 版本：

```sh
npm ci --ignore-scripts
npm run build
```

`src/shared-handlers.js` 同时用于 Node reference controller 和真实 worker
bundle。三个入口覆盖：

- 默认 `AutoRouter`；
- `Router` 的默认 `{ fetch: router.fetch }`；
- 手工 `IttyRouter` promise pipeline 的 named `fetch`。

CMake 使用同一 `build.mjs`，并在 worker 测试加载前审计每个自包含 ESM。
验证范围、排除项和差分规则见
[`../../docs/framework-compatibility/itty-router.md`](../../docs/framework-compatibility/itty-router.md)。
