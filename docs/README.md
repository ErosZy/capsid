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

**Compatibility and Quality**

- Standards/framework compatibility: [conformance.md](conformance.md) ·
  [framework-compatibility/README.md](framework-compatibility/README.md)
- Test gate: [testing.md](testing.md)
- Performance evidence: [performance-benchmarks.md](performance-benchmarks.md)
- Deployed bytecode optimizer: [bytecode-aot-optimizer.md](bytecode-aot-optimizer.md)
- Completed opcode profiling/AOT evidence:
  [quickjs-ng-opcode-optimization.md](quickjs-ng-opcode-optimization.md)
- Active CFG+SSA, shape IC, and extended-opcode plan:
  [quickjs-ng-cfg-ssa-shape-ic.md](quickjs-ng-cfg-ssa-shape-ic.md)

## Maintenance Rules

- Maintain only the contracts and reproducible conclusions of the current commit; do not preserve review processes, status snapshots, or generated reports.
- Source-of-truth priority: public headers and build configuration > raw test/benchmark artifacts > Markdown.
- Every `docs/*.md` file must be reachable from this page; relative links are validated by `tests/audit-current-docs.mjs`.
