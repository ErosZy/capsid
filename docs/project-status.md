# Capsid Runtime 当前状态

> 更新：2026-07-31

本文件只记录尚未完成的事项和发布门。实现边界见
[`architecture.md`](architecture.md)，自动化证据见
[`testing.md`](testing.md)，性能结论见
[`performance-benchmarks.md`](performance-benchmarks.md)。

## 当前结论

核心运行时、ABI v7、固定 Web API profile、84-file WPT、Linux 沙箱、能力
策略、三个框架矩阵、现代 HTTP/SQLite benchmark、Vue SSR 容量复测、可信
字节码冷启动和单 worker 资源基线均已形成自动化证据。

产品默认保持最小权限。十二个 `capsid:` 模块已进入显式授权表面；WebSocket、
产品 SQLite、readline、文件写入/监听、FFI 和 raw socket 已完成评估并明确
不提供，不属于未完成实现。

## 活跃事项

| ID | 状态 | 完成条件 |
| --- | --- | --- |
| TODO-P2-04 | 待远端 | 将当前变更提交并推送后，GitHub hosted workflow 的 Release/WPT、delegated sandbox、ASan、UBSan、fuzz 和 macOS job 全绿，并保存最终证据索引。 |

本地 workflow 审计已经固定第三方 action SHA、JUnit 路径和安全门。hosted
运行结果是外部证据，不能用本机的环境型 skip 代替。

## 已冻结的决策

- Capsid 只提供 runtime 与嵌入 ABI，不内置 HTTP server 或 worker pool；
- 公共 JavaScript module 只使用 `capsid:`；
- 系统 allocator 为默认，mimalloc 只在明确回归 profile 下重开 A/B；
- QuickJS bytecode 只接受宿主同构建生成并校验的可信产物；
- benchmark-only SQLite 不进入产品能力表面；
- 任何新能力必须同时增加 build、module、operation 和 OS sandbox 证据；
- 性能改动必须保留 conformance、credit/cancel、资源上限和安全边界。

## 发布门

- vendor、patch、WPT、npm lock 和第三方 action revision 固定；
- Release/LTO 全测通过，delegated sandbox 取得正向证据；
- 标准矩阵、偏差表和 capability manifest 与代码一致；
- 最终二进制通过 source、symbol、archive、module 正负控审计；
- 宿主正确处理背压、view 生命周期、取消、超时、退出和 worker 替换；
- 部署方公布资源、网络、namespace、cgroup 和 sandbox 策略。
