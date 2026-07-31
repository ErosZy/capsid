# Hono 参考应用

本目录固定 Capsid Runtime 兼容套件使用的 Hono 版本：

```sh
npm ci --ignore-scripts
```

`src/app.js` 同时用于 Node `app.request()` reference 和真实 worker bundle。
三个入口覆盖允许的 export 形式。CMake/esbuild 为每个入口生成一个自包含 ESM；
应用运行时不会从本目录 import。

验证范围、排除项和测试命令见
[`../../docs/framework-compatibility/hono.md`](../../docs/framework-compatibility/hono.md)。
