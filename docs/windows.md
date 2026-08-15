# Windows 构建与平台能力

Capsid v0.1.2 起提供 Windows x86_64 支持：Runtime 静态库、`capsid-worker`、
`capsid-bytecode-compile` 与第一方 `capsid-host`（`--mode single-worker` /
`static-pool`）在 MSVC 下原生构建，并以 `capsid-<版本>-windows-x86_64.zip`
形式随 Release 发布。本页说明构建前置条件、能力矩阵与已知限制。

Linux 的严格沙箱语义（seccomp/Landlock/namespace/cgroup）没有 Windows
等价物；需要完整隔离边界的部署应继续使用 Linux 包（[linux-sandbox.md](linux-sandbox.md)）。

## 构建

### 前置条件

- Windows 10 1803+（worker 与 Host 之间的 IPC 使用 Winsock 回环 socket；
  Admin socket 若启用 AF_UNIX 同样需要 1803+。所有官方测试均在
  `windows-latest` runner（Windows Server 2022+）上运行）；
- Visual Studio 2022+，含“使用 C++ 的桌面开发”工作负载（MSVC + Windows SDK）；
- CMake ≥ 3.18 与 Ninja（VS 安装器自带，或单独安装）；
- Node.js 24（用于 `npm ci` 提供 esbuild）；
- Python 3（`python` 或 `python3` 均可，测试夹具使用）；
- vcpkg（提供静态 OpenSSL 与 Boost 头文件）。

### 步骤

```powershell
# 1. 依赖：静态 OpenSSL 与 Boost.System/Asio（Boost ≥1.87 为 header-only；
#    boost-asio 是独立 port，Host 数据面需要 <boost/asio.hpp>）
vcpkg install openssl boost-system boost-asio --triplet x64-windows-static

# 2. 锁定 JS 依赖（esbuild）
npm ci --ignore-scripts --prefix vendor/txiki.js
npm ci --ignore-scripts --prefix examples/hono-reference
npm ci --ignore-scripts --prefix examples/itty-router-reference
npm ci --ignore-scripts --prefix examples/h3-v2-reference

# 3. 配置（静态 CRT 与 x64-windows-static triplet 必须匹配）
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCAPSID_BUILD_HOST=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static

# 4. 构建与测试
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -E '^(wpt_conformance_not_configured|worker_package_.*)$'

# 5. 打包
cmake --build build --target package   # 产出 build/capsid-<版本>-windows-x86_64.zip + .sha256
```

配置要点：

- **静态 CRT**：顶层 `CMakeLists.txt` 在 MSVC 下固定
  `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`。vendored txiki.js 与其依赖
  （libwebsockets/mbedtls 等）同样固定静态 CRT；混用 `/MD` 会在链接期
  LNK2038 失败。
- **iconv**：Windows 没有系统 iconv，构建使用仓库内置的
  [win-iconv](../vendor/win-iconv/VENDOR.txt)（公有领域，Win32
  MultiByteToWideChar 实现）。Linux/macOS 继续使用系统 iconv。
- **txiki overlay**：构建把 vendored txiki.js 复制到 build 树并打补丁。
  没有开启“开发者模式”的机器无法创建符号链接，`PrepareTxiki.cmake` 会
  自动退化为整目录复制（内容一致，overlay key 只哈希文件内容）。
- **OpenSSL/Boot**：通过 vcpkg `x64-windows-static` triplet 提供；本项目
  不执行配置期下载（延续 build_host.cmake 的“无 FetchContent”约束）。

### 已知可用配置

- `CAPSID_BUILD_WORKER=ON` + `CAPSID_BUILD_HOST=ON`（Release，含 LTO）：发布
  矩阵配置；
- `CAPSID_STRICT_WARNINGS=ON`（默认）：MSVC 下为 `/W4 /WX`；
- `CAPSID_ENABLE_ASAN=ON`：MSVC `/fsanitize=address`（须
  `CAPSID_USE_MIMALLOC=OFF`）。

### 不可用配置（配置期报错）

- `CAPSID_ENABLE_UBSAN` / `CAPSID_BUILD_FUZZERS`：MSVC 不支持
  （`CMakeLists.txt` 明确拒绝）；
- `CAPSID_ENABLE_TSAN`：仅 Linux；
- `CAPSID_GENERATE_LINK_MAP`：仅 Linux GNU/Clang；
- `--mode managed`：见下文“Host”。

## 平台能力矩阵

| 能力 | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Runtime C ABI（spawn/request/credit/streaming） | ✅ | ✅ | ✅ |
| `capsid-worker`（txiki.js + 受限核心） | ✅ | ✅ | ✅ |
| `capsid-bytecode-compile`（M1D 可信字节码） | ✅ | ✅ | ✅ |
| Host `--mode single-worker` / `static-pool` | ✅ | ✅ | ✅ |
| Host `--mode managed`（coordinator/Admin/多 App） | ✅ | ❌ | ❌ |
| 出站网络策略（egress host/address 规则、保护段） | ✅ | ✅ | ✅（JS 层） |
| 能力策略（模块/权限/环境快照） | ✅ | ✅ | ✅ |
| fs 权限模块（capsid:fs read/stat/list） | ✅ | ❌ | ❌（见下） |
| RLIMIT_AS / RLIMIT_NOFILE / RLIMIT_CORE | ✅ | 部分 | 部分（见下） |
| strict sandbox（seccomp/Landlock/namespace/cgroup） | ✅ | ❌ | ❌ |

### worker 沙箱语义（Windows）

`apply_sandbox` 在 Windows 上的行为：

- **进程内存上限**（`process_memory_limit`）：通过 Job Object
  （`JOB_OBJECT_LIMIT_PROCESS_MEMORY`）在 worker 进程内自施压。注意语义差异：
  Linux 的 RLIMIT_AS 限制虚拟地址空间，Windows 的 job 限制**已提交内存**
  （working set 上限），两者不等价；
- **文件描述符上限**：Windows 没有进程级句柄数限制。非零的
  `file_descriptor_limit` 会被**接受但不执行**（默认值 64 也在此列），
  并如实不在 applied features 中声明 `CAPSID_SANDBOX_FEATURE_RLIMITS`
  的该部分——申请了该 feature 的宿主会在 hello 校验中看到缺失并失败；
- **core dump 抑制**（RLIMIT_CORE 等价）：`SetErrorMode` 抑制 WER 崩溃
  对话框，worker 崩溃不会阻塞前台会话；
- **strict 模式**：spawn 阶段即拒绝（`CAPSID_INVALID_ARGUMENT`），worker
  自身也会以 “strict sandbox is unavailable on this platform/build”
  拒绝。网络 namespace fd、cgroup 路径等 strict-only 参数同样在 spawn
  校验中拒绝。

### Host（Windows）

- **single-worker / static-pool** 完整可用。进程停止信号：Windows 没有
  `sigwait`，`static-pool` 用控制台控制处理器（CTRL_C/CTRL_BREAK/关闭）
  触发同一停止路径；`single-worker` 使用 Boost.Asio `signal_set`（Windows
  下同样可用）。
- **managed 模式不可用**：coordinator 的状态机依赖 dirfd 相对路径
  （openat/mkdirat/fstatat）、uid 属主校验与 UDS Admin 平面；这些语义
  无法在 Windows 上等价实现，构建直接排除（`--mode managed` 启动时报错
  并指向本页）。进程快照（RSS/CPU）在 Windows 上通过
  `GetProcessMemoryInfo`/`GetProcessTimes` 提供（PSS 无等价物，回退 RSS）。
- **文件属主/权限校验**（trusted key store、部署读取、状态目录）：
  Windows 没有 uid/mode 位，属主检查被跳过；边界由 NTFS ACL 承担，
  部署读取保留 reparse-point（符号链接/junction）拒绝语义。

### worker 内部差异

- **IPC 传输**：host↔worker 的 IPC 从 `socketpair(AF_UNIX)` 换成回环 TCP
  socket 对。子进程句柄通过 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 显式继承
  （只有 IPC socket 与按需的 stdio 跨进程边界，等价于 POSIX 的
  close_range 扫尾）。`--ipc-fd` 携带的是句柄值而不是 fd 编号。
- **fs 模块不可用**：`capsid:fs` 的读路径依赖 `openat2(RESOLVE_NO_SYMLINKS)`
  （Linux-only），Windows 上模块调用返回 “filesystem module is
  unavailable on this platform”（与 macOS 一致）。需要读取文件的部署应
  使用 bundle 内资源或 storage 模块。
- **TextDecoder**：受限核心的任意编码→UTF-8 转换在 Windows 上由
  win-iconv 提供。win-iconv 不做严格编码校验（其 readme 明示），
  TextDecoder 语义与 Linux（glibc iconv）存在允许范围内的差异。

## CI

- `.github/workflows/testing-validity.yml` 的 `windows-host-library` job：
  构建 Runtime/worker/Host + 运行全部平台中立测试（Linux-only 测试按
  “不注册即跳过”原则缺席，与 macOS 行同一策略），产出 JUnit 证据并参与
  `hosted-evidence-index` 硬门禁。
- `.github/workflows/release.yml` 的 Windows 矩阵项产出
  `capsid-<版本>-windows-x86_64.zip` 与 `.sha256` 并上传 Release。

## 更新检查单

升级 txiki.js / 修改平台层时：

1. 本地与 CI 双跑 `windows-host-library` 矩阵；
2. 检查 `docs/linux-sandbox.md` 与本页的能力矩阵是否需要同步；
3. 若新增强制（如 fd 上限的 Windows 实现）落地，更新本页对应小节并
   重跑 sandbox 相关契约测试。
