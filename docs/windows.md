# Windows Build and Platform Capabilities

> For a cross-platform capability overview and selection guidance, see [Platform Support Overview](platform-support.md).

Capsid v0.1.2+ provides Windows x86_64 support: the Runtime static library, `capsid-worker`, `capsid-bytecode-compile`, and the first-party `capsid-host` (`--mode single-worker` / `static-pool`) build natively under MSVC and ship with Releases as `capsid-<version>-windows-x86_64.zip`. This page describes build prerequisites, the capability matrix, and known limitations.

Linux's strict sandbox semantics (seccomp/Landlock/namespace/cgroup) have no Windows equivalent; deployments that need a full isolation boundary should continue using the Linux package ([linux-sandbox.md](linux-sandbox.md)).

## Build

### Runtime stack budgets

MSVC executables embedding TJS reserve 8 MiB of native stack, matching the
upstream txiki CLI. This provides room to construct and catch a JavaScript
stack-limit exception before the operating-system stack is exhausted. The
public `capsid_worker_config_init()` JavaScript stack default remains 1 MiB.
`worker_js_stack_limits_default` and `worker_js_stack_limits_262144` exercise
actual recursive calls, catch `RangeError`, and then serve another request,
twice, under the default and an explicit 256 KiB JavaScript limit.

The Windows ASan Hono functional profile explicitly launches with a 2 MiB
JavaScript stack because instrumented interpreter frames consume more native
stack. Its CTest commands and driver logs record this budget. Ordinary builds
use 1 MiB; both profiles run the same response and lifecycle assertions.
Passing the ASan functional profile does not establish that Hono fits within
the 1 MiB ASan budget. Default-limit recovery remains a separate required test.

### Consuming an extracted package

The archive contains `CapsidConfig.cmake` and a version file, so an independent
CMake project can consume the exported runtime target:

```cmake
cmake_minimum_required(VERSION 3.18)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(my_capsid_app LANGUAGES C CXX)
find_package(Capsid CONFIG REQUIRED)
add_executable(my_app main.cc)
target_compile_features(my_app PRIVATE cxx_std_17)
target_link_libraries(my_app PRIVATE Capsid::capsid_runtime)
if(Capsid_RUNTIME_DLLS)
    add_custom_command(TARGET my_app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${Capsid_RUNTIME_DLLS} "$<TARGET_FILE_DIR:my_app>"
        VERBATIM)
endif()
```

Configure with `-DCMAKE_PREFIX_PATH=<extracted-package>` and use a build
configuration matching the package. The C header also needs the C++ linker
because the runtime library is implemented in C++; enable both languages for
a C executable. Sanitizer compilation requirements come from the exported
target.

Ordinary Windows packages use the static CRT. An MSVC ASan package additionally
carries its selected toolchain's ASan DLL and the two VC runtime DLLs it
imports. `Capsid_RUNTIME_DLLS` exposes these files for an embedding application;
the packaged worker/compiler/Host load them from `bin`. Their names, compiler
identity and SHA-256 values are recorded in `share/capsid/asan-runtime.json`,
the file manifest and the SBOM. `find_package` checks the DLL checksums after
relocation. Package smoke tests run with the developer toolchain removed from
PATH and verify that the consumer actually loaded the three local DLLs.

This follows the current [MSVC ASan runtime model](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-runtime?view=msvc-170):
even `/MT` instrumented programs require the ASan runtime DLL. A nonstandard
toolchain layout can supply `CAPSID_MSVC_ASAN_RUNTIME`; the selected DLL and its
VC runtime companions must come from the matching toolchain directory.

### Prerequisites

- Windows 10 1803+ (IPC between worker and Host uses Winsock loopback sockets; an Admin socket using AF_UNIX also requires 1803+ when enabled. All official tests run on the `windows-latest` runner (Windows Server 2022+));
- Visual Studio 2022+, including the "Desktop development with C++" workload (MSVC + Windows SDK);
- CMake ≥ 3.18 and Ninja (bundled with the VS installer, or installed separately);
- Node.js **24.14.0** (the locked runtime for the WebSocket comparison tests;
  also used by `npm ci` to provide esbuild);
- CPython **3.12.3** with `venv` and `pip` for the locked independent
  WebSocket tests; an embedded distribution without `venv` is insufficient for
  the setup command below;
- An OpenSSL CLI on PATH for test certificates (for example Git for Windows'
  `mingw64/bin/openssl.exe`); this is separate from the linked vcpkg library;
- vcpkg (provides static OpenSSL, c-ares and Boost headers).

### Steps

```powershell
# 1. Dependencies: static OpenSSL and Boost.System/Asio (Boost ≥1.87 is header-only;
#    boost-asio is a separate port; the Host data plane needs <boost/asio.hpp>)
vcpkg install openssl c-ares boost-system boost-asio boost-beast --triplet x64-windows-static

# 2. Lock JS dependencies (esbuild)
npm ci --ignore-scripts --prefix vendor/txiki.js
npm ci --ignore-scripts --prefix examples/hono-reference
npm ci --ignore-scripts --prefix examples/itty-router-reference
npm ci --ignore-scripts --prefix examples/h3-v2-reference
npm ci --ignore-scripts --prefix examples/elysia-reference

# 3. Install the locked independent clients, using the full Python 3.12.3
#    installation and Node 24.14.0 on PATH. The setup verifies package digests.
$interop_root = Join-Path $env:TEMP 'capsid-ws-interop'
python tools/setup-ws-interop-python.py $interop_root
if ($LASTEXITCODE -ne 0) { throw 'WebSocket client setup failed' }
$env:CAPSID_WS_INTEROP_PYTHON = Join-Path $interop_root 'Scripts/python.exe'
$env:CAPSID_WS_INTEROP_NODE_PACKAGE = Join-Path $interop_root 'node-ws'
openssl version
if ($LASTEXITCODE -ne 0) { throw 'OpenSSL CLI for fixtures is unavailable' }

# 4. Configure (the static CRT and x64-windows-static triplet must match)
cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=cl `
  -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_BUILD_TYPE=Release `
  -DCAPSID_BUILD_HOST=ON `
  -DCAPSID_BUILD_WORKER=ON `
  -DCAPSID_STRICT_WARNINGS=ON `
  -DCAPSID_USE_MIMALLOC=OFF `
  "-DCAPSID_WS_INTEROP_PYTHON=$env:CAPSID_WS_INTEROP_PYTHON" `
  "-DCAPSID_WS_INTEROP_NODE_PACKAGE=$env:CAPSID_WS_INTEROP_NODE_PACKAGE" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static

# 5. Build and execute the current native CI profile
cmake --build build --parallel 2
ctest --test-dir build --no-tests=error --output-on-failure -E '^wpt_conformance_not_configured$'

# 6. Package (the build command does not establish package smoke acceptance)
cmake --build build --target package   # Produces build/capsid-<version>-windows-x86_64.zip + .sha256
```

The current native test failures and package fixture limits are recorded in the
[takeover audit, section 33](v0.3.0-glm-takeover-review.md#33-2026-10-16-测试入口原生-windows-与仍然失败的验收).
A successful build or registration check does not establish complete Windows
WebSocket interoperability. The current profile executes the package gates;
keep all failed and skipped results visible. Earlier audit profiles that
excluded four package tests retain their original scope and results.

Configuration notes:

- **Static CRT**: the top-level `CMakeLists.txt` pins `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` under MSVC. The vendored txiki.js and its dependencies (libwebsockets/mbedtls, etc.) also pin the static CRT; mixing `/MD` fails at link time with LNK2038.
- **MSVC compiler**: in a shell without vcvars (CI, ordinary terminal), Ninja picks up MinGW gcc from PATH and cannot build the CRT/winsock portability layer; you must explicitly pass `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl`. When a non-MSVC compiler is configured, the top-level CMakeLists emits a FATAL_ERROR directly.
- **iconv**: Windows has no system iconv; the build uses the repository-bundled [win-iconv](../vendor/win-iconv/VENDOR.txt) (public domain, a Win32 MultiByteToWideChar implementation). Linux/macOS continue to use the system iconv.
- **txiki overlay**: the build copies the vendored txiki.js into the build tree and applies patches. Machines without Developer Mode enabled cannot create symbolic links, so `PrepareTxiki.cmake` automatically falls back to copying the whole directory (content is identical; the overlay key hashes only file contents).
- **OpenSSL/Boost/c-ares**: provided through the vcpkg `x64-windows-static` triplet; this project does not perform configure-time downloads (continuing the "no FetchContent" constraint from build_host.cmake).
- **LTO is forcibly disabled on Windows**: MSVC's `/GL` binds `operator new/delete` at compile time, so overrides of globally replaceable symbols (which ABI guard allocation-failure injection relies on) no longer intercept IPO'd translation units. GCC/Clang LTO preserves replaceable symbols, so POSIX builds are unaffected; build identity feature flags faithfully record `lto=OFF`.

### Known working configurations

- `CAPSID_BUILD_WORKER=ON` + `CAPSID_BUILD_HOST=ON` (Release): the release matrix configuration (LTO is always OFF on Windows, see above);
- `CAPSID_STRICT_WARNINGS=ON` (default): `/W4 /WX` under MSVC;
- `CAPSID_ENABLE_ASAN=ON`: MSVC `/fsanitize=address` (requires `CAPSID_USE_MIMALLOC=OFF`).

### Unavailable configurations (configure-time error)

- `CAPSID_ENABLE_UBSAN` / `CAPSID_BUILD_FUZZERS`: not supported by MSVC (`CMakeLists.txt` explicitly rejects them);
- `CAPSID_ENABLE_TSAN`: Linux only;
- `CAPSID_GENERATE_LINK_MAP`: Linux GNU/Clang only.

## Platform capability matrix

| Capability | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Runtime C ABI (spawn/request/credit/streaming) | ✅ | ✅ | ✅ |
| `capsid-worker` (txiki.js + restricted core) | ✅ | ✅ | ✅ |
| `capsid-bytecode-compile` (M1D trusted bytecode) | ✅ | ✅ | ✅ |
| Host `--mode single-worker` / `static-pool` | ✅ | ✅ | ✅ (multi-shard via pool-level acceptor) |
| Host `--mode managed` (coordinator/Admin/multiple Apps) | ✅ | ❌ (runtime notice) | ❌ (runtime notice) |
| Egress network policy (egress host/address rules, protected segments) | ✅ | ✅ | ✅ (JS layer) |
| Capability policy (module/permission/environment snapshot) | ✅ | ✅ | ✅ |
| fs permission module (capsid:fs read/stat/list) | ✅ full | ⚠️ degraded (dirfd walk, symlink rejection) | ⚠️ degraded (drive-letter paths, reparse rejection) |
| RLIMIT_AS / RLIMIT_NOFILE / RLIMIT_CORE | ✅ | partial (RLIMIT_AS rejected at compile time) | partial (see below) |
| strict sandbox (seccomp/Landlock/namespace/cgroup) | ✅ | ❌ | ❌ (see below) |
| Multi-shard shared port (SO_REUSEPORT) | ✅ | ✅ | ❌ (pool-level acceptor distribution instead) |
| worker CPU affinity (`capsid_worker_set_cpu_affinity`) | ✅ | ❌ | ✅ (within current processor group) |

### worker sandbox semantics (Windows)

`apply_sandbox` behavior on Windows:

- **Process memory limit** (`process_memory_limit`): enforced in the worker process via a Job Object (`JOB_OBJECT_LIMIT_PROCESS_MEMORY`). Note the semantic difference: Linux's RLIMIT_AS limits virtual address space, while Windows' job limit limits **committed memory** — neither working set nor virtual address space, and the two are not equivalent. If `AssignProcessToJobObject` fails (for example, the worker is already assigned to an outer job such as a CI runner), the worker refuses to start rather than silently degrading;
- **File descriptor limit**: Windows has no process-level handle count limit. A non-zero `file_descriptor_limit` is **accepted but not enforced** (the default value 64 falls in this category too). The applied features deliberately omit the corresponding `CAPSID_SANDBOX_FEATURE_RLIMITS` bit, so a host that requested the feature sees it missing during hello validation and fails rather than assuming enforcement;
- **Core dump suppression** (RLIMIT_CORE equivalent): `SetErrorMode` suppresses WER crash dialogs, so worker crashes do not block the foreground session;
- **strict mode**: on Linux, strict is an available capability; on Windows/macOS the Host CLI fails `--strict-sandbox on` directly at the argument phase ("requires Linux strict sandbox", matching `--mode managed`). A programmatic spawn with `strict_sandbox=true` still returns success — the worker, after starting and discovering it cannot impose a strict sandbox, asynchronously reports "strict sandbox is unavailable on this platform/build" during the startup handshake and exits with exit code 1, and the host detects the failure through the worker exit event rather than the spawn return value. Strict-only parameters are rejected at spawn time: a network namespace fd returns `CAPSID_INVALID_ARGUMENT` on non-Linux platforms; a cgroup path causes the child process to be terminated immediately after startup, and spawn also returns `CAPSID_INVALID_ARGUMENT`. macOS strict semantics match Windows (see the managed entry under Host below).

### Host (Windows)

- **single-worker / static-pool fully available**: single and multi-shard modes share the same external contract as Linux/macOS. Windows has no `SO_REUSEPORT`; multi-shard uses a pool-level shared acceptor bound to the public port and distributes connections round-robin to each shard, so clients see the same port and the same READY and drain contract. Process stop signal: Windows has no `sigwait`; `static-pool` uses a console control handler (CTRL_C/CTRL_BREAK/close) to trigger the same stop path; `single-worker` uses Boost.Asio `signal_set` (also available on Windows).
- **Multi-shard scenario tests** (`host_static_pool_server_shared_port_lifecycle`, `host_admission_pool_forwards_options`, `host_concurrent_pool_wait`) are registered and run on Windows; single-shard drain scenarios are also registered. Only two m2-group scenarios are not registered because of POSIX dependencies; see "Test coverage differences on Windows" below.
- **Parent-controlled normal exit**: local modes accept an optional `--stop-fd` pointing to an inherited readable pipe, distinct from `--ready-fd`. Writing any byte or closing the parent's end requests the same normal stop as console events. The Host owns a non-inheritable duplicate and cancels its watcher on startup failure. Active inbound WebSockets receive a 1001 Close and have at most one second to reply before their transport closes; Workers are then stopped and joined. Node `child.kill('SIGTERM')` on Windows terminates the process and does not exercise this normal shutdown path. The test backend uses the explicit parent pipe and still requires exit code 0, no signal, and no forced cleanup.
- **managed mode**: the Windows build keeps the `--mode managed` CLI entry, but the runtime directly prints "managed coordinator requires Linux strict sandbox" and exits, matching macOS behavior; the coordinator implementation remains Linux-only and is not included in the Windows package. Process snapshots (RSS/CPU) on Windows are provided through `GetProcessMemoryInfo`/`GetProcessTimes` (PSS has no equivalent; RSS is the fallback).
- **File owner/permission checks** (trusted key store, deployment reads, state directory): Windows has no uid/mode bits, so owner checks are skipped; the boundary is provided by NTFS ACLs, and deployment reads retain the reparse-point (symlink/junction) rejection semantics.

### Binding v1 (Windows)

- **Local Binding development is supported**: `single-worker` and `static-pool` accept `--bindings-root` and run the same production registry scanner, manifest validator, manifest/App intersection, `LOAD_BINDING` ordering, Binding Runtime, and `capsid:binding/*` facade as Linux/macOS.
- **Registry trust boundary**: Windows rejects reparse points (symlink/junction) and hard links, requires the root/package/file owner to be the current user, rejects Everyone/Users writable ACLs, enforces the 1 MiB manifest / 16 MiB source / 64 MiB aggregate limits, and rechecks identities after every read.
- **Sandbox profiles are Linux-only**: a Windows Binding package may declare profiles so the same package can run under Linux, but Windows does not enforce seccomp/Landlock. Packages must keep `sandbox.requires` empty when Windows-native behavior is the only environment, and profile enforcement must be validated on Linux.
- **Managed mode remains unavailable** on Windows, so Binding `secrets.valueFrom` is rejected in local modes (there is no managed secret provider).
- Tests: `host_binding_registry_win` (registry security fixtures) and `worker_binding_windows_smoke` (real worker LOAD_BINDING → Binding Runtime → facade response) run in the `windows-host-library` job; the full POSIX zero-binding regression stays Linux/macOS-only.

### Worker internal differences

- **IPC transport**: host↔worker IPC switches from `socketpair(AF_UNIX)` to a loopback TCP socket pair. Child process handles are explicitly inherited through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (only the IPC socket and on-demand stdio cross the process boundary, equivalent to POSIX close_range cleanup). `--ipc-fd` carries a handle value rather than an fd number. An accepted socket must call `SO_UPDATE_ACCEPT_CONTEXT` to rebind the listener context before the listener is closed: otherwise, after AFD reclaims the listener's endpoint name, subsequent I/O on the accepted socket fails randomly with `ERROR_ALREADY_EXISTS` (both ends of the socket pair can be poisoned).
- **fs module (degraded but usable)**: on Windows, `capsid:fs` readText/stat/list are available. Paths must be drive-letter absolute paths (`C:/...` or `C:\...`, uniformly normalized to `C:/...`; UNC is not supported); the open process uses `FILE_FLAG_OPEN_REPARSE_POINT` per component and rejects any symlink/junction. Linux uses `openat2(RESOLVE_NO_SYMLINKS)`, macOS uses `openat(O_NOFOLLOW)` dirfd walk, the three platforms share the same external contract, and symlink/reparse paths are always rejected.
- **Connection termination semantics**: on Windows, reads on a peer-reset socket end with EOF (returning 0) (`WSAECONNRESET` is folded into the normal close path), so the host observes an EXIT event when a worker terminates abnormally; Linux can distinguish CLOSED from ERROR. WSAPoll does not report `POLLHUP`; a connection close can only be detected by a read returning 0.
- **Startup handshake timeout**: the non-blocking retry path for protocol reads during worker startup has a 2s cap (unbounded on the POSIX side); if the startup fd itself is blocking, both platforms wait indefinitely.
- **Process termination exit code**: `TerminateProcess` always reports exit code 1 (the semantic equivalent of SIGKILL), unlike Linux's signal-mapped 128+N exit codes — this difference is only visible in tests that inspect exit codes.
- **TextDecoder**: arbitrary encoding→UTF-8 conversion in the restricted core is provided by win-iconv on Windows. win-iconv does not perform strict encoding validation (its readme states this explicitly), so TextDecoder semantics may differ within the allowed range from Linux (glibc iconv).

## CI

- The `windows-host-library` job in `.github/workflows/testing-validity.yml` builds Runtime/worker/Host and runs all platform-neutral tests (Linux-only tests are absent per the "not registered means skipped" or `SKIP_RETURN_CODE 77` principle, following the same strategy as macOS), produces JUnit evidence, and participates in the `hosted-evidence-index` hard gate.
- The Windows matrix entry in `.github/workflows/release.yml` produces `capsid-<version>-windows-x86_64.zip` and `.sha256` and uploads them to the Release.

The Worker process receives only `SystemRoot`, resolved through the operating
system API. It does not copy the parent's value or other process variables.
Winsock needs this value to load its provider; application environment values
continue to arrive in the HELLO snapshot. `worker_windows_environment` checks
actual v3/v4 startup and requests with a deliberately incorrect parent value,
and uses a separate native peer to check the physical environment entries.

The ABI guard executes its attached-worker `runtime_error`, request OOM and
destroy-after-OOM scenarios on Windows too. After correcting the private
request helper's C++ linkage, the local MSVC 14.50 static-CRT build passes
171 checks with these branches enabled. Windows LTO remains OFF so the global
allocation override is effective. Hosted execution remains a separate gate.

## Test coverage differences on Windows

The following contracts are not fully covered on Windows (Linux CI retains all scenarios):

- **WPT conformance suite**: the native Node/MSVC toolchain supports the same manifest-pinned 84 files and audits every upstream input against its Git blob. Set `CAPSID_WPT_ROOT` to the pinned checkout when configuring; an unconfigured Windows build still omits `wpt_conformance_not_configured` and provides no WPT evidence. The local qualification now builds the fixtures on Windows rather than importing Linux bundles. The Windows hosted job still needs this configuration and an evidence audit before its WPT coverage can be claimed.
- **m2-group POSIX-dependent scenarios**: `host_static_pool_activation_barrier` wraps the worker with a POSIX shell script (mkdir/sleep/exec semantics), and `host_static_pool_worker_exit_isolation` needs `/proc` exit evidence; both are not registered on Windows. Multi-shard and single-shard static-pool scenarios are already registered and run on Windows.
- **generation pool kill-injection scenario**: without `/proc/<pid>/status`, the "killed worker is replaced" path cannot be verified; after the create/drain and startup-failure scenarios run normally, the test reports the skip honestly with SKIP_RETURN_CODE 77 overall (same on Windows and macOS).
- **A/B benchmark evidence contract** (`host_single_worker_ab_emits_complete_evidence`): the runner is a POSIX shell wrapper (`bench/run-ab.sh`), and Windows has no usable perf path, so it is skipped with 77.
- **worker_package_\* bundling trio** (`contents` / `smoke` / `reproducibility`): excluded by the default ctest command with `-E` (see the build steps above), and CI does not run them either; they must be run explicitly. `worker_install_tree` is not in that exclusion regex and runs by default.

## Update checklist

When upgrading txiki.js / modifying the platform layer:

1. Run the `windows-host-library` matrix both locally and in CI;
2. Check whether `docs/linux-sandbox.md` and this page's capability matrix need to be synchronized;
3. If new enforcement (such as a Windows implementation of the fd limit) lands, update the corresponding section on this page and rerun the sandbox-related contract tests.
