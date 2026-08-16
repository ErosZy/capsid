# 文档中心

欢迎阅读 Capsid Runtime 文档。这里维护当前契约、使用说明和可复核的证据规则。
首次接触项目请从[项目首页](../README.md)开始；准备提交变更请阅读
[贡献指南](../CONTRIBUTING.md)，敏感问题按[安全策略](../SECURITY.md)处理。

## 按任务查找

**选型与架构**

- 理解 Capsid 是否适合我的场景：先看
  [架构与产品边界](architecture.md)，再看[标准与合规](conformance.md)。
- 选择部署平台：先看[平台支持总览](platform-support.md)；Linux 隔离见
  [Linux 严格沙箱](linux-sandbox.md)，Windows 构建见
  [Windows 构建与平台能力](windows.md)。

**宿主与部署**

- 把 Runtime 嵌入现有网关：从[宿主嵌入规范](host-integration.md)开始，
  深入阅读[能力策略](capability-policy.md)。
- 部署第一方 managed Host：先看[Host 配置参考](host-config.md)，设计细节见
  [Host v1 详细设计](host-technical-design-review.md)。

**应用与权限**

- 编写并授权应用：跟着[capsid.json 教程](capsid-json.md)逐步配置，
  字段细节查[模块与权限参考](module-permissions.md)。
- 运行不可信代码：只有[Linux 严格沙箱](linux-sandbox.md)提供生产隔离；
  三平台边界见[平台支持总览](platform-support.md)。

**兼容性与证据**

- 移植现有 Fetch 框架：见[框架兼容性](framework-compatibility/README.md)。
- 复现质量或性能结论：测试门见[测试门禁](testing.md)，性能证据见
  [性能证据](performance-benchmarks.md)。

## 维护规则

- 文档只维护当前 commit 的契约与可复现结论；不保存一次性评审过程、每日状态快照
  或生成报告的副本。
- 原始样本与 profile 保存在 `bench/results/`，CI 证据保存在 workflow artifact。
- 事实发生冲突时，优先级如下：

1. 公共头文件、manifest、构建配置和测试；
2. 由当前 commit 生成的原始测试或 benchmark artifact；
3. Markdown 说明。

当前代码线的 Runtime、worker 与第一方 `capsid-host`
（`--mode single-worker` / `static-pool` / `managed`）均可构建、可测试，属可运行
的 benchmark/integration 模式，**非生产部署接口**；里程碑状态以源码和测试为准，
不单独维护易漂移的状态文档。

## 全部文档

### 入门与架构

- [项目首页](../README.md)：定位、三平台差异、快速开始、集成模型、安全配置
- [架构与产品边界](architecture.md)：进程模型、平台契约、JavaScript 表面、
  受限构建、安全边界、资源策略与限制
- [平台支持总览](platform-support.md)：Linux/macOS/Windows 能力矩阵、构建与
  CI 覆盖、生产隔离与选型建议
- [Host v1 详细设计](host-technical-design-review.md)：第一方 Host 的权威
  设计、已冻结契约、验收门与实施顺序

### 宿主嵌入与集成

- [宿主嵌入与集成规范](host-integration.md)：C ABI 生命周期、线程与事件循环、
  请求/credit 背压、SSE/streaming、取消与关闭、ABI 版本策略与上线清单
- [host.json 与 capsid.json 配置参考](host-config.md)：managed 模式两层
  JSON 配置的字段速查、目录布局、secret 文件与 Admin API
- [capsid.json 怎么写（教程）](capsid-json.md)：从最小配置逐步到完整
  配置的手把手教程，含本地模式（`--capsid-json`）、字段值域、常见错误表与部署三步
- [宿主能力策略](capability-policy.md)：三层门禁、可用模块、环境快照、
  storage/stdio/fs 契约、审计事件与逃逸级能力门禁
- [JavaScript 模块与权限参考](module-permissions.md)：bundle 可导入的模块、
  API→权限映射与配置配方

### 平台与安全

- [平台支持总览](platform-support.md)：三平台原生开发/生产隔离矩阵与选型
- [Linux 严格沙箱](linux-sandbox.md)：strict baseline、cgroup v2、网络
  namespace、出站网络策略与明确限制
- [Windows 构建与平台能力](windows.md)：MSVC 构建前置条件与步骤、平台
  能力矩阵、worker 沙箱语义与 managed 模式限制

### 正确性与兼容性

- [测试与持续门禁](testing.md)：测试分层、有效性规则、sanitizer/TSan 与
  平台契约门
- [标准与合规](conformance.md)：ECMA-429 与 WPT 来源锁、合规偏差表、
  能力追踪矩阵
- [框架兼容性](framework-compatibility/README.md)：固定版本的 Hono、itty-router
  和 H3 v2

### 性能

- [性能：证据规则与当前形态](performance-benchmarks.md)：结论门槛、测量
  分层、池规模结论、三栈对照与第一方 Host 优化结果。runner 与证据目录约定
  见 `bench/` 下的脚本与 `bench/results/`。

txiki.js 升级报告由 CI 生成并作为 workflow artifact 保存；仓库只保留构建身份所需的
[`txiki-upgrade-baseline.json`](txiki-upgrade-baseline.json)，不提交会过期的报告副本。
