# 平台支持总览

Capsid 对 Linux、macOS 与 Windows 的承诺分为两个独立层次：

- **原生开发（native dev）**：Runtime、worker、字节码编译器与第一方 Host 能在
  该平台原生构建、启动并通过平台中立测试；
- **生产隔离（production isolation）**：能否在该平台运行不可信代码。当前只有
  Linux strict sandbox 满足这一承诺。

“能构建”不等于“能生产隔离”。macOS 与 Windows 的原生构建用于开发、联调、CI
与 benchmark；涉及不可信代码的生产部署应使用 Linux 容器或 VM。

## 统一能力契约（三平台交集）

**single-worker 与 static-pool 按三平台统一行为承诺**：`static-pool` 的统一契约
固定为**单 shard**。只有 Linux、macOS、Windows 三者行为完全一致的能力才出现在
下面的 ✅ 清单里；任何平台缺失、语义不一致或只能部分实现的能力，一律移出统一
契约，作为平台差异单独说明。

统一承诺：

- Runtime C ABI（spawn/request/credit/streaming）✅
- `capsid-worker`（txiki.js + 受限核心）✅
- `capsid-bytecode-compile`（可信字节码）✅
- Host `--mode single-worker` ✅
- Host `--mode static-pool`（单 shard）✅
- 出站网络策略（egress host/address 规则）✅
- 能力策略（模块/权限/环境快照）✅

**不属于三平台统一契约**（因此按不支持处理，除非显式使用单平台能力）：

- Host `--mode static-pool` 多 shard：仅 Linux/macOS；Windows 启动时拒绝
- Host `--mode managed`（coordinator/Admin/多 App）：仅 Linux
- `capsid:fs`（read/stat/list）：仅 Linux
- strict sandbox（seccomp/Landlock/namespace/cgroup）：仅 Linux
- 多 shard 共享端口（`SO_REUSEPORT`）：仅 Linux/macOS
- worker CPU affinity：Linux 完整；Windows 仅当前处理器组；macOS 无
- `RLIMIT_AS` / `RLIMIT_NOFILE` / `RLIMIT_CORE`：各平台语义不一致，无统一承诺

部署若需要跨三平台一致行为，请只依赖 ✅ 清单；使用差异清单中的能力前必须先做
平台分支或把部署锁定到明确支持的平台。

## 平台构建

### Linux

- 推荐 x86-64 或 AArch64；Release 使用 musl 全静态包
  `capsid-<版本>-linux-musl.tar.gz`；
- `CAPSID_ENABLE_LTO`、`CAPSID_ENABLE_ASAN/UBSAN/TSAN`、fuzz 与
  `CAPSID_GENERATE_LINK_MAP` 可用；
- strict sandbox 要求内核提供 seccomp 与 Landlock；cgroup/namespace 能力由宿主
  委派。详细契约见 [Linux 严格沙箱](linux-sandbox.md)。

### macOS

- 使用系统 Clang 原生构建，Release 包为 `capsid-<版本>-darwin-arm64.tar.gz`；
- single-worker 与单 shard static-pool 属三平台统一契约；macOS 额外支持多
  shard static-pool，但这不是统一承诺；
- 没有 `sched_setaffinity` 等价 API，CPU affinity 测试按 CTest 77 跳过；
- `capsid:fs` 依赖 Linux-only 的 `openat2` 语义，模块调用返回
  “filesystem module is unavailable on this platform”；
- 请求 strict sandbox 会在 worker 启动握手期失败；managed Host 虽可构建，
  但其每次 spawn 都要求 strict，因此不可用于生产。

### Windows

- 使用 MSVC + Ninja + vcpkg（静态 CRT），Release 包为
  `capsid-<版本>-windows-x86_64.zip`；
- single-worker 与单 shard static-pool 属三平台统一契约；多 shard 池在启动
  时被拒绝；
- `capsid:fs`、strict sandbox、managed Host 不可用；
- CPU affinity 通过 `SetProcessAffinityMask` 实现，但只覆盖当前处理器组；
- Worker 内存上限通过 Job Object 的 `JOB_OBJECT_LIMIT_PROCESS_MEMORY` 约束
  **已提交内存**，与 Linux `RLIMIT_AS`（虚拟地址空间）语义不同。
  完整前置条件与差异见 [Windows 构建与平台能力](windows.md)。

## CI 覆盖

`.github/workflows/testing-validity.yml` 提供五类 hosted 证据：

- Ubuntu 24.04 Release/LTO + 固定 WPT + delegated sandbox；
- Ubuntu ASan、UBSan、TSan；
- Clang/libFuzzer 四个 corpus gate；
- macOS 14 posix-host-library；
- `windows-latest` MSVC host-library（平台中立矩阵 + JUnit 证据）。

各平台不支持的场景遵循“不注册即跳过”或 CTest `SKIP_RETURN_CODE 77` 原则，
不得以静默绿代替缺失覆盖。详细清单见 [测试与持续门禁](testing.md)。

## 选型建议

| 场景 | 推荐平台 |
| --- | --- |
| 本地开发、AI 代码生成联调 | Linux、macOS 或 Windows 任意 |
| benchmark 复现 | Linux（发布包与性能基线一致） |
| 运行不可信代码 | **仅 Linux**，启用 strict sandbox 并验证 READY feature bits |
| 需要 Windows 原生宿主 | Windows single-worker / 单 shard static-pool，仅运行受信任代码 |
| 需要 managed 多 App / Admin / 蓝绿部署 | Linux |

事实冲突时以公共头文件、`docs/capability-manifest.json`、构建配置与 CI workflow
为准，Markdown 仅为导航与解释。
