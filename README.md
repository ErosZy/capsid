![Capsid](logo.png)

[![Testing validity](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml/badge.svg)](https://github.com/ErosZy/capsid/actions/workflows/testing-validity.yml)
[![Release](https://img.shields.io/github/v/release/ErosZy/capsid?label=release)](https://github.com/ErosZy/capsid/releases)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**Run AI-generated backend code without giving it your process, filesystem, or
network.** Capsid is an embeddable JavaScript data plane for untrusted
Web-standard Fetch handlers. Your Host keeps control of listeners, TLS,
routing, worker pools, and policy; each worker receives one self-contained ESM
bundle and only the capabilities the Host approves.

> **Status**: `0.2.2`, ABI v7. The first-party `capsid-host` is a
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

The implementation is small and measurable: the v0.2.2 performance point
measures capsid in its production posture — a managed coordinator with the
mandatory Linux strict sandbox. It reaches about **9,490 QPS** on JSON 1 KiB,
starts a 10 KiB bundle in **19.7 ms** source / **17.7 ms** trusted bytecode
under the full sandbox, and serves from a **16.4 MB PSS** path (8.5 MB
coordinator + 7.8 MB sandboxed worker). Claims are
backed by pinned WPT, framework differentials, sanitizers, fuzzing, privileged
sandbox probes, and identity-linked performance evidence.

## Install Prebuilt Release

The `v0.2.2` release carries an `install.sh` that downloads the
archive for your OS/architecture, verifies its SHA-256, and extracts the
binaries into `$HOME/.local` (override with `PREFIX`):

```sh
# exact release
curl -fsSL https://github.com/ErosZy/capsid/releases/download/v0.2.2/install.sh \
  | bash -s -- v0.2.2

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

Conclusion-level v0.2.2 samples (Intel i5-12400F 6C/12T on WSL2, single
serving process per stack, conns=64 / inflight=64, warmup 5 s + 5 measured
rounds of 15 s, exact-body correctness checked every round). capsid is
measured in its production posture — a managed coordinator with the
mandatory Linux strict sandbox (namespaces, Landlock, seccomp) and
production stream-window defaults; the comparison stacks run their
framework-optimal single-process configs. The interactive history page
with full charts lives at <https://eroszy.github.io/capsid/>.

| Dimension | Capsid (managed, strict sandbox) | Comparison |
| --- | ---: | ---: |
| JSON 1 KiB throughput | **9,490 QPS** | workerd 26,624<br>FastAPI 12,948<br>Flask 3,568<br>PHP Slim 2,095 |
| JSON 16 KiB throughput | 7,109 QPS | workerd **20,189**<br>FastAPI 12,599<br>Flask 2,878<br>PHP Slim 2,008 |
| Stream 1 KiB throughput | 6,523 QPS | workerd 14,192<br>FastAPI 6,671<br>Flask 3,987<br>PHP Slim 2,032 |
| Serving path memory | **16.4 MB PSS** (8.5 coordinator + 7.8 sandboxed worker) | Flask 17.0<br>PHP Slim 28.1<br>FastAPI 46.1<br>workerd 581.0 (untuned V8) |
| Measured 2 GB density, strict sandbox, init → warmed (10k / 100k / 1M) | **891→882 / 569→568 / 132→132** workers (±1%) | workerd cells 1,061→638 / 917→580 / 244→214 (V8 grows −40%..−12%) |
| Small bundle cold start (10 KiB, full sandbox) | **17.7 ms** bytecode / 19.7 ms source | Deno 20<br>workerd 20<br>Node 57 |
| 1 MB trusted bytecode cold start | **31.1 ms** | workerd 38<br>Deno 55<br>Node 81 |

workerd leads raw throughput on every cell but its process grows to ≈581 MB
PSS under the 64-connection load (no return observed within the sampling
windows, and workerd serves ~2.8× the requests there); capsid holds the
whole sandboxed serving path at ≈16 MB. Isolation itself is cheap: the
strict sandbox moves cold start by under 5 ms, 10k density by 1.6%, and
JSON throughput not at all. Full methodology, the
five-stack protocol, per-process memory breakdowns, and evidence rules are in
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
| Quality & performance | [testing.md](docs/testing.md) · [performance-benchmarks.md](docs/performance-benchmarks.md) |
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
