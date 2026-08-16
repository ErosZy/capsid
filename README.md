![Capsid](logo.png)

[![Testing validity](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml/badge.svg)](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml)
[![Release](https://img.shields.io/github/v/release/ErosZy/capsid?label=release)](https://github.com/ErosZy/capsid/releases)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**An embeddable JavaScript runtime for isolated, capability-based execution. Run untrusted and AI-generated code inside your application**: the host manages `capsid-worker`
processes through `libcapsid_runtime`, each worker loads exactly one
self-contained ESM, and serves HTTP requests over streaming FetchRPC. The
runtime does not listen on ports, terminate TLS, or manage routing; those
belong to the host.

> **Status**: `0.1.x`, ABI v7. The first-party `capsid-host` is a
> development/benchmark entry point, not a production deployment interface;
> production isolation is only promised by the Linux strict sandbox.

## Why Capsid

- Put untrusted/AI-generated Fetch handlers into separate workers and
  constrain behavior with capability whitelists, resource limits, and audit
  events;
- process-level failure boundary: crashes, timeouts, and reclamation are
  controlled by the host;
- least privilege: modules, env, fs, storage, stdio, and egress network are
  all explicitly authorized and denied by default;
- host data plane: C ABI / C++11 RAII, non-blocking IPC, credit backpressure,
  cancellation, and streaming;
- high performance: on a 4-core benchmark, 2 workers sustain about
  **6,800 QPS**, roughly 1.5× a same-machine Flask app and 3.7× Slim;
- fast cold start: a small bundle takes about **8–10 ms**; a ~1 MB bundle
  using trusted bytecode takes about **42 ms**;
- low resident memory: Host + 2 workers idle PSS is about **12.3 MB**;
- auditable: pinned WPT, framework differentials, sanitizers, fuzz, and
  identity-backed performance evidence.

## Standards and Frameworks

Capsid targets **ECMA-429 Minimum Common Web API** — the WinterTC (ECMA TC55)
specification, first edition, December 2025 — through the profile
`CAPSID-MIN-2025-subset-v0`. Conformance evidence is a pinned Web Platform Tests
revision plus process-level regressions; Capsid does not claim full ECMA-429
coverage beyond this profile. See
[standards and conformance](docs/conformance.md).

Frameworks that compile to a single self-contained ESM exporting a standard
`fetch(request)` handler are the supported integration path, provided they avoid
Node/server adapters, listeners, filesystem static serving, and provider-specific
bindings. The compatibility suite pins and continuously verifies **Hono 4.12.32**,
**itty-router 5.0.24**, and **H3 v2 2.0.1-rc.26**; other Web-standard frameworks
can be evaluated against the same rules, but only pinned versions carry evidence.
See [framework compatibility](docs/framework-compatibility/README.md).

## Quick Start

### 1. Application

```js
export default {
  async fetch(request) {
    return Response.json({
      message: "hello from Capsid",
      path: new URL(request.url).pathname,
    });
  },
};
```

### 2. Build

Linux / macOS:

```sh
git submodule update --init --recursive
npm ci --ignore-scripts --prefix vendor/txiki.js
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DCAPSID_BUILD_HOST=ON
cmake --build build-release --parallel
```

Windows (PowerShell + MSVC + vcpkg):

```powershell
vcpkg install openssl boost-system boost-asio boost-beast --triplet x64-windows-static
cmake -S . -B build-release -G Ninja `
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_BUILD_TYPE=Release -DCAPSID_BUILD_HOST=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-release --parallel
```

### 3. Bundle and Run

For a single-machine run, explicitly using a least-privilege `capsid.json` is
recommended: when the file is absent, the baseline is deny-all, and you add
allows item by item when `capsid:*` modules or egress `fetch` are needed.

```json
// capsid.json
{
  "apiVersion": "capsid/app-v1",
  "permissions": {
    "modules": [],
    "fetch": { "allow": [] }
  },
  "pool": { "minReady": 1, "maxWorkers": 1 }
}
```

```sh
npx esbuild app.js --bundle --format=esm \
  --platform=neutral --target=esnext --outfile=app.bundle.js

./build-release/capsid-host --mode single-worker \
  --worker ./build-release/capsid-worker \
  --source-bundle app.bundle.js \
  --source-name "file://$PWD/app.bundle.js" \
  --application orders --listen 127.0.0.1:8080 \
  --routing path --public-scheme http \
  --capsid-json ./capsid.json
```

```sh
curl http://127.0.0.1:8080/@capsid/orders/
# {"message":"hello from Capsid","path":"/"}
```

`capsid-host` supports `single-worker`, `static-pool`, and `managed`;
step-by-step permission field configuration is in the
[capsid.json tutorial](docs/capsid-json.md).

## Configuration Guide

Application permissions are written in `capsid.json`; `managed` mode adds a
Host-authoritative `host.json`.

```json
// capsid.json — what capabilities the application requests
{
  "apiVersion": "capsid/app-v1",
  "permissions": {
    "modules": ["capsid:env"],
    "fetch": { "allow": ["api.example.com"] }
  },
  "pool": { "minReady": 1, "maxWorkers": 1 }
}
```

```json
// host.json — managed mode: what the Host allows, where data lives
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/applications",
  "stateRoot": "/srv/capsid/state",
  "secretRootTemplate": "/srv/capsid/secrets/{application}",
  "admin": { "unix": "/run/capsid/admin.sock", "mode": "0600" }
}
```

See [docs/capsid-json.md](docs/capsid-json.md) for the `capsid.json`
tutorial, and [docs/host-config.md](docs/host-config.md) for `host.json`
fields.

## Integration Model

The host links `libcapsid_runtime` and manages the listener, TLS, routing,
and pool lifecycle itself:

```c
#include <capsid/runtime.h>

capsid_worker_config config;
capsid_worker_config_init(&config);
config.worker_path = "/opt/capsid/bin/capsid-worker";
config.request_timeout_ms = 5000;

capsid_worker *worker = NULL;
capsid_result result = capsid_worker_spawn(&config, &worker);
```

Install the header `<capsid/runtime.h>` and the C++11 wrapper
`<capsid/runtime.hpp>`:

```sh
cmake --install build-release --prefix "$PWD/dist"
```

Or embed Capsid into the host build:

```cmake
add_subdirectory(path/to/capsid EXCLUDE_FROM_ALL)
target_link_libraries(my_gateway PRIVATE capsid::runtime)
```

The full READY/credit/streaming/cancel contract is described in the
[host embedding specification](docs/host-integration.md).

## Permissions and Security

Least privilege by default: without a capability policy, `capsid:*` modules cannot be imported. When `egress_policy == NULL`, all egress Fetch requests are denied. `strict_sandbox` is off by default, and the default configuration is only suitable for trusted code.

Authorization goes through three gates: build-time capabilities → module
whitelist → resource allow/deny rules; Host limits and application requests
are intersected.

Current public modules (each requires explicit authorization):

- Policy-constrained: `capsid:env`, `capsid:fs`, `capsid:stdio`,
  `capsid:storage`, `capsid:system`
- Permission query: `capsid:permissions`
- Pure utilities: `capsid:assert`, `capsid:getopts`, `capsid:hashing`,
  `capsid:ipaddr`, `capsid:utils`, `capsid:uuid`

`tjs:*` modules cannot be enabled through configuration. Linux production environments must explicitly enable the strict sandbox and verify that `CAPSID_EVENT_READY.flags` contains the sandbox features required by the deployment. See [Linux strict sandbox](docs/linux-sandbox.md), [capability policy](docs/capability-policy.md), and [security policy](SECURITY.md).

## Performance

4-core benchmark (Ryzen 3300X, Alpine v3.24/WSL2):

| Dimension | Capsid | Comparison |
| --- | ---: | ---: |
| JSON 1 KiB throughput | **6,820 QPS** | Flask 4,625 · Slim 1,826 |
| Small bundle cold start | **8–10 ms** | Node 110 ms · Deno 39 ms |
| 1 MB trusted bytecode cold start | **42 ms** | Node 149 ms · Deno 53 ms |
| Host + 2 workers idle PSS | **12.3 MB** | Python 3 stack 62.6 MB |

Full methodology, 12 workloads, and evidence rules are in
[performance-benchmarks.md](docs/performance-benchmarks.md).

## Platform Support

- **Linux**: full support. `single-worker` / `static-pool` (multi-shard) /
  `managed` are available; strict sandbox and `capsid:fs` are complete.
  **For production, run untrusted code only on Linux.**
- **macOS**: development only. Runtime, worker, bytecode compiler, and the
  single/static-pool Host are available; `capsid:fs` is degraded (symlinks
  are rejected); strict sandbox and `managed` are unavailable, and
  `--mode managed` prints a notice and exits at runtime.
- **Windows**: development only (MSVC, since v0.1.2). Runtime, worker,
  bytecode compiler, and the single/static-pool Host are available;
  multi-shard static-pool is distributed by a pool-level acceptor;
  `capsid:fs` is degraded (`C:/...` paths only, reparse points are rejected);
  strict sandbox and `managed` are unavailable, and `--mode managed` prints a
  notice and exits at runtime.

The full matrix and build requirements are in
[docs/platform-support.md](docs/platform-support.md).

## Documentation Index

| Topic | Entry |
| --- | --- |
| Architecture & boundaries | [architecture.md](docs/architecture.md) |
| Platform differences | [platform-support.md](docs/platform-support.md) · [windows.md](docs/windows.md) |
| Host embedding | [host-integration.md](docs/host-integration.md) |
| Configuration & permissions | [host-config.md](docs/host-config.md) · [capsid-json.md](docs/capsid-json.md) |
| Security & sandbox | [capability-policy.md](docs/capability-policy.md) · [linux-sandbox.md](docs/linux-sandbox.md) |
| Compatibility | [conformance.md](docs/conformance.md) · [framework-compatibility/](docs/framework-compatibility/README.md) |
| Quality & performance | [testing.md](docs/testing.md) · [performance-benchmarks.md](docs/performance-benchmarks.md) |

The full task index is in [docs/README.md](docs/README.md).

## Development and Validation

```sh
for d in examples/hono-reference examples/itty-router-reference examples/h3-v2-reference; do
  npm ci --ignore-scripts --prefix "$d"
done
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DCAPSID_BUILD_HOST=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

The full CI matrix is in [testing.md](docs/testing.md); contribution
guidelines are in [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[Apache-2.0](LICENSE) © Capsid contributors
