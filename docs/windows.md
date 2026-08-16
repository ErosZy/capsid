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
vcpkg install openssl boost-system boost-asio boost-beast --triplet x64-windows-static

# 2. 锁定 JS 依赖（esbuild）
npm ci --ignore-scripts --prefix vendor/txiki.js
npm ci --ignore-scripts --prefix examples/hono-reference
npm ci --ignore-scripts --prefix examples/itty-router-reference
npm ci --ignore-scripts --prefix examples/h3-v2-reference

# 3. 配置（静态 CRT 与 x64-windows-static triplet 必须匹配）
cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=cl `
  -DCMAKE_CXX_COMPILER=cl `
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
- **MSVC 编译器**：Ninja 在未 vcvars 的 shell（CI、普通终端）里会从 PATH
  抓到 MinGW gcc，无法构建 CRT/winsock 移植面；必须显式
  `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl`。配置非 MSVC 编译器时
  顶层 CMakeLists 会直接 FATAL_ERROR。
- **iconv**：Windows 没有系统 iconv，构建使用仓库内置的
  [win-iconv](../vendor/win-iconv/VENDOR.txt)（公有领域，Win32
  MultiByteToWideChar 实现）。Linux/macOS 继续使用系统 iconv。
- **txiki overlay**：构建把 vendored txiki.js 复制到 build 树并打补丁。
  没有开启“开发者模式”的机器无法创建符号链接，`PrepareTxiki.cmake` 会
  自动退化为整目录复制（内容一致，overlay key 只哈希文件内容）。
- **OpenSSL/Boot**：通过 vcpkg `x64-windows-static` triplet 提供；本项目
  不执行配置期下载（延续 build_host.cmake 的“无 FetchContent”约束）。
- **LTO 在 Windows 上强制关闭**：MSVC 的 `/GL` 在编译期绑定
  `operator new/delete`，全局可替换符号的覆写（ABI guard 的分配失败注入
  依赖这一契约）不再拦截 IPO 过的翻译单元。GCC/Clang 的 LTO 保留可替换
  符号，故 POSIX 构建不受影响；build identity 的 feature flags 会如实
  记录 `lto=OFF`。

### 已知可用配置

- `CAPSID_BUILD_WORKER=ON` + `CAPSID_BUILD_HOST=ON`（Release）：发布
  矩阵配置（Windows 上 LTO 恒为 OFF，见上）；
- `CAPSID_STRICT_WARNINGS=ON`（默认）：MSVC 下为 `/W4 /WX`；
- `CAPSID_ENABLE_ASAN=ON`：MSVC `/fsanitize=address`（须
  `CAPSID_USE_MIMALLOC=OFF`）。

### 不可用配置（配置期报错）

- `CAPSID_ENABLE_UBSAN` / `CAPSID_BUILD_FUZZERS`：MSVC 不支持
  （`CMakeLists.txt` 明确拒绝）；
- `CAPSID_ENABLE_TSAN`：仅 Linux；
- `CAPSID_GENERATE_LINK_MAP`：仅 Linux GNU/Clang；
- `--mode managed`（Windows）：构建直接排除；见下文“Host”。

## 平台能力矩阵

| 能力 | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Runtime C ABI（spawn/request/credit/streaming） | ✅ | ✅ | ✅ |
| `capsid-worker`（txiki.js + 受限核心） | ✅ | ✅ | ✅ |
| `capsid-bytecode-compile`（M1D 可信字节码） | ✅ | ✅ | ✅ |
| Host `--mode single-worker` / `static-pool` | ✅ | ✅ | ✅ |
| Host `--mode managed`（coordinator/Admin/多 App） | ✅ | ❌（运行时失败） | ❌（构建排除） |
| 出站网络策略（egress host/address 规则、保护段） | ✅ | ✅ | ✅（JS 层） |
| 能力策略（模块/权限/环境快照） | ✅ | ✅ | ✅ |
| fs 权限模块（capsid:fs read/stat/list） | ✅ | ❌（模块边界拒绝） | ❌（模块边界拒绝） |
| RLIMIT_AS / RLIMIT_NOFILE / RLIMIT_CORE | ✅ | 部分（RLIMIT_AS 编译期拒绝） | 部分（见下） |
| strict sandbox（seccomp/Landlock/namespace/cgroup） | ✅ | ❌ | ❌（见下） |
| 多 shard 共享端口（SO_REUSEPORT） | ✅ | ✅ | ❌ |
| worker CPU affinity（`capsid_worker_set_cpu_affinity`） | ✅ | ❌ | ✅ |

### worker 沙箱语义（Windows）

`apply_sandbox` 在 Windows 上的行为：

- **进程内存上限**（`process_memory_limit`）：通过 Job Object
  （`JOB_OBJECT_LIMIT_PROCESS_MEMORY`）在 worker 进程内自施压。注意语义差异：
  Linux 的 RLIMIT_AS 限制虚拟地址空间，Windows 的 job 限制**已提交
  （committed）内存**——既不是 working set 也不是虚拟地址空间，两者
  不等价。若 `AssignProcessToJobObject` 失败（例如 worker 已被 CI runner
  等外层 job 归属），worker 会拒绝启动而不是静默降级；
- **文件描述符上限**：Windows 没有进程级句柄数限制。非零的
  `file_descriptor_limit` 会被**接受但不执行**（默认值 64 也在此列），
  并如实不在 applied features 中声明 `CAPSID_SANDBOX_FEATURE_RLIMITS`
  的该部分——申请了该 feature 的宿主会在 hello 校验中看到缺失并失败；
- **core dump 抑制**（RLIMIT_CORE 等价）：`SetErrorMode` 抑制 WER 崩溃
  对话框，worker 崩溃不会阻塞前台会话；
- **strict 模式**：与 Linux 在 spawn 阶段即返回错误不同，Windows 上
  spawn 返回成功，strict-only 参数（网络 namespace fd、cgroup 路径等）
  被接受；worker 启动后发现自己无法施加 strict sandbox，在启动握手期
  异步报 “strict sandbox is unavailable on this platform/build” 并以
  退出码 1 退出。宿主通过 worker 的退出事件感知失败，而不是 spawn 的
  返回值。macOS 的 strict 语义与 Windows 一致（见下文 Host 的 managed
  条目）。

### Host（Windows）

- **single-worker / static-pool** 完整可用。进程停止信号：Windows 没有
  `sigwait`，`static-pool` 用控制台控制处理器（CTRL_C/CTRL_BREAK/关闭）
  触发同一停止路径；`single-worker` 使用 Boost.Asio `signal_set`（Windows
  下同样可用）。
- **多 shard 静态池不可用**：多 shard 共享端口依赖 `SO_REUSEPORT`。
  Windows 不提供该选项（Linux/macOS 均可用），Windows 构建只支持单
  shard 池；多 shard 启动会被拒绝。多 shard 场景测试
  （`host_static_pool_server_shared_port_lifecycle`、
  `host_admission_pool_forwards_options`、`host_concurrent_pool_wait`）
  在 Windows 上不注册；m2 组其余测试（含 single-worker 与单 shard 池
  场景）正常注册并运行。m2 组中另有两个因 POSIX 依赖而不注册的场景，
  见下文“Windows 上的测试覆盖差异”。
- **managed 模式不可用**：coordinator 的状态机依赖 dirfd 相对路径
  （openat/mkdirat/fstatat）、uid 属主校验与 UDS Admin 平面；这些语义
  无法在 Windows 上等价实现，构建直接排除（`--mode managed` 启动时报错
  并指向本页）。macOS 上 managed 可编译且能启动，但每次 spawn 都会打开
  strict sandbox（Landlock/seccomp/namespace 均无 macOS 实现），首个
  worker 必然失败——macOS 上部署 managed 同样不可行，只是失败点在
  spawn 而不是构建期。进程快照（RSS/CPU）在 Windows 上通过
  `GetProcessMemoryInfo`/`GetProcessTimes` 提供（PSS 无等价物，回退 RSS）。
- **文件属主/权限校验**（trusted key store、部署读取、状态目录）：
  Windows 没有 uid/mode 位，属主检查被跳过；边界由 NTFS ACL 承担，
  部署读取保留 reparse-point（符号链接/junction）拒绝语义。

### worker 内部差异

- **IPC 传输**：host↔worker 的 IPC 从 `socketpair(AF_UNIX)` 换成回环 TCP
  socket 对。子进程句柄通过 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 显式继承
  （只有 IPC socket 与按需的 stdio 跨进程边界，等价于 POSIX 的
  close_range 扫尾）。`--ipc-fd` 携带的是句柄值而不是 fd 编号。
  accept 出来的 socket 在关闭 listener 前必须
  `SO_UPDATE_ACCEPT_CONTEXT` 重新绑定 listener 上下文：否则 AFD 回收
  listener 的端点名后，已接受 socket 的后续 I/O 会以
  `ERROR_ALREADY_EXISTS` 随机失败（socket 对两端都可能中毒）。
- **fs 模块不可用**：`capsid:fs` 的读路径依赖 `openat2(RESOLVE_NO_SYMLINKS)`
  （Linux-only），Windows 上模块调用返回 “filesystem module is
  unavailable on this platform”（与 macOS 一致）。需要读取文件的部署应
  使用 bundle 内资源或 storage 模块。
- **连接终止语义**：Windows 上对端 reset 的 socket 读以 EOF（返回 0）
  结束（`WSAECONNRESET` 并入正常关闭路径），因此 worker 异常终止时
  host 观察到的是 EXIT 事件；Linux 能区分 CLOSED 与 ERROR。WSAPoll 不
  报告 `POLLHUP`，连接关闭只能通过读返回 0 检测。
- **启动握手超时**：worker 启动阶段的协议读取有 2s 上限（POSIX 侧无界），
  启动过慢或僵住的 worker 会被宿主判定启动失败。
- **进程终止退出码**：`TerminateProcess` 固定报退出码 1（SIGKILL 的
  语义等价物），与 Linux 按信号映射的 128+N 退出码不同——该差异仅在
  检查退出码的测试中可见。
- **TextDecoder**：受限核心的任意编码→UTF-8 转换在 Windows 上由
  win-iconv 提供。win-iconv 不做严格编码校验（其 readme 明示），
  TextDecoder 语义与 Linux（glibc iconv）存在允许范围内的差异。

## CI

- `.github/workflows/testing-validity.yml` 的 `windows-host-library` job：
  构建 Runtime/worker/Host + 运行全部平台中立测试（Linux-only 测试按
  “不注册即跳过”或 `SKIP_RETURN_CODE 77` 原则缺席，与 macOS 行同一
  策略），产出 JUnit 证据并参与 `hosted-evidence-index` 硬门禁。
- `.github/workflows/release.yml` 的 Windows 矩阵项产出
  `capsid-<版本>-windows-x86_64.zip` 与 `.sha256` 并上传 Release。

## Windows 上的测试覆盖差异

以下契约在 Windows 上不做完整覆盖（Linux CI 保留全部场景）：

- **ABI guard 的分配失败注入**：`test-abi-guard` 通过全局
  `operator new` 覆写注入 bad_alloc/runtime_error。MSVC 静态 CRT 下，
  “worker 进程已附着时的 throw-from-operator-new”路径会触发 fast-fail
  或挂起，因此 `internal-error-injection`（worker 分支）、
  `request-path-oom` 与 `destroy-reaps` 场景在 Windows 上跳过；
  spawn 路径的注入 sweep（覆盖同一 guard 机制）仍然运行。
- **WPT 一致性套件**：WPT 工具链仅 Linux；`wpt_conformance_not_configured`
  的“响亮失败”门禁在 Windows 上不注册。
- **多 shard 池契约**：见上文 SO_REUSEPORT 限制（三个多 shard 场景
  不注册）。
- **m2 组的 POSIX 依赖场景**：`host_static_pool_activation_barrier` 用
  POSIX shell 脚本（mkdir/sleep/exec 语义）包装 worker，
  `host_static_pool_worker_exit_isolation` 需要 `/proc` 退出证据，
  两者在 Windows 上不注册；m2 组其余测试正常运行。
- **generation pool 的 kill-injection 场景**：`/proc/<pid>/status`
  不可用时无法验证“杀掉的 worker 被替换”路径；create/drain 与启动
  失败场景照常运行后，测试整体以 SKIP_RETURN_CODE 77 如实报告跳过
  （Windows 与 macOS 一致）。
- **A/B benchmark 证据契约**（`host_single_worker_ab_emits_complete_evidence`）：
  runner 是 POSIX shell 包装（`bench/run-ab.sh`），Windows 无可用 perf
  路径，按 77 跳过。
- **worker_package_\* 打包四件套**：默认 ctest 命令以 `-E` 排除（见
  上文构建步骤），CI 同样不跑；需要单独显式运行。

## 更新检查单

升级 txiki.js / 修改平台层时：

1. 本地与 CI 双跑 `windows-host-library` 矩阵；
2. 检查 `docs/linux-sandbox.md` 与本页的能力矩阵是否需要同步；
3. 若新增强制（如 fd 上限的 Windows 实现）落地，更新本页对应小节并
   重跑 sandbox 相关契约测试。
