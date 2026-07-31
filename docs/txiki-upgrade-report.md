# txiki.js 升级门禁报告

> 生成时间：2026-07-30T15:10:28.212266+00:00
> vendor：`v26.6.0` / `1a230d31183f062fae7a6c4fd2cff466cecc1787`

## 结论

门禁通过。vendor、patch、overlay、受限源码与符号、完整测试矩阵、合规偏差差异和运行时表面清单已汇总到同一份 JSON 证据。

## 测试

- 注册：201
- 通过：199
- 环境型 skip：2 （worker_sandbox_cgroup_v2, worker_sandbox_network_namespace）
- 固定 WPT 文件：84

## 合规与表面

- 偏差：10 项；相对 baseline 无变化；
- 全局：124 个，`worker_global_surface` 验证精确集合；
- 应用可见原生模块：capsid:assert, capsid:env, capsid:fs, capsid:getopts, capsid:hashing, capsid:ipaddr, capsid:permissions, capsid:stdio, capsid:storage, capsid:system, capsid:utils, capsid:uuid；
- 受限原生 helper：11 个。

权威明细、哈希、patch 列表、审计输出、完整全局/模块/原生清单和 JUnit 摘要均在同名 JSON 文件中。
