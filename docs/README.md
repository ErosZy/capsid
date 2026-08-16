# 文档中心

Capsid 文档按“选型 → 集成 → 配置 → 验证”组织。首次接触从
[项目首页](../README.md)开始；贡献见 [CONTRIBUTING.md](../CONTRIBUTING.md)；
安全问题见 [SECURITY.md](../SECURITY.md)。

## 按任务查找

**选型与架构**

- 产品边界：[architecture.md](architecture.md)
- 平台差异与选型：[platform-support.md](platform-support.md)；
  Linux 隔离：[linux-sandbox.md](linux-sandbox.md)；
  Windows 构建：[windows.md](windows.md)
- 许可证与商业边界：[licensing.md](licensing.md)；
  商标：[../TRADEMARK.md](../TRADEMARK.md)；
  认证：[certification.md](certification.md)

**宿主集成与部署**

- 嵌入 C/C++ 宿主：[host-integration.md](host-integration.md)
- 第一方 Host 配置：[host-config.md](host-config.md) ·
  [capsid-json.md](capsid-json.md)
- 能力策略与模块权限：[capability-policy.md](capability-policy.md) ·
  [module-permissions.md](module-permissions.md)
- managed Host 设计：[host-technical-design-review.md](host-technical-design-review.md)

**兼容性与质量**

- 标准/框架兼容：[conformance.md](conformance.md) ·
  [framework-compatibility/README.md](framework-compatibility/README.md)
- 测试门禁：[testing.md](testing.md)
- 性能证据：[performance-benchmarks.md](performance-benchmarks.md)

## 维护规则

- 只维护当前 commit 的契约与可复现结论，不保存评审过程/状态快照/生成报告。
- 事实优先级：公共头文件与构建配置 > 原始测试/benchmark artifact > Markdown。
- `docs/*.md` 必须能从本页到达；相对链接由 `tests/audit-current-docs.mjs` 校验。
