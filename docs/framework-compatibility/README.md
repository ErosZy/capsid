# Framework compatibility

Frameworks run as ordinary, self-contained ESM applications. They are not part of the Capsid Web API profile and do not enter the runtime ABI or native capability policy. The runtime has no framework detection, no special branches, and no modified npm source.

| Framework | Pinned version | Differential scope | Notes |
| --- | --- | ---: | --- |
| [Hono](hono.md) | 4.12.32 | 68 vectors | Core routing, middleware, streaming, and lifecycle |
| [itty-router](itty-router.md) | 5.0.24 | 96 vectors × 3 variants | AutoRouter, Router, IttyRouter |
| [H3 v2](h3-v2.md) | 2.0.1-rc.26 | 129 vectors | Core, middleware/hooks, some Web-standard utilities |

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
