# 文档导航

这里只维护当前契约、使用说明和可复核的证据规则，不保存一次性评审过程、每日状态
快照或生成报告的副本。事实发生冲突时，优先级如下：

1. 公共头文件、manifest、构建配置和测试；
2. 由当前 commit 生成的原始测试或 benchmark artifact；
3. Markdown 说明。

截至当前工作树，Runtime、worker 与第一方 `capsid-host`（`--mode single-worker`）
均可构建、可测试：Host 已具备单 worker 数据面、FetchRPC 协议与 benchmark 基线
（`bench/`），属于可运行的 benchmark/integration 模式，**非生产部署接口**；多
App/pool、多 worker 与安全部署闭环（M1D）继续实现中。里程碑状态以源码和测试为准，
不单独维护易漂移的状态文档。

## 入门与架构

- [项目首页](../README.md)：定位、构建、应用打包和最短集成路径
- [架构与产品边界](architecture.md)：进程模型、平台契约、JavaScript 表面和 vendor 策略
- [Host v1 详细设计](host-technical-design-review.md)：第一方 Host 的唯一权威设计、
  已冻结契约、实施顺序和验收门

## Runtime 嵌入与安全

- [宿主嵌入接口](embedding-api.md)：C ABI 生命周期、流控、事件和超时
- [第三方宿主集成规范](host-integration.md)：线程、SSE、取消、关闭和升级
- [宿主能力策略](capability-policy.md)：模块、操作授权、quota 和审计
- [JavaScript 模块与权限参考](module-permissions.md)：`tjs:*` 门禁、
  `capsid:*` 模块和 API 到权限的映射
- [逃逸级能力门禁](escape-capabilities.md)：FFI/raw socket 不提供结论
- [Linux 严格沙箱](linux-sandbox.md)：seccomp、Landlock、namespace 与 cgroup

## 正确性与兼容性

- [测试与持续门禁](testing.md)：测试分层、反空跑规则和执行方式
- [标准来源锁](conformance-sources.md)：ECMA-429 与 WPT 固定版本
- [能力追踪矩阵](standards-matrix.md)：标准能力到自动化证据的映射
- [合规偏差](conformance-deviations.md)：接受的排除项及退出条件
- [框架兼容性](framework-compatibility/README.md)：固定版本的 Hono、itty-router
  和 H3

## 性能

- [性能证据规则](performance-benchmarks.md)：profile、A/B、原始数据和结论边界
- [Bodyless 性能验收 waiver](bodyless-performance-waiver.md)：机制验收通过、
  性能门未达成的产品决策记录（waiver 不是自动通过）

txiki.js 升级报告由 CI 生成并作为 workflow artifact 保存；仓库只保留构建身份所需的
[`txiki-upgrade-baseline.json`](txiki-upgrade-baseline.json)，不提交会过期的报告副本。
