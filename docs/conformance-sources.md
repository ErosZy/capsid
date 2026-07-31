# 标准来源锁

本文是 `CAPSID-MIN-2025-subset-v0` 的人类可读版本锁。机器读取的 WPT 选择以
[`tests/wpt/manifest.json`](../tests/wpt/manifest.json) 为准。

## 规范基线

- 标准：ECMA-429《Minimum common web API》第一版，2025 年 12 月；
- 发布文档：
  `https://ecma-international.org/wp-content/uploads/ECMA-429_1st_edition_december_2025.pdf`；
- PDF SHA-256：
  `9f8abe3fa86517675cb8388b8b2b3a4024bb6d5d9e3467b89ae4013d20ae30b5`；
- 建锁时参考的编辑源：`WinterTC55/proposal-minimum-common-api`
  commit `fe94bc2b0e349d7aae635c27c653b5165039ab66`。

在线 editor draft 只供参考，不能静默替换已发布版本。ECMA-429 引用的 living
standard 通过固定 WPT revision 提供可复现测试；移动 WPT commit 本身就是
需要审查的 conformance 更新。

## WPT 锁

- 仓库：`https://github.com/web-platform-tests/wpt.git`；
- commit：`1985b47aa8972a970f005957f2bfa036da1787c6`；
- 精确路径：`tests/wpt/manifest.json`；
- branch name 不能作为构建或 CI 输入；
- 测试文件和传递资源必须来自同一 commit；
- WPT checkout 只作为测试输入，不链接到运行时。

当前 profile 执行 manifest 中 `executedProfile` 的 84 个路径。每个文件与
项目 adapter 组合后，在独立 worker/realm 中执行。CMake 会拒绝 `HEAD` 与
锁定 commit 不一致的 checkout。

HTML 输入只提取原 inline script，不修改 assertion。每个 bundle 使用固定的
WPT URL 作为逻辑源码名，便于错误定位。

`promise-rejection-events.html` 的 document-scoped harness 不适合当前
one-file-per-worker-realm 模型，因此 manifest 将其列入 `notExecuted`；
对应 worker support source 直接执行，ordering 语义由项目 contract 补足。

## 证据分层

1. `worker_p1_platform_contract`：项目自有的进程级回归，不标记为 WPT；
2. adapted WPT：保留上游 assertion 和 metadata，adapter 只提供 harness、
   固定资源和结果传输；
3. host integration：覆盖 IPC、生命周期、网络、资源限制和 C ABI。

第一层通过不能推出第二层通过。能力矩阵会分别记录。

adapter 支持当前选择所需的 sync/async/promise test primitives。任何未支持
harness 能力必须使批次失败，不能静默 skip。测试专用的 `location.href`、
固定 resource map 和 rejection trigger 只存在于测试 realm，不进入产品表面。

已审查的机械适配包括：

- classic-script 资源在 ESM bundling 前按固定顺序拼接；
- 去除拼接后会产生重复声明的 helper tail；
- 将 strict ESM 中非法的 rest parameter `arguments` 改为
  `importArguments`；
- CAPSID-D009 下用等价 `Reflect.construct` 探针替换 QuickJS 有问题的
  Proxy constructor probe。

expected failure 必须精确到 path/subtest，并引用已登记偏差；unexpected
failure 和 unexpected pass 都使结果失败。

## 选择与排除

- 只有属于 ECMA-429 且纳入本 profile 的 API 才在范围内；
- profile 内测试若不执行，必须引用偏差或开放 gap ID；
- harness 不兼容是需要修复的工作，不能当成语义 expected failure；
- Window、Document、ServiceWorker、WASI 等 profile 外 API 无需偏差 ID；
- tentative test 默认不选，除非有明确且稳定的 profile 需求；
- 不得编辑测试去迎合当前 txiki.js 行为；
- 网络测试必须使用确定性的本地 fixture，公共互联网不是 conformance 依赖。

## 更新流程

更新必须记录旧/新 commit，审查所有已选文件变化，运行完整
contract/WPT/integration 矩阵，确认 vendor clean，并同步能力矩阵与偏差表。
CI 必须拒绝与 manifest commit 不一致的 WPT checkout。
