# Platform Support Overview

Capsid's commitments for Linux, macOS, and Windows are split into two independent levels:

- **Native development (native dev)**: the runtime, worker, bytecode compiler, and first-party Host can build, start, and pass platform-neutral tests natively on that platform;
- **Production isolation**: whether untrusted code can be run on that platform. Currently only the Linux strict sandbox fulfills this commitment.

Being able to build does not mean being able to isolate in production. Native builds on macOS and Windows are for development, integration debugging, CI, and benchmarks; production deployments involving untrusted code should use Linux containers or VMs.

## Unified Capability Contract (Three-Platform Intersection)

**single-worker and static-pool are committed to uniform behavior across the three platforms**: `static-pool`'s single and multi shard modes are available on Linux, macOS, and Windows with identical external behavior (same public port, same READY/drain/request contract). Linux/macOS share the port through kernel `SO_REUSEPORT`; Windows uses a pool-level shared acceptor that polls and dispatches. Only capabilities whose behavior is consistent across all three platforms enter the ✅ list; any capability that is missing, semantically inconsistent, or only partially implemented on any platform is listed separately as a difference.

Unified commitments:

- Runtime C ABI (spawn/request/credit/streaming) ✅
- `capsid-worker` (txiki.js + restricted core) ✅
- `capsid-bytecode-compile` (trusted bytecode) ✅
- Host `--mode single-worker` ✅
- Host `--mode static-pool` (single / multi shard) ✅
- Egress network policy (egress host/address rules) ✅
- Capability policy (modules/permissions/environment snapshot) ✅

**Not part of the three-platform unified contract** (therefore treated as unsupported unless a single-platform capability is explicitly used):

- Host `--mode managed` (coordinator/Admin/multi-App): Linux only; macOS/Windows exit after a direct CLI message
- `capsid:fs` (read/stat/list): fully supported on Linux; degraded but usable on macOS/Windows, and always rejects symlink/reparse paths (macOS: `openat(O_NOFOLLOW)` dirfd walk; Windows: drive-letter absolute paths, reparse points rejected)
- strict sandbox (seccomp/Landlock/namespace/cgroup): Linux only
- Multi-shard shared port (`SO_REUSEPORT`): internal implementation only on Linux/macOS; Windows uses a pool-level shared acceptor to achieve the same external behavior
- Worker CPU affinity: full on Linux; current processor group only on Windows; none on macOS
- `RLIMIT_AS` / `RLIMIT_NOFILE` / `RLIMIT_CORE`: semantics differ by platform, so there is no unified commitment
- Secure `bindingsRoot` scanning for local Binding development: Linux/macOS
  (POSIX owner/mode + fd-relative scan) and Windows native-dev
  (reparse-point/hard-link rejection, current-user ownership, Everyone/Users
  writable ACL rejection). Windows runs the Binding Runtime and native gates,
  but Linux sandbox profile enforcement remains Linux-only

For consistent cross-platform behavior, rely only on the ✅ list; before using any capability from the differences list, branch by platform or pin the deployment to an explicitly supported platform.

## Platform Builds

### Linux

- x86-64 or AArch64 recommended; Release uses the musl fully static package `capsid-<version>-linux-musl.tar.gz`;
- `CAPSID_ENABLE_LTO`, `CAPSID_ENABLE_ASAN/UBSAN/TSAN`, fuzz, and `CAPSID_GENERATE_LINK_MAP` are available;
- strict sandbox requires the kernel to provide seccomp and Landlock; cgroup/namespace capabilities are delegated by the host. See [Linux strict sandbox](linux-sandbox.md) for the detailed contract.

### macOS

- Built natively with the system Clang; the Release package is `capsid-<version>-darwin-arm64.tar.gz`;
- single-worker and static-pool (single / multi shard) are part of the three-platform unified contract; multi shard uses `SO_REUSEPORT`;
- `capsid:fs` is degraded but usable: readText/stat/list behave consistently with Linux, implemented as a component-by-component `openat(O_NOFOLLOW)` dirfd walk; symlinks are always rejected;
- There is no API equivalent to `sched_setaffinity`; CPU affinity tests are skipped with CTest 77;
- `--strict-sandbox on` fails directly at the CLI ("requires Linux strict sandbox"); `--mode managed` fails directly at the CLI with the message "managed coordinator requires Linux strict sandbox", matching Windows behavior. A programmatic spawn that still requests strict sandbox fails during the worker startup handshake (defense in depth).

### Windows

- Built with MSVC + Ninja + vcpkg (static CRT); the Release package is `capsid-<version>-windows-x86_64.zip`;
- single-worker and static-pool (single / multi shard) are part of the three-platform unified contract; there is no `SO_REUSEPORT`, and multi shard is dispatched by a pool-level shared acceptor;
- `capsid:fs` is degraded but usable: readText/stat/list behave consistently with Linux; paths accept only `C:/...` (also `C:\...`) drive-letter absolute paths, opened component by component, and reparse points (symlink/junction) are rejected; UNC paths are not supported;
- strict sandbox and managed Host are unavailable; `--mode managed` fails directly at the CLI with a message, matching macOS behavior;
- CPU affinity is implemented via `SetProcessAffinityMask`, but only covers the current processor group;
- The worker memory limit constrains **committed memory** via Job Object's `JOB_OBJECT_LIMIT_PROCESS_MEMORY`, which is semantically different from Linux `RLIMIT_AS` (virtual address space). See [Windows build and platform capabilities](windows.md) for full prerequisites and differences.

## CI Coverage

`.github/workflows/testing-validity.yml` provides five categories of hosted evidence:

- Ubuntu 24.04 Release/LTO + pinned WPT + delegated sandbox;
- Ubuntu ASan, UBSan, TSan;
- Four Clang/libFuzzer corpus gates;
- macOS 14 posix-host-library;
- `windows-latest` MSVC host-library (platform-neutral matrix + JUnit evidence).

Unsupported scenarios on each platform follow the "skip unless registered" principle or CTest `SKIP_RETURN_CODE 77`; silent green must not substitute for missing coverage. See [testing and continuous gate](testing.md) for the detailed list.

## Selection Recommendations

| Scenario | Recommended platform |
| --- | --- |
| Local development and AI code-generation integration | Linux, macOS, or Windows, any |
| Benchmark reproduction | Linux (release package matches the performance baseline) |
| Running untrusted code | **Linux only**, with strict sandbox enabled and READY feature bits verified |
| Need a Windows native host | Windows single-worker / single shard static-pool, trusted code only |
| Need managed multi-App / Admin / blue-green deployment | Linux |

When facts conflict, public headers, `docs/capability-manifest.json`, build configuration, and CI workflows take precedence; Markdown is only for navigation and explanation.
