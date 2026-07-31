# H3 v2 参考应用

本目录固定 Capsid Runtime 兼容套件使用的 H3 版本。安装依赖并生成自包含 ESM：

```sh
npm ci --ignore-scripts
npm run build
```

reference controller 与真实 worker bundle 共享应用逻辑。构建产物仅供测试；
CMake 还会审计 external/dynamic import、Node/server adapter、platform global
和大小边界。

验证范围、排除项和测试命令见
[`../../docs/framework-compatibility/h3-v2.md`](../../docs/framework-compatibility/h3-v2.md)。
