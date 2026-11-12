# Documentation

Capsid documentation is organized around selection → integration → configuration → validation. If you are new, start with the [project homepage](../README.md); see [CONTRIBUTING.md](../CONTRIBUTING.md) for contributions and [SECURITY.md](../SECURITY.md) for security issues.

## Find by Task

**Selection and Architecture**

- Product boundary: [architecture.md](architecture.md)
- Platform differences and selection: [platform-support.md](platform-support.md);
  Linux isolation: [linux-sandbox.md](linux-sandbox.md);
  Windows build: [windows.md](windows.md)

**Host Integration and Deployment**

- Embedding a C/C++ host: [host-integration.md](host-integration.md)
- First-party Host configuration: [host-config.md](host-config.md) ·
  [capsid-json.md](capsid-json.md)
- Capability policies and module permissions: [capability-policy.md](capability-policy.md) ·
  [module-permissions.md](module-permissions.md)
- managed Host design: [host-technical-design-review.md](host-technical-design-review.md)
- Host-authored Binding packages and isolated runtimes:
  [binding-technical-design.md](binding-technical-design.md) ·
  [binding-modules.md](binding-modules.md)
- Binding v1 adversarial audit and TDD remediation plan:
  [2026-08-16-binding-v1-security-audit-remediation.md](superpowers/plans/2026-08-16-binding-v1-security-audit-remediation.md)

**Compatibility and Quality**

- Standards/framework compatibility: [conformance.md](conformance.md) ·
  [framework-compatibility/README.md](framework-compatibility/README.md)
- Native stream observations and request lifetime:
  [native-stream-observation.md](native-stream-observation.md)
- Gzip in HTTP upgrade fallback responses:
  [native-upgrade-response-gzip.md](native-upgrade-response-gzip.md)
- Array and string iterator storage:
  [native-iterator-storage.md](native-iterator-storage.md)
- Test gate: [testing.md](testing.md)
- Performance evidence: [performance-benchmarks.md](performance-benchmarks.md)

**审校**

- AI 审计简报（v0.2.2 后两阶段总结）:
  [ai-review-brief-v0.2.2-to-v0.3.0.md](ai-review-brief-v0.2.2-to-v0.3.0.md)

- GLM 实现审校、接棒整改与剩余阻塞：
  [v0.3.0-glm-takeover-review.md](v0.3.0-glm-takeover-review.md)

- GLM 当前工作区续接说明、验证顺序与执行命令：
  [v0.3.0-glm-continuation-handoff.md](v0.3.0-glm-continuation-handoff.md)

- 0.3.0 测试完整性、workerd 同输入观察与增补清单：
  [v0.3.0-test-completeness-review.md](v0.3.0-test-completeness-review.md)

- 最新 generation 回收实现、兼容差异与测试集合并反馈：
  [v0.3.0-latest-combined-review.md](v0.3.0-latest-combined-review.md)

- GLM 第三、四轮修复复审与新增边界回归：
  [v0.3.0-glm-fix-review.md](v0.3.0-glm-fix-review.md)

- GLM 第五至七轮历史审校、回收复现与后续修复入口：
  [v0.3.0-glm-round5-7-review.md](v0.3.0-glm-round5-7-review.md)

**v0.3.0 (FetchRPC v4 / WebSocket foundation)**

- Milestone plan:
  [v0.3.0-fetchrpc-v4-plan.md](v0.3.0-fetchrpc-v4-plan.md)
- Implementation design and audit remediation:
  [v0.3.0-implementation-design.md](v0.3.0-implementation-design.md)
- GLM execution plan with task dependencies and acceptance gates:
  [2026-10-12-v0.3.0-remediation-and-delivery.md](superpowers/plans/2026-10-12-v0.3.0-remediation-and-delivery.md)
- FetchRPC v4 wire contract:
  [fetchrpc-v4.md](fetchrpc-v4.md)
- workerd WebSocket semantics dossier:
  [workerd-websocket-semantics.md](workerd-websocket-semantics.md)

## Maintenance Rules

- Maintain only the contracts and reproducible conclusions of the current commit; do not preserve review processes, status snapshots, or generated reports.
- Source-of-truth priority: public headers and build configuration > raw test/benchmark artifacts > Markdown.
- Every `docs/*.md` file must be reachable from this page; relative links are validated by `tests/audit-current-docs.mjs`.

- Dated `docs/evidence/v0.3.0/*/qualification-before.md` files preserve earlier qualification text verbatim. Their adjacent `index.json` must contain the exact SHA256; the documentation audit checks those bytes as archives. Other Markdown files retain the ordinary link and reachability checks.
