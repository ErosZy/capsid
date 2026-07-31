# 文档导航

除明确标注为拟议设计的规划文档外，文档只维护当前有效契约，不保留一次性审计
过程或 revision 流水账。事实优先级：

1. 公共头文件、capability/WPT manifest 和构建配置；
2. 自动生成的测试与 benchmark 原始报告；
3. Markdown 解释文档。

如果 Markdown 与前两层冲突，应修正文档并增加防漂移测试。

## 产品与嵌入

- [项目首页](../README.md)：定位、能力和最短构建路径
- [架构与产品边界](architecture.md)：进程模型、JavaScript 表面和 vendor 策略
- [宿主嵌入接口](embedding-api.md)：C ABI 生命周期、流控、事件和超时
- [第三方宿主集成规范](host-integration.md)：线程、SSE、取消、关闭和升级
- [Capsid Host 架构规划](host-architecture-plan.md)：拟议的应用发现、JSON
  配置、发布、路由、worker pool 和弹性模型
- [当前状态与发布门](project-status.md)：唯一活跃事项和冻结决策

## 正确性与兼容性

- [测试与持续门禁](testing.md)：测试分层、反空跑规则和当前基线
- [标准来源锁](conformance-sources.md)：ECMA-429 与 WPT 固定版本
- [能力追踪矩阵](standards-matrix.md)：标准能力到自动化证据的映射
- [合规偏差](conformance-deviations.md)：接受的排除项及退出条件
- [框架兼容性](framework-compatibility/README.md)：固定版本的 Hono、itty-router
  和 H3
- [txiki.js 升级报告](txiki-upgrade-report.md)：生成的 vendor、全测和表面摘要

## 安全与性能

- [宿主能力策略](capability-policy.md)：模块、操作授权、quota 和审计
- [JavaScript 模块与权限参考](module-permissions.md)：`tjs:*` 门禁、
  `capsid:*` 公共模块和 API 到权限的完整映射
- [逃逸级能力门禁](escape-capabilities.md)：FFI/raw socket 不提供结论
- [Linux 严格沙箱](linux-sandbox.md)：seccomp、Landlock、namespace 与 cgroup
- [性能结论](performance-benchmarks.md)：当前权威结果和适用边界
- [基准复现](../bench/README.md)：构建、运行、内容校验和结果保存
