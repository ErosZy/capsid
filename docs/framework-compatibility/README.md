# Framework compatibility

Capsid implements the WinterTC **ECMA-429 Minimum Common Web API** profile
`CAPSID-MIN-2025-subset-v0` (see [standards and conformance](../conformance.md)).
Any framework that compiles to a single self-contained ESM exporting a standard
`fetch(request)` handler can be run on Capsid, provided it stays inside the
Web-standard surface and does not depend on a Node/server adapter, a listener,
filesystem static serving, or provider-specific bindings.

Frameworks run as ordinary, self-contained ESM applications. They are not part of the Capsid Web API profile and do not enter the runtime ABI or native capability policy. The runtime has no framework detection, no special branches, and no modified npm source.

## Verified frameworks

The compatibility suite pins these versions and continuously verifies them with
differential vectors and independent absolute assertions:

| Framework | Pinned version | Differential scope | Notes |
| --- | --- | ---: | --- |
| [Hono](hono.md) | 4.12.32 | 68 vectors | Core routing, middleware, streaming, and lifecycle |
| [itty-router](itty-router.md) | 5.0.24 | 96 vectors × 3 variants | AutoRouter, Router, IttyRouter |
| [H3 v2](h3-v2.md) | 2.0.1-rc.26 | 129 vectors | Core, middleware/hooks, some Web-standard utilities |
| [Elysia](elysia.md) | 1.4.29 | 49 vectors | AOT compose, hooks, multipart, @elysiajs plugins, schema validation; three pinned app-level notes |

## Using other frameworks

Other Web-standard frameworks that target the same fetch-handler model can be
evaluated against the same rules: bundle to one audited ESM, avoid the excluded
surfaces listed below, and run the differential/lifecycle/global-surface matrix.
Only the pinned versions above carry Capsid evidence; adding a framework requires
the same reference app, vector suite, negative controls, and upgrade process as
the existing three.

Common validation path:

```text
spawn worker
  → LOAD_BUNDLE (single audited ESM)
  → READY
  → FetchRPC request/credit
  → exported fetch(Request)
  → FetchRPC response/credit
```

The differential between the reference and the real worker is only part of the evidence. Every vector containing `expect` also has independent absolute assertions, so a reference and runtime failing at the same time cannot still appear as a pass.

Common exclusions: Node/Deno/Bun/Cloudflare adapters, server/listener, filesystem static serving, WebSocket server, external/remote/`file:` imports, and provider ambient bindings. These boundaries are maintained by separate expected-rejection tests and are not counted as framework core incompatibilities.
