![Capsid](logo.png)

[![Testing validity](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml/badge.svg)](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml)
[![Release](https://img.shields.io/github/v/release/ErosZy/capsid?label=release)](https://github.com/ErosZy/capsid/releases)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**Run AI-generated backend code without giving it your process, filesystem, or
network.** Capsid is an embeddable JavaScript data plane for untrusted
Web-standard Fetch handlers. Your Host keeps control of listeners, TLS,
routing, worker pools, and policy; each worker receives one self-contained ESM
bundle and only the capabilities the Host approves.

> **Status**: `0.2.1`, ABI v7. The first-party `capsid-host` is a
> development/benchmark entry point, not a production deployment interface;
> production isolation is only promised by the Linux strict sandbox.

Future development, including the protocol-first WebSocket, Binding v2, and
progressive Cloudflare workerd compatibility work, is tracked in the
[project roadmap](ROADMAP.md).

Capsid is aimed at AI app builders, multi-tenant automation, and plugin systems
that need to execute generated code as a service—not as trusted code inside the
main application process.

## Why Capsid Is Different

| Concern | Host-controlled boundary | What untrusted code receives |
| --- | --- | --- |
| HTTP | Listener, TLS, routing, admission, and pool lifecycle | A standard `fetch(request)` call |
| Authority | Module and resource policy, Linux sandbox, limits, and audit | Deny-by-default `capsid:*` facades |
| Databases and services | Trusted Binding implementation, credentials, and maximum permissions | A narrow asynchronous `capsid:binding/<id>` API |
| Failure | Process lifetime, timeout, cancellation, crash budget, and replacement | No process-control API |

This is not a general-purpose Node.js replacement. Capsid intentionally omits
runtime package installation, server adapters and listeners, FFI, raw sockets,
and ambient process APIs. In return, the Host gets a non-blocking C ABI/C++11
data plane with streaming, credit backpressure, explicit lifecycle control,
and a capability boundary designed for hostile code.

The implementation is small and measurable. The current clean run reaches
about **7,042 QPS** on JSON 1 KiB, starts a 10 KiB bundle in **8.43 ms** from
source or **7.45 ms** from trusted bytecode, and serves from about **6.2 MB
PSS** for the host plus **6.4 MB** per worker. Claims are backed by pinned WPT,
framework differentials, sanitizers, fuzzing, privileged sandbox probes, and
identity-linked performance evidence.

## Install Prebuilt Release

The `v0.2.1` release carries an `install.sh` that downloads the
archive for your OS/architecture, verifies its SHA-256, and extracts the
binaries into `$HOME/.local` (override with `PREFIX`):

```sh
# exact release
curl -fsSL https://github.com/ErosZy/capsid/releases/download/v0.2.1/install.sh \
  | bash -s -- v0.2.1

# latest stable release
curl -fsSL https://github.com/ErosZy/capsid/releases/latest/download/install.sh | bash
```

After installation, add `$HOME/.local/bin` to `PATH` (the script prints the
exact command when needed).

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

`capsid-host` supports `single-worker`, `static-pool`, and `managed`.
`static-pool` takes any positive `--workers` count up to 4096 (each worker
owns one shard sharing the listener port), and listener-level CORS is available in
every mode: `--cors-*` flags on the local modes, per-listener `cors` in
managed `host.json` — both driven by the same engine. See the
[host modes and CLI reference](docs/host-config.md); step-by-step
permission field configuration is in the
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

## Host Bindings

Bindings let the Host install trusted integrations such as MongoDB, MySQL, or
Redis clients and expose only a small asynchronous method surface:

```js
import mongo from "capsid:binding/mongo";

const rows = await mongo.find({ collection: "orders", filter: { open: true } });
```

Each package is a Host-managed directory containing `manifest.json` and
`index.js`. Its manifest fixes the maximum modules, network targets,
filesystem paths, and Linux sandbox profiles; the App can only narrow those
resources. When an App declares a Binding, Capsid creates a separate Binding
Runtime in the same worker process and crosses the runtime boundary through
bounded asynchronous queues and structured-cloned values. If no Binding is
declared, Capsid keeps the original single-runtime path and pays no Binding
runtime or sandbox cost.

Application code receives the generated `capsid:binding/<id>` facade, never
the Binding's TJS modules, sockets, native handles, or credentials. See the
[technical design](docs/binding-technical-design.md) and the exact
[module and permission reference](docs/binding-modules.md).

## Standards and Frameworks

Capsid targets **ECMA-429 Minimum Common Web API** — the WinterTC (ECMA TC55)
specification, first edition, December 2025 — through the profile
`CAPSID-MIN-2025-subset-v0`. Conformance evidence is a pinned Web Platform Tests
revision plus process-level regressions; Capsid does not claim full ECMA-429
coverage beyond this profile. See
[standards and conformance](docs/conformance.md).

Frameworks that compile to a single self-contained ESM exporting a standard
`fetch(request)` handler are the supported integration path, provided they avoid
Node/server adapters, listeners, and filesystem static serving. External
services can be exposed through Host-authored Capsid Bindings. The compatibility
suite pins and continuously verifies **Hono 4.12.32**,
**itty-router 5.0.24**, **H3 v2 2.0.1-rc.26**, and **Elysia 1.4.29**; other
Web-standard frameworks can be evaluated against the same rules, but only
pinned versions carry evidence.
See [framework compatibility](docs/framework-compatibility/README.md).

## Performance

Current clean samples were captured on 2026-08-25 (AMD Ryzen 3 3300X 4C/8T,
Ubuntu 24.04/WSL2, conns=64, three rotated rounds). All 144 correctness checks
passed, with zero errors or timeouts:

| Dimension | Capsid | Comparison |
| --- | ---: | ---: |
| JSON 1 KiB throughput | **7,042 QPS** | FastAPI 6,260<br>Flask 5,068<br>Slim 1,872 |
| JSON 16 KiB throughput | 5,070 QPS | FastAPI **5,520** |
| Static bytes (1k-32k) | 3,273-5,168 QPS | FastAPI 4,667-5,954 QPS (leads) |
| Stream 1 KiB throughput | **4,753 QPS** | Flask 4,608<br>FastAPI 2,160 |
| Serving path memory (host + 2 workers) | **6.2 MB PSS host, 6.4 MB per worker** | Gunicorn worker 23.7 MB PSS<br>Uvicorn worker 42.1 MB PSS |
| Small bundle cold start (10 KiB) | **8.43 ms** source / **7.45 ms** bytecode | Node 110 ms<br>Deno 39 ms |
| 1 MB trusted bytecode cold start | **36.23 ms** | Node 137 ms<br>Deno 52 ms |
| Retained optimizer portfolio | **+2.91%** | paired 95% CI **[+0.84%, +5.02%]** |

Full methodology, the 12-workload matrix (1k-32k × json/bytes/stream), per-process
resource breakdown, and evidence rules are in
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
  local Binding development (`--bindings-root`) is supported with
  reparse-point/hard-link/ACL checks, while strict sandbox profiles and
  `managed` remain Linux-only and `--mode managed` prints a notice and exits
  at runtime.

The full matrix and build requirements are in
[docs/platform-support.md](docs/platform-support.md).

## Documentation Index

| Topic | Entry |
| --- | --- |
| Architecture & boundaries | [architecture.md](docs/architecture.md) |
| Platform differences | [platform-support.md](docs/platform-support.md) · [windows.md](docs/windows.md) |
| Host embedding | [host-integration.md](docs/host-integration.md) |
| Configuration & permissions | [host-config.md](docs/host-config.md) · [capsid-json.md](docs/capsid-json.md) |
| Host Bindings | [binding-technical-design.md](docs/binding-technical-design.md) · [binding-modules.md](docs/binding-modules.md) |
| Security & sandbox | [capability-policy.md](docs/capability-policy.md) · [linux-sandbox.md](docs/linux-sandbox.md) |
| Compatibility | [conformance.md](docs/conformance.md) · [framework-compatibility/](docs/framework-compatibility/README.md) |
| Quality & performance | [testing.md](docs/testing.md) · [performance-benchmarks.md](docs/performance-benchmarks.md) · [bytecode-aot-optimizer.md](docs/bytecode-aot-optimizer.md) · [quickjs-optimization.md](docs/quickjs-optimization.md) |
| Versioned compatibility plan | [ROADMAP.md](ROADMAP.md) |

The full task index is in [docs/README.md](docs/README.md).

## Development and Validation

```sh
for d in examples/hono-reference examples/itty-router-reference examples/h3-v2-reference examples/elysia-reference; do
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
