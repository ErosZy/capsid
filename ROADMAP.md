# Capsid Roadmap: Progressive workerd Compatibility

## Purpose

Capsid will progressively support common Cloudflare workerd application source,
Worker Runtime semantics, Web Standards, Web Platform APIs, and the main local
development workflows associated with Wrangler. The goal is useful, measured
compatibility rather than complete workerd or Cloudflare platform parity.

Capsid will retain QuickJS-ng, its process-isolation model, and its
deny-by-default capability boundary. The roadmap prioritizes:

1. stable Host/Worker and cross-runtime protocol foundations;
2. the Module Worker request and lifetime model;
3. Web Platform behavior and common workerd Runtime APIs;
4. Cache, TCP sockets, Service Binding/RPC, and Assets through Binding v2;
5. Wrangler-compatible build, development, and Capsid deployment workflows;
6. a practical Node.js compatibility subset and legacy Service Worker syntax.

R2, D1, KV, Durable Objects, Queues, and other Cloudflare-managed products are
not part of this roadmap.

## Current Baseline

The `0.2.0` line is the stable baseline for the current HTTP runtime. It
contains:

- a pinned txiki.js, QuickJS-ng, and WAMR stack;
- self-contained ESM bundles with `default.fetch(request)` or a named `fetch`;
- Fetch, Headers, Request, Response, Streams, URL, Encoding, Crypto,
  Compression, MessageChannel, and a WebAssembly subset;
- request-scoped async context, timeout, cancellation, streaming, and credit
  backpressure;
- a pinned WPT profile, explicit deviations, and process-level platform tests;
- process isolation, the Linux strict sandbox, and capability policy;
- Binding v1 with isolated User and Binding QuickJS runtimes.

The current standards baseline is
[`CAPSID-MIN-2025-subset-v0`](docs/conformance.md). The exact global surface is
recorded in [`js/profile-manifest.js`](js/profile-manifest.js), the product
boundary is documented in [`docs/architecture.md`](docs/architecture.md), and
the current cross-runtime contract is documented in
[`docs/binding-technical-design.md`](docs/binding-technical-design.md).

The major structural gaps are:

- Capsid invokes `handler(request)`, while a workerd Module Worker uses
  `fetch(request, env, ctx)`;
- the current terminal cleanup model cannot yet represent `ctx.waitUntil()` or
  resources that outlive the HTTP response;
- FetchRPC v3 models HTTP request/response flow but not an independent duplex
  connection;
- Binding v1 only transports bounded structured values and rejects Streams,
  Sockets, remote objects, and native handles;
- WebSocketPair, HTTP Upgrade, EventSource, scheduler, and several workerd
  extensions are absent;
- Capsid does not consume Wrangler configuration or artifacts directly.

## Architectural Order

Protocol work comes before the public workerd compatibility layer. A small set
of workerd fixtures will first define the required behavior, but the public API
promise will not be frozen until the underlying protocols have been exercised
end to end.

```text
minimal workerd/WebSocket behavior fixtures
                    |
                    v
       lifetime and stream state machines
             /                    \
            v                      v
 Host <-> Worker FetchRPC v4   User <-> Binding RPC v2
            |                      |
            v                      v
   WebSocket vertical slice   Cache / TCP / RPC / Assets
             \                    /
              v                  v
       Module Worker and workerd profiles
                    |
                    v
          Wrangler compatibility layer
```

FetchRPC and Binding RPC remain separate protocol planes:

- **FetchRPC** crosses the Host/Worker process boundary and carries HTTP,
  Upgrade, WebSocket, and Host-mediated service traffic.
- **Binding RPC** crosses QuickJS runtime boundaries inside a worker and
  carries capability calls, proxied streams, and remote objects.

They may share state-machine concepts such as resource IDs, credits,
cancellation, half-close, and owner tokens. They must not be forced into one
wire format or one trust boundary.

## Compatibility Policy

### Engine strategy

- QuickJS-ng remains the JavaScript engine; V8 migration and a dual-engine
  architecture are out of scope.
- Engine compatibility means a tested ECMAScript syntax and behavior subset,
  measured with selected Test262 cases, WPT, and workerd differential tests.
- V8-specific behavior, JIT characteristics, GC details, and differences that
  cannot be reproduced reliably must be listed as explicit deviations.
- A workerd profile must apply the relevant workerd security restrictions,
  including restrictions on dynamic code and dynamic WebAssembly compilation.

### Version locks

Every Capsid minor release targeting workerd compatibility must record:

- the workerd commit;
- the Cloudflare compatibility date;
- the supported compatibility flags;
- the Wrangler and Cloudflare runtime type versions;
- the WPT and Test262 revisions;
- the compatibility matrix and exact deviations.

The initial research baseline is:

- workerd commit `6dd2348caee25ee1f93e3b27ed468ad53bfa59a5`;
- compatibility date `2026-08-20`;
- Wrangler `4.124.0`.

An unknown date or unsupported compatibility flag must produce an actionable
error. Capsid must never silently ignore it.

### Profiles and compatibility

- During `0.x`, workerd behavior is opt-in and the existing Capsid profile
  remains the default.
- Existing `capsid/app-v1` and `capsid/app-v2` applications retain their
  current behavior.
- A future `capsid/app-v3` schema carries runtime profile, compatibility date,
  compatibility flags, and platform Binding declarations.
- In `1.0`, newly created App v3 and Wrangler applications default to the
  workerd profile. Existing applications remain pinned to their declared
  profile.
- Patch releases cannot change observable compatibility behavior without an
  existing compatibility flag.

## Version Roadmap

### v0.2.0: Freeze the HTTP Runtime Baseline

- Ship the current HTTP Runtime, ABI v7, FetchRPC v3, Binding v1, and
  `CAPSID-MIN-2025-subset-v0` behavior without adding workerd features.
- Publish stable release notes, packages, checksums, SBOMs, build identity, and
  the hosted evidence index from the exact release commit.
- Treat this release as the compatibility and performance baseline for all
  subsequent protocol work.

Acceptance: the final release commit passes Release/LTO with pinned WPT and
delegated sandbox checks, ASan, UBSan, TSan, fuzzing, macOS native development,
Windows native development, package smoke tests, reproducibility checks, and
the hosted evidence index.

### v0.3.0: FetchRPC v4 and WebSocket Foundation

Define the FetchRPC v4 contract before implementing the public WebSocket API.
The contract must specify:

- version and capability negotiation, including deterministic rejection of
  unsupported peers;
- a `channel_id` independent from the HTTP `request_id`;
- Upgrade request, acceptance, and rejection;
- open, data, credit, half-close, close, error, and cancel operations;
- frame sequencing, message boundaries, fragmentation, maximum sizes, and
  duplicate, unknown, late, or out-of-order frame handling;
- independent read/write credit and bounded buffering;
- close code/reason limits and ping/pong ownership;
- ownership by a request, response stream, tail task, or duplex connection;
- client disconnect, request timeout, worker shutdown, and worker replacement
  cleanup rules.

Implement one complete vertical slice after the protocol tests are frozen:

- Host HTTP Upgrade handling;
- Worker-side `WebSocketPair`, `WebSocket.accept()`, and events;
- status 101 responses and the WebSocket response extension;
- bidirectional Host/Worker frames with backpressure and bounded cleanup;
- outbound WebSocket support under the existing egress policy.

Add the public C ABI needed by embedders to accept/reject an Upgrade and
exchange duplex messages. If this cannot be expressed additively, introduce
ABI v8 with an explicit ABI v7 compatibility path. FetchRPC v3 and v4 peers
must never be mixed silently.

Acceptance: real client/server interoperability, fragmented messages, slow
consumers, simultaneous close, half-close where applicable, invalid frames,
timeouts, disconnects, shutdown, and worker replacement all pass process-level
tests, sanitizers, and protocol fuzzing.

### v0.4.0: Binding RPC v2

Build Binding v2 on the lifetime and stream state-machine lessons proven by
the WebSocket vertical slice. Binding v2 adds:

- bidirectional credit-based Stream proxies;
- controlled Request, Response, ReadableStream, and WritableStream proxies;
- remote object and method stubs with leases and explicit disposal;
- deadline, AbortSignal, cancellation, late-result disposal, and structured
  remote errors;
- per-request, per-Binding, and per-worker limits for handles, streams, queued
  frames, and bytes;
- fair scheduling across User and Binding runtimes;
- deterministic cleanup during request end, Binding failure, worker shutdown,
  and worker poisoning.

Real Socket, file, Cache backend, and other native handles remain in the
trusted Binding Runtime. The User Runtime receives proxies only. Existing
Binding v1 packages and the zero-Binding fast path remain supported.

Acceptance: stream backpressure, cancellation, remote-object disposal, GC
fallback, deadline races, quota exhaustion, cross-request handle misuse,
malicious providers, and zero-Binding performance all have positive and
negative tests.

### v0.5.0: Module Worker Runtime Model

- Introduce the opt-in workerd runtime profile and `capsid/app-v3`.
- Support `export default { fetch(request, env, ctx) {} }`.
- Keep `env` identity stable within one Generation and inject plain variables,
  secrets, and Binding facades.
- Implement `ctx.waitUntil(promise)` with a response-independent tail phase,
  all-settled behavior, cancellation, and a 30-second default upper bound that
  a Host may reduce.
- Cancel unregistered detached work when the invocation ends.
- Implement `ctx.passThroughOnException()` state. Without a configured
  Host-authoritative fallback provider, the original exception propagates.
- Add compatibility date and flag resolution to startup identity and
  deployment snapshots.

Acceptance: concurrent requests cannot exchange env/context state; response
completion, streaming, waitUntil, disconnect, timeout, shutdown, rejection,
and worker replacement have deterministic and leak-free behavior.

### v0.6.0: Web Platform Core and Runtime Utilities

- Expand Fetch, Headers, Request/Response, FormData, Web Streams, Abort, URL,
  Encoding, Crypto, EventTarget, Console, Performance, and Timers coverage.
- Audit header guards, body disturbed/locked state, clone/tee, redirects,
  multipart parsing, AbortSignal reasons, and cancellation propagation.
- Implement workerd request-context timers and its observable Date and
  Performance behavior for the selected profile.
- Implement EventSource and `scheduler.wait()`.
- Apply the selected workerd restrictions for dynamic JavaScript and
  WebAssembly compilation.
- Expand selected Test262 coverage to document QuickJS/V8 language differences.

Acceptance: every advertised API passes pinned standards tests, process-level
integration tests, and differential fixtures. Expected failures identify an
exact test and registered deviation; unexpected passes also fail the gate.

### v0.7.0: Platform Capabilities on Binding v2

Implement workerd-shaped facades backed by Binding v2:

- `caches.default`, `caches.open()`, and `Cache.match/put/delete`;
- `cloudflare:sockets.connect()` with `readable`, `writable`, `opened`,
  `closed`, `close()`, and `startTls()`;
- Service Binding `fetch()`, ordinary method calls, `WorkerEntrypoint`,
  `RpcTarget`, and remote stubs;
- `env.ASSETS.fetch()` and static asset serving;
- a Host-authoritative fallback provider for
  `ctx.passThroughOnException()`.

For TCP, the native socket remains in a trusted Binding Runtime. The User
Runtime's readable/writable streams proxy bytes through Binding RPC v2 with
bounded credits. A Service Binding may target a Host-provided package or a
Host-routed Capsid application, but the application cannot select an arbitrary
privileged target.

Acceptance: TLS upgrade, TCP half-close, slow consumers, streamed Cache bodies,
conditional cache behavior, RPC disposal, service failure, disconnect,
deadline, quota, and policy denial all pass end-to-end and sandbox tests.

### v0.8.0: Wrangler-Compatible Workflow

- Publish `@capsid/wrangler` with `capsid-wrangler build`, `dev`, and `deploy`.
- Use a pinned Wrangler/esbuild dry-run build rather than copying or forking
  Wrangler's bundler.
- Support TOML, JSON, and JSONC plus `name`, `main`, `compatibility_date`,
  `compatibility_flags`, environments, vars, define, tsconfig, custom build,
  rules, minify, no-bundle, assets, `.dev.vars`, and `.env` overrides.
- Make `dev` watch, rebuild, start Capsid Host, select a port, and replace the
  local Generation gracefully.
- Make `deploy` produce an immutable Capsid Generation, translate App v3
  configuration, and call the existing Capsid Admin API.
- Reject cloud-only configuration and unsupported product Bindings with exact
  field-level diagnostics.

Acceptance: representative Wrangler HTTP Worker and Assets projects complete
build, dev, and deployment to Capsid Host without source changes. A failed
build or warm-up cannot replace the active Generation.

### v0.9.0: Practical Node Compatibility and Legacy Workers

- Under `nodejs_compat`, implement `Buffer`, a restricted `process`, and
  AsyncLocalStorage.
- Add practical subsets of `node:assert`, `node:buffer`, `node:events`,
  `node:util`, `node:path`, `node:url`, `node:querystring`, `node:timers`,
  `node:timers/promises`, `node:crypto`, and AsyncLocalStorage from
  `node:async_hooks`.
- Expose only the Host-provided environment snapshot through `process.env`.
- Add legacy `addEventListener("fetch", ...)`, FetchEvent, `respondWith()`, and
  event-side `waitUntil()` compatibility. Module Workers remain preferred.

Acceptance: selected npm and framework fixtures pass absolute assertions and
workerd differential tests. Unsupported Node modules fail explicitly instead
of providing successful no-op stubs.

### v0.10.0: Compatibility Convergence and Hardening

- Run real Wrangler templates and the Cloudflare build targets of Hono,
  itty-router, H3, and Elysia end to end.
- Expand WPT, Test262, compatibility date/flag, concurrency, duplex transport,
  Binding v2, and sandbox coverage.
- Generate a workerd upgrade report covering API, behavior, type, performance,
  and security-boundary changes.
- Establish regression budgets for cold start, fixed HTTP responses, streaming,
  WebSocket, Cache, TCP, and RPC.
- Publish machine-readable compatibility results and user-facing deviations.

Acceptance: the full CI, sanitizer, fuzz, privileged sandbox, soak, and
performance gates pass, and every omitted capability is visible in the
compatibility report.

### v1.0.0: Stable Compatibility Contract

- New App v3 and Wrangler projects default to the workerd profile.
- App v1/v2 applications remain pinned to the legacy Capsid profile.
- Stabilize App v3, the current C ABI, Binding v2, the supported Wrangler
  configuration subset, and compatibility-date policy.
- Require every future behavior change to enter through a new compatibility
  date or explicit flag.

## Test and Release Gates

Every compatibility release must provide:

1. differential results from identical fixture bundles on pinned workerd and
   Capsid builds;
2. pinned WPT and selected Test262 results;
3. process tests for request, cancel, stream, timeout, Upgrade, duplex channel,
   and lifecycle behavior;
4. Wrangler fixtures for TOML/JSONC, environment inheritance, rebuild, assets,
   dry-run artifacts, and Admin API deployment;
5. Binding v2 backpressure, disconnect, leak, deadline, quota, ownership, and
   sandbox tests;
6. ASan, UBSan, TSan, fuzz, Linux privileged sandbox, and soak evidence;
7. machine audits of globals, modules, compatibility flags, and deviations;
8. performance comparisons with the preceding Capsid release and the pinned
   workerd baseline.

Differential tests are not sufficient by themselves. Fixtures must retain
independent absolute assertions so that identical failures in both runtimes do
not appear as compatibility.

## Explicitly Out of Scope Through v1.0

- R2, D1, KV, Durable Objects, Queues, AI, Analytics, and Email;
- Cron/Scheduled, Queue, and Email event handlers;
- Cloudflare accounts, authentication, remote resource provisioning, and
  deployment to Cloudflare;
- launching Capsid directly through the unmodified `npx wrangler dev` command;
- HTMLRewriter, inbound TCP listeners, browser DOM, Window, and Document;
- complete Node.js, child processes, worker threads, native addons, or ambient
  process/OS authority;
- V8 JIT, V8 bytecode, V8 GC, inspector-protocol, or performance equivalence;
- every workerd compatibility flag or every Cloudflare Runtime API.

Production isolation remains a Linux strict-sandbox contract. macOS and
Windows remain native development targets and do not claim equivalent
production isolation.

## Upstream References

- [workerd repository](https://github.com/cloudflare/workerd)
- [Workers Runtime APIs](https://developers.cloudflare.com/workers/runtime-apis/)
- [JavaScript and Web Standards](https://developers.cloudflare.com/workers/runtime-apis/web-standards/)
- [Fetch Handler and ExecutionContext](https://developers.cloudflare.com/workers/runtime-apis/handlers/fetch/)
- [Compatibility Flags](https://developers.cloudflare.com/workers/configuration/compatibility-flags/)
- [Wrangler Configuration](https://developers.cloudflare.com/workers/wrangler/configuration/)
- [Wrangler Bundling](https://developers.cloudflare.com/workers/wrangler/bundling/)
- [Cache API](https://developers.cloudflare.com/workers/runtime-apis/cache/)
- [TCP sockets](https://developers.cloudflare.com/workers/runtime-apis/tcp-sockets/)
- [Workers RPC](https://developers.cloudflare.com/workers/runtime-apis/rpc/)
- [WebSockets](https://developers.cloudflare.com/workers/runtime-apis/websockets/)
