# Hono compatibility

## Status

Capsid Runtime validates pinned **Hono 4.12.32** as an ordinary bundle. The exact version and integrity are pinned by `examples/hono-reference/package.json` and the lockfile; this conclusion does not automatically cover other 4.x or future versions.

All three entry bundles must be single-file ESM with zero external imports and must pass the build audit. The runtime adds no Hono global, platform adapter, or framework-specific branch.

## Build and verification

```sh
npm ci --ignore-scripts --prefix vendor/txiki.js
npm ci --ignore-scripts --prefix examples/hono-reference

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --target test-hono-worker-driver test-module-denial
ctest --test-dir build -L hono --output-on-failure
```

The differential suite contains 68 deterministic vectors, 11 of which have independent absolute assertions. The reference side calls unmodified Hono, while the runtime side runs through a real `capsid-worker`, FetchRPC, and the same application logic.

## Verified

- `app.fetch()`, default `{ fetch }`, and named `fetch` entries;
- method/path routing, params/query, 404/405, base path;
- middleware order, context, header/cookie, exception handling;
- JSON/text/HTML/binary/streaming responses;
- request body, FormData, AbortSignal;
- concurrency isolation, handler/body/response-stream cancellation;
- async timeout, sync CPU timeout, and worker reuse;
- txiki.js direct `fetch()` controlled by the normal egress policy;
- Node/Deno/Bun/txiki/platform globals remain absent.

Responses compare status, normalized headers, and exact body bytes; only well-defined non-semantic fields such as dynamic `Date` are normalized. Stream transport chunk boundaries are not part of Web Streams semantics, so the total length and content are compared rather than the underlying chunk counts.

## Support boundaries

Applications can bundle the Web-standard Hono Core paths above into an ESM and export Capsid Runtime's normal fetch contract. The host remains responsible for HTTP/TLS, worker pool, timeout, network, and sandbox policy.

Explicit exclusions:

- `@hono/node-server`, Node built-ins, and Node/Bun/Deno/Cloudflare adapters;
- HTTP/WebSocket server and upgrade APIs;
- static-file adapter that depends on the filesystem;
- Cloudflare bindings, `ExecutionContext`, cache, Durable Object;
- context storage/`AsyncLocalStorage`;
- external, remote, and `file:` module loading.

Corresponding negative tests use the `hono-excluded` label. Product capability changes do not automatically expand this pinned-version compatibility statement.

## Upgrade process

1. Update the exact dependency and rebuild the lockfile;
2. Do not modify `node_modules` or Hono source;
3. Rebuild and audit all bundles;
4. Run the differential, lifecycle, excluded-import, global-surface, and sanitizer matrices;
5. Review normalization or exclusion changes as compatibility policy changes.
