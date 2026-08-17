# Architecture and Product Boundary

Capsid Runtime is a process-isolated JavaScript runtime built from a pinned txiki.js vendor tree. It targets embedded HTTP hosts and provides a versioned Minimum Common Web API subset; it does not claim full ECMA-429 conformance. Capsid is the only product name; external organization names are used only to identify standard sources and historical internal implementation.

## Deliverables and Application Model

The project produces two primary artifacts:

- `capsid-worker`: a sandboxable persistent JavaScript subprocess;
- `libcapsid_runtime`: a stable C ABI, plus a C++11 RAII wrapper in the headers.

Each worker loads a self-contained ESM bundle from memory. The regular path loads source; the host may also load trusted bytecode generated and validated by the exact same Capsid/QuickJS build. Bytecode is not a tenant input format and cannot serve as a sandbox boundary. Bundles must not depend on external, remote, or file modules, and must export one of the following forms:

```js
export default { fetch(request) { /* ... */ } }
```

```js
export function fetch(request) { /* ... */ }
```

The Runtime library does not provide an HTTP server, TLS termination, routing, worker pool, or tenant scheduling. Those responsibilities belong to the embedding host; the first-party C++ Host being developed in this repository is an independent host on that boundary and will not push these responsibilities back into the Runtime ABI.

It is also not a general-purpose POSIX application container: terminal readline, arbitrary TCP, long-running fswatch, and WebSocket server are outside the request-worker product surface. When persistent connections or background watchers are needed, the host owns the lifecycle; workers only receive bounded, cancelable input attributable to a request.

## Process and Data Paths

```text
Host HTTP/TLS, routing, worker pool, audit
                   │
                   │ FetchRPC v1 / platform worker transport
                   ▼
capsid-worker
  QuickJS-ng + libuv
  Capsid Web API bootstrap
  in-memory application bundle
  restricted native core
    ├─ timers / encoding / URL / streams / crypto / compression
    ├─ WAMR
    └─ DNS + TLS + HTTP client (standard fetch only)
```

Inbound requests and application responses pass through length-prefixed FetchRPC. Outbound `fetch()` calls made by the application use the worker's internal txiki.js HttpClient/libwebsockets directly, bypassing the host HTTP proxy or FetchRPC broker. Hostname targets resolve through the operating system resolver (`uv_getaddrinfo` — Windows DNS Client, nsswitch, `/etc/hosts`) before connect, rather than lws's internal raw-DNS client; numeric addresses and proxied targets connect directly as-is. The egress policy still inspects the hostname, every resolved address, and every redirect.

## Platform Contract

Platform support splits into two independent commitments: native development and production isolation.

| Platform | Native development target | Production contract |
| --- | --- | --- |
| Linux x86-64/AArch64 | Supported | strict sandbox, target for the v1 production release |
| macOS | Supported | v1 does not claim Linux-equivalent isolation; use Linux containers or VMs for production consistency |
| Windows x86-64 (MSVC) | Supported since v0.1.2 | v1 uses Linux containers/WSL2; native production isolation is validated separately |

Native development at minimum requires that the Host and worker can build and start on the target system and complete source/trusted bytecode loading, `capsid:env`, HTTP requests, streaming, cancel, worker replacement, and integration tests. Native development modes that do not yet have production-grade isolation must be enabled explicitly, may only bind to loopback, and must not be described as production isolation in documentation, logs, or READY status.

Platform boundaries keep a single responsibility. The host decides which capabilities it needs and verifies the actual features at READY. The runtime implements process creation, IPC, termination/reclamation, and OS sandboxing. Linux uses seccomp, Landlock, namespace, and cgroup; a future Windows production backend will use Windows native process and security mechanisms and must not misreport Job Object, Restricted Token, or AppContainer as a Linux feature bit. The deployment environment remains responsible for additional network boundaries; the Host does not create a privileged network supervisor.

For the current ABI v7, the worker event source is a Unix fd on POSIX and a CRT fd on Windows (backed by a loopback TCP socket); spawn/reap and the worker-event adapter are implemented per platform. The Windows native development toolchain has been delivered since v0.1.2. It covers source/trusted-bytecode identity, requests, streaming, cancel, crash/reap, and loopback-only negative controls. Multi-shard static-pool is available through a pool-level shared acceptor, while strict sandbox and the managed Host remain unavailable and `capsid:fs` is degraded (drive-letter absolute paths, reparse points rejected); see the [platform support overview](platform-support.md) and [Windows build and platform capabilities](windows.md). The first-party Host must not leak platform differences into pool, routing, or lifecycle; only the platform worker-event adapter may access fd/HANDLE directly. Trusted bytecode remains subject to compatibility identity, and bytecode built locally on Windows must not bypass identity validation to be deployed to an incompatible Linux worker.

## JavaScript Surface

The profile name is `CAPSID-MIN-2025-subset-v0`, targeting the WinterTC
**ECMA-429 Minimum Common Web API** (first edition, December 2025). It mainly
includes:

- Event, Abort, timers, microtask, and error/rejection reporting;
- Encoding, URL/URLSearchParams/URLPattern;
- Blob, File, FormData, Fetch, Streams, Compression;
- Web Crypto, Console, Performance;
- MessageChannel/MessagePort;
- WebAssembly Module, Instance, Memory, Table, Global, and compile/instantiate/validate (including streaming variants);
- `navigator.userAgent`.

Formal deviations and resource limits are documented in [standards and conformance](conformance.md). The following are never exposed: txiki.js `globalThis.tjs`, `tjs:internal/*`, process/child process, server, WASI, external module loading, REPL, file execution, and host IPC control.

Frameworks are just ordinary bundles. The current validation covers Hono 4.12.32, itty-router 5.0.24, and H3 2.0.1-rc.26; the runtime source contains no framework detection or special branches.

## Restricted Build

The current overlay directly excludes txiki.js's generic core bootstrap and dangerous builtin bytecode such as FFI, path, POSIX socket, readline, SQLite, and WASI in the restricted profile; any remaining native translation unit, even if it enters the static archive, must be proven absent from `capsid-worker` through link-time stripping and final-binary positive/negative control audits. Security claims are therefore supported by compile-time conditions, the module loader, and final artifact audits together, not by relying solely on "unreachable at runtime".

The final artifact retains QuickJS-ng, libuv, WAMR, the Web API implementation, and the DNS/TLS/HTTP client required for standard `fetch()`. mimalloc is optional and disabled by default. The vendor tree is not modified in place: CMake creates an overlay in the build directory and applies `patches/txiki/` in order.

## Security Boundary

The security policy is divided into mutually independent layers:

1. The build layer determines which capabilities exist;
2. The module loader determines whether a bundle can import them;
3. Capability/egress policy determines concrete resource operations;
4. Linux seccomp, Landlock, namespace, cgroup, and the host firewall provide the process boundary.

JavaScript cannot request or expand permissions on its own. The currently buildable modules include the read-only `capsid:permissions` and six pure utilities with no ambient authority: `capsid:assert`, `capsid:getopts`, `capsid:hashing`, `capsid:ipaddr`, `capsid:utils`, `capsid:uuid`. `capsid:env` only reads an immutable snapshot that the host explicitly provides, is authorized per key, and is isolated per worker; it does not read the process environment. `capsid:system` only returns compile-time version and feature flags; it does not collect host system information. `capsid:storage` provides an in-memory key-value store that is authorized by namespace, has a fixed quota, and lives only within a single worker; it does not touch disk. `capsid:stdio` only routes approved stdout/stderr messages into bounded IPC log events; it does not expose real fds or stdin. `capsid:fs` provides bounded reads constrained by path rules, Landlock, and `openat2`, and rejects all symlinks and write operations.
Every module still requires explicit host authorization; extensions not listed in the machine-readable available set remain unavailable. See [host capability policy](capability-policy.md) and [Linux strict sandbox](linux-sandbox.md) for the concrete contracts.

## Resource Policy

- A single Wasm linear memory is limited to 256 pages (16 MiB);
- A single Wasm table is limited to 1024 elements;
- IPC frames, headers, bundles, concurrent requests, queues, and per-request buffers all have explicit limits;
- A single `capsid:stdio` message is at most 16 KiB and is also bounded by the same IPC queue limit;
- `capsid:fs` files are at most 1 MiB each, and a single directory enumeration is limited to 1024 entries;
- Request/response bodies use a per-request-ID credit window;
- A synchronous CPU timeout makes the worker no longer reusable; an asynchronous timeout only cancels the corresponding request;
- Destruction escalates in a bounded sequence: graceful shutdown → SIGTERM → SIGKILL.

Deployers can also configure JS heap, process address space, fd, cgroup CPU/memory/PID, egress body size, and network policy. The host is responsible for publishing the actual resource and isolation policy based on workload.

## Vendor Update Principles

`vendor/txiki.js` and its recursive submodules must stay pinned and clean. When upgrading, you must:

1. Update the pinned version and regenerate the overlay;
2. Verify each patch applies;
3. Review native modules, global objects, and final-binary differences;
4. Run the full contract, WPT, framework, sandbox, and negative-control matrix;
5. Update conformance deviations and the upgrade report.

Overlay keys, stamps, actual-content manifests, and configure dependencies all use fail-closed validation; build trees of unknown origin or tampered with must not be reused.
