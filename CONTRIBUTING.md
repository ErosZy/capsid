# 贡献指南

感谢你参与 Capsid。项目当前处于 `0.1.x` 阶段，公共 ABI、安全边界和行为契约
仍在快速演进；清晰的变更范围与可复核证据，比变更规模更重要。

## 开始之前

- Bug、设计建议和兼容性问题可先通过 GitHub Issue 对齐范围。
- 安全漏洞不要公开披露，请按[安全策略](SECURITY.md)报告。
- 大型架构变更、公共 ABI 变更或新增逃逸级能力应先形成设计讨论，再开始实现。
- 一个 Pull Request 尽量只解决一个问题；不要夹带无关格式化或重构。

## 开发环境

基础依赖包括 CMake 3.18+、C/C++ 工具链、Node.js/npm 和 OpenSSL。构建第一方
Host 还需要 Boost；完整 CI 环境以
[`testing-validity.yml`](.github/workflows/testing-validity.yml)为准。

```sh
git clone --recurse-submodules https://github.com/ErosZy/capsid.git
cd capsid
npm ci --ignore-scripts --prefix vendor/txiki.js

for directory in \
  examples/hono-reference \
  examples/itty-router-reference \
  examples/h3-v2-reference
do
  npm ci --ignore-scripts --prefix "$directory"
done
```

日常开发构建：

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DBUILD_TESTING=ON \
  -DCAPSID_BUILD_HOST=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

这条命令不等价于完整 CI。固定 WPT、sanitizer、fuzz 和需要权限的 Linux sandbox
验证见[测试与持续门禁](docs/testing.md)。

## 变更原则

### 代码与契约

- 保持 C11 ABI 和 C++11 公共封装可用；Host 内部代码可以使用其目标声明的
  更高 C++ 标准。
- 新的运行时行为必须有测试覆盖；修复回归时优先先加入能失败的最小测试。
- 安全判断保持 fail-closed。不要用静默降级换取可用性，也不要绕过 capability、
  egress、sandbox 或 artifact identity 检查。
- 不直接修改 `vendor/txiki.js` 生成工作树；供应商变更应通过受审查的 patch 与
  锁定 identity 流程进入。

### 文档

README 只维护定位、最短上手路径、安全警告和稳定入口。字段全集、协议细节、
测试方法和性能证据应放在对应的 `docs/` 权威文档中。

事实冲突时按以下顺序判断：

1. 公共头文件、capability manifest、构建配置与测试；
2. 当前 commit 生成的原始测试或 benchmark artifact；
3. Markdown 说明。

文档变更需要同步检查：

- 命令能否从干净 checkout 执行；
- 本地相对链接是否存在；
- 新增 `docs/*.md` 是否能从 `docs/README.md` 到达；
- 是否复制了会迅速过期的测试数量、状态快照或生成报告；
- 行为、配置或权限变更是否同步更新了全部权威入口。

可直接运行文档审计：

```sh
node tests/audit-current-docs.mjs .
```

## Pull Request 检查清单

- [ ] 变更范围单一，提交信息说明了“为什么”。
- [ ] 新行为或回归修复有对应测试。
- [ ] 本地相关测试已通过，并在 PR 中注明未运行的环境型门禁。
- [ ] 公共 ABI、配置、权限或平台契约变更已同步文档。
- [ ] 未提交 build 目录、依赖目录、临时 profile 或生成报告。
- [ ] 性能结论包含可复核的原始样本、环境、命令、correctness 与 profile 证据。

维护者可能要求拆分变更、补充负控，或提供能复现结果的最小 fixture。CI 结果是
合入依据之一，但不能替代对安全边界和契约变更的人工审查。
