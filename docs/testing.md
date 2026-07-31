# 测试与持续门禁

项目把自有契约、适配 WPT、进程集成和环境型沙箱验证分开报告。任何一层通过
都不能替代另一层。

## 测试分层

1. C/C++ 单元测试：协议、header、策略、拓扑、审计与结构化解析；
2. 真实 worker contract：bundle、IPC、流控、取消、超时、sandbox 和 fetch；
3. 固定 WPT：84 个上游文件，每个在独立 worker realm 中执行；
4. 框架差分：Node reference 与真实 worker 逐向量比较，并保留独立绝对断言；
5. sanitizer/fuzz：项目 target 的 ASan、UBSan 和四个 libFuzzer harness；
6. benchmark contract：先验证内容、版本和环境，再允许记录性能样本。

标准来源、WPT 选择和偏差分别见
[标准来源锁](conformance-sources.md)、
[能力追踪矩阵](standards-matrix.md)和
[合规偏差](conformance-deviations.md)。

## 有效性规则

长期门禁采用 fail-closed：

- 未配置 WPT 时登记 `wpt_conformance_not_configured` 失败哨兵；
- checkout 必须等于 manifest 固定 commit；
- manifest 与实际注册路径逐项比对，不只比较数量；
- WPT realm 必须报告顶层唯一、正整数 `total` 和
  `ranNothing: false`；
- expected failure 与 `notExecuted` 只从 manifest 读取，并必须引用已登记偏差；
- restricted 二进制审计必须先证明符号表、archive 和 LTO marker 可读；
- Wasm 的 C、JavaScript 和 fixture 资源常量必须一致；
- overlay key、stamp、manifest 和 configure dependencies 必须来自同一
  canonical 输入；
- 三个框架的差分向量必须有独立绝对断言，anchor 数不能为零；
- 负控必须证明门禁能捕获错误输入，不能只有“正确实现会绿”的正向测试。

测试有效性专项审计已在 2026-07-29 闭环，并由上述自动化门禁取代。持续保留
一次性审计报告会与当前代码漂移，因此不再将其作为产品状态文档。

## 常用命令

基础构建：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

固定 WPT：

```sh
cmake -S . -B build-wpt \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPSID_ENABLE_LTO=ON \
  -DCAPSID_WPT_ROOT=/absolute/path/to/pinned/wpt
cmake --build build-wpt
ctest --test-dir build-wpt --output-on-failure
```

WPT 根目录的 `HEAD` 必须等于 `tests/wpt/manifest.json` 中的 commit。

ASan 示例：

```sh
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCAPSID_ENABLE_ASAN=ON \
  -DCAPSID_USE_MIMALLOC=OFF
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure \
  -E '^(worker_strict_sandbox_direct_fetch|worker_strict_sandbox_https_ca)$'
```

UBSan 将开关替换为 `CAPSID_ENABLE_UBSAN=ON`。fuzz 构建使用 Clang、
`CAPSID_BUILD_WORKER=OFF` 和 `CAPSID_BUILD_FUZZERS=ON`。

## 环境型 sandbox 证据

普通宿主缺少 cgroup delegation 或 namespace 权限时，相应测试返回 CTest
skip code 77。这只表示环境不具备前置条件，不能写成通过。

`.github/workflows/testing-validity.yml` 包含四类独立 job：

- Ubuntu Release/LTO、固定 WPT、benchmark smoke 和 privileged delegated
  sandbox，并生成 txiki.js 升级报告；
- Ubuntu ASan 与 UBSan 普通矩阵；ASan 仅排除两个已由 Release 严格门禁和
  ASan 非严格同功能门禁重复覆盖的 strict-sandbox 网络/TLS 退出项，因为
  seccomp 会在 instrumented runtime teardown 阶段终止进程；
- Clang/libFuzzer 的四个 bounded corpus gate；
- macOS 14 的 POSIX host-library 与非 worker 单元矩阵。

JUnit、build metadata 和升级报告作为 workflow artifact 保存。最终
`hosted-evidence-index` job 下载各 job 证据，写入 run URL、commit SHA、
各证据文件 SHA-256 和证据树摘要，并要求所有依赖 job 成功。delegated sandbox
脚本把 77 视为 CI failure；普通 runner 的环境型 skip 不能替代它。首次
hosted 全绿索引只能在本次变更 commit/push 后取得，由 `TODO-P2-04` 保留这一项
外部证据。

所有第三方 action 都固定到审查过的 40 位 commit SHA。仓库内的
`testing_validity_workflow_audit` 同时锁定 action 清单、四类 job、安全门和
CTest JUnit 相对路径；`--test-dir` 已经确定输出根目录，禁止再次把 build
目录写进 `--output-junit`，以免报告与 artifact 读取到不存在的双层路径。

## 当前已知基线

2026-07-30 的 Release/LTO + 固定 WPT + benchmark 构建注册 201 项：
普通宿主实测 199 通过、2 个环境型 skip、0 失败。84 个固定 WPT 文件均执行；
两项 delegated sandbox 正向测试必须在具备权限的独立环境中通过，不能用普通
宿主的 skip 代替。

本轮完整证据汇总见
[`txiki-upgrade-report.md`](txiki-upgrade-report.md)。数字只描述该固定版本和
manifest；新增或删除测试后应更新报告，不能把旧计数当成永久承诺。
