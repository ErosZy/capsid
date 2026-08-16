# Capsid 兼容性与认证（草案）

认证用于区分“能用 Capsid 接口”和“是官方认可的 Capsid 实现”。本文件是
机制草案，具体认证门槛与工具链随正式认证计划发布。

## 两个等级

| 等级 | 要求 | 名称使用 |
| --- | --- | --- |
| Capsid Compatible | 通过公开 conformance 测试子集，可正确调用 C ABI / FetchRPC / policy schema | 只能写“兼容 Capsid”，不能用认证标记 |
| Capsid Certified Runtime | 通过完整 conformance、安全负控、sandbox 语义与版本化兼容性测试 | 可申请使用认证标记，须遵守商标政策 |

## 认证范围（草案）

- C ABI：`capsid/runtime.h`、`capsid/runtime.hpp`
- FetchRPC：READY、request credit、response credit、streaming、cancel
- 能力策略：module/permission/env/fs/fetch/storage/stdio
- 构建身份：build identity、兼容性 ID、可信字节码
- 平台语义：Linux 完整；macOS/Windows 按有损支持矩阵

## 不认证的内容

- 性能数字
- 宿主路由、TLS、控制平面
- 未进入公共 ABI 的内部实现

认证程序由 Capsid 官方发布，Fork 或第三方实现不得自行使用“Capsid
Certified”标记。
