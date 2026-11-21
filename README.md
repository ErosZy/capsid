![Capsid](logo.png)

[![Testing validity](https://github.com/rowanellis-113/capsid/actions/workflows/testing-validity.yml/badge.svg)](https://github.com/rowanellis-113/capsid/actions/workflows/testing-validity.yml)
[![Prerelease v0.3.0-beta.5](https://img.shields.io/badge/prerelease-v0.3.0--beta.5-orange)](https://github.com/rowanellis-113/capsid/releases/tag/v0.3.0-beta.5)
[![Site & Benchmarks](https://img.shields.io/badge/site%20%26%20benchmarks-live-00b4e6)](https://rowanellis-113.github.io/capsid/)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**A secure, embeddable, high-density JavaScript runtime for untrusted AI code.**
Capsid runs self-contained ESM applications exporting a Web-standard
`fetch(request)` handler behind a Host-controlled boundary. Applications execute
in separate `quickjs-turbo` (QuickJS-NG 0.15.1-derived) worker processes; the
Host owns listeners, routing, capabilities, resource limits and worker lifetime.

- **Hard Linux OS isolation**: per-worker Linux Namespaces, Landlock and Seccomp
  with default-deny egress (`egress_policy == NULL`) and zero JIT RWX pages.
- **Micro-container density**: **729** warmed 10 KiB strict-sandbox workers in
  2 GiB RAM and **18.5 MiB** total host + worker PSS under 64-connection load.
- **Sub-20 ms cold starts**: **6.9 ms** (10 KiB) / **18.6 ms** (1 MiB) with
  precompiled trusted bytecode.
- **Embeddable C11 / C++11 SDK**: stable `<capsid/runtime.h>` (ABI v7) with
  non-blocking IPC, streaming, WebSocket v4 and trusted Host Bindings.

> **Prerelease**: [`v0.3.0-beta.5`](https://github.com/rowanellis-113/capsid/releases/tag/v0.3.0-beta.5).
> **Stable**: [`v0.2.2`](https://github.com/rowanellis-113/capsid/releases/tag/v0.2.2). Public C ABI v7.
> The beta is experimental, not a stable-production recommendation. Only Linux
> strict sandboxing provides the production isolation boundary; native macOS
> and Windows support is for trusted development and integration.
>
> Documentation/data reviewed **2026-11-22** for `dev/v0.3.1` (`quickjs-turbo`
> `v0.15.1-turbo.4` integration; the published `v0.3.0-beta.5` release retains
> QuickJS-NG 0.15.1).
> [Current status and data scope](docs/current-status.md) separates the published
> beta, current source interface, measured development snapshot and dated history.

## Start Here

- **Explore the product & verified benchmarks:** [Capsid Site & Benchmark Hub](https://rowanellis-113.github.io/capsid/).
- **Run a local application:** [Getting started](docs/getting-started.md).
- **Choose or verify a package:** [Releases and installation](docs/releases.md).
- **Build the runtime or SDK:** [Building from source](docs/building.md).
- **Embed in your own C/C++ Host:** [Host integration](docs/host-integration.md).
- **Find a reference or design record:** [Documentation index](docs/README.md).

Select `v0.3.0-beta.5` explicitly rather than relying on GitHub's default
`latest` download.

**Linux or macOS** (installer script):

```sh
curl -fsSL https://github.com/rowanellis-113/capsid/releases/download/v0.3.0-beta.5/install.sh \
  | CAPSID_REPO=rowanellis-113/capsid bash -s -- v0.3.0-beta.5
```

**Windows PowerShell** (`x86_64` archive + SHA-256 verification):

```powershell
$base = 'https://github.com/rowanellis-113/capsid/releases/download/v0.3.0-beta.5'
$zip  = '.\capsid-0.3.0-windows-x86_64.zip'
Invoke-WebRequest "$base/capsid-0.3.0-windows-x86_64.zip" -OutFile $zip
Invoke-WebRequest "$base/capsid-0.3.0-windows-x86_64.zip.sha256" -OutFile "$zip.sha256"
$exp = ((Get-Content "$zip.sha256" -Raw).Trim() -split '\s+')[0]
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ine $exp) { throw 'SHA-256 mismatch' }
Expand-Archive -LiteralPath $zip -DestinationPath "$HOME\capsid-beta" -Force
```

**Direct archives (no `curl | bash` / `wget` / offline)**: download any native
`.tar.gz` or `.zip` and its `.sha256` sidecar directly from the
[`v0.3.0-beta.5` Release](https://github.com/rowanellis-113/capsid/releases/tag/v0.3.0-beta.5)
for Linux musl (`x86_64`, `arm64`), macOS (`arm64`, `x86_64`) or Windows
(`x86_64`). See the [installation guide](docs/releases.md) for `wget` / manual
tarball verification, channel selection and package contents.

## Application Model

```js
// app.bundle.js — already a self-contained ESM; no bundler needed here.
export default {
  fetch(request) {
    return Response.json({
      message: "hello from Capsid",
      path: new URL(request.url).pathname,
    });
  },
};
```

Capsid is not a general-purpose Node.js replacement. Applications do not own a
server listener, install packages at runtime, or receive ambient process access,
FFI or raw sockets. Bundle dependencies ahead of time and use Web-standard APIs.
Only [pinned framework versions](docs/framework-compatibility/README.md) carry
compatibility evidence.

| Responsibility | Host | Application |
| --- | --- | --- |
| HTTP service | Listener, TLS boundary, routing and admission | `fetch(Request)` and `Response` |
| Authority | Capability policy, egress policy and OS isolation | Explicitly authorized facades |
| External services | Trusted Bindings and credentials | Narrow asynchronous Binding methods |
| Failure and resources | Deadlines, cancellation, limits and process reclamation | Request-scoped work |

Inbound WebSocketPair/101 and outbound ws/wss are available within the documented
policy and lifetime boundaries. Start with the [inbound](examples/websocket.js)
and [outbound](examples/websocket-outbound.js) examples; consult the
[compatibility reference](docs/workerd-websocket-semantics.md) for limitations.

## Choose a Host Mode

| Mode | Purpose | Platforms |
| --- | --- | --- |
| `single-worker` | One local application and worker; development and integration | Linux, macOS, Windows |
| `static-pool` | A fixed local pool sharing one listener; integration and benchmarks | Linux, macOS, Windows |
| `managed` | Linux coordinator, application versions, Admin API and supervised pools | Linux only |

The local CLI is a development/benchmark entry point, not a production deployment
recipe. Managed mode is the first-party Linux production path, but its deployment
interface remains experimental. Neither mode selection nor successful startup
alone proves a secure deployment. See [Host configuration](docs/host-config.md)
and [platform support](docs/platform-support.md).

Application requests belong in `capsid.json`. Managed mode additionally uses a
Host-authoritative `host.json`; effective authority is the intersection of Host
limits and application requests. Unknown or invalid configuration fails closed.
Schema recognition is not proof that every resource field is enforced: App
`pidsMax` is not an effective per-App PID limit in this source. See the
[resource caveats](docs/capsid-json.md) before relying on a ceiling.

## Security Boundary

Separate processes are only one layer of isolation:

- `strict_sandbox` is off by default in the C API; default configuration is for
  trusted code only.
- When `egress_policy == NULL`, all egress Fetch requests are denied.
- `tjs:*` modules cannot be enabled through configuration.
- On Linux, verify that `CAPSID_EVENT_READY.flags` contains the sandbox features required by the deployment.
- Load only trusted bytecode produced by the matching build compatibility
  identity. Bytecode is not a safe format for untrusted input.

Current public modules require explicit authorization:

- Resource access: `capsid:env`, `capsid:fs`, `capsid:stdio`, `capsid:storage`,
  `capsid:system`.
- Permission queries: `capsid:permissions`.
- Utilities: `capsid:assert`, `capsid:getopts`, `capsid:hashing`, `capsid:ipaddr`,
  `capsid:utils`, `capsid:uuid`.

For untrusted workloads, configure the [Linux strict sandbox](docs/linux-sandbox.md),
[capability policy](docs/capability-policy.md), egress and surrounding OS/network
boundaries together. Read the [security policy](SECURITY.md) before deployment.

## Embed the Runtime

The SDK provides the C11 header `<capsid/runtime.h>` and the C++11 wrapper
`<capsid/runtime.hpp>`. Your Host drives the non-blocking request, credit,
streaming, cancellation and lifecycle APIs; the runtime does not replace your
HTTP server.

From a prepared source build:

```sh
cmake --install build-release --prefix "$HOME/opt/capsid-sdk"
```

See [building and SDK consumption](docs/building.md) and the
[embedding contract](docs/host-integration.md) before implementing a Host.
Bindings can expose trusted databases or other services without passing their
credentials or native handles to application code; their authority model is in
[Binding modules](docs/binding-modules.md).

## Compatibility, Validation and Performance

Capsid targets the documented `CAPSID-MIN-2025-subset-v0` Web API profile, not
complete ECMA-429 or browser compatibility. Required CI gates and pinned workerd
compatibility acceptance are not equivalent to every ordinary WPT passing.
Known raw failures and genuine prerequisite skips remain visible. The current
pinned workerd compatibility mapping is v2; five raw WPT file failures remain.
The ordinary suite is not represented as all-passing.

- [Testing guide](docs/testing-guide.md): local checks, full qualification and
  evidence boundaries.
- [Standards and conformance](docs/conformance.md): selected API surface and
  deviations.
- [Release qualification](docs/v0.3.0-current-qualification.md): source-bound
  beta results, original failures and remaining limits.
- [Performance methodology](docs/performance-benchmarks.md) and
  [performance charts](https://rowanellis-113.github.io/capsid/): measurements belong to
  their recorded source, build, machine and protocol. Historical v0.2.2 numbers
  are not 0.3.0-beta measurements.

**Latest accepted local measurement:** `v0.3.0-beta.5` (`c7a80b9`), rebuilt
locally under WSL2 on an i5-12400F, 2026-11-21 (`capsid-perf-lane-v3` seven-stack
pure-JS matrix). This measures the local Release/LTO build, **not the
distributed packages**.

| Dimension | Capsid (Strict Linux Sandbox) | Notes |
| --- | --- | --- |
| **2 GiB warmed pool** | **729 / 503 / 128** workers | 10 KiB / 100 KiB / 1 MiB (100 req/worker) |
| **Cold start (1 MiB bytecode)** | **18.64 ms** (`16.0 MiB` PSS) | **6.9 ms** at 10 KiB bytecode |
| **Serving memory (PSS)** | **15.0 MiB** (10 req/s) · **18.5 MiB** (64-conn) | Complete host + strict worker tree |
| **Sustained throughput** | **21,746.6 / 14,194.6 / 10,649.7 / 9,808.1** QPS | JSON 1 KiB / 16 KiB / stream / dynamic API (**1,175 QPS/MiB** on 1 KiB) |

Interactive charts, three-tier peer comparisons and SHA-256 replayable archives
are published at the [Capsid Site & Benchmark Hub](https://rowanellis-113.github.io/capsid/);
earlier records remain in the [performance reference](docs/performance-benchmarks.md).

For pinned C/C++, JavaScript and Python formatting commands, see
[source formatting](docs/formatting.md).

To contribute, follow [CONTRIBUTING.md](CONTRIBUTING.md). Planned work is tracked
in [ROADMAP.md](ROADMAP.md); dated reviews and handoffs are indexed separately
under [history and evidence](docs/history/README.md).

## License

[Apache-2.0](LICENSE) © Capsid contributors
