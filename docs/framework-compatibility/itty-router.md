# itty-router compatibility

## Status

Capsid Runtime validates pinned **itty-router 5.0.24**. The version and artifact integrity are pinned by `examples/itty-router-reference/package.json` and the lockfile, and do not automatically cover other v5 or future versions.

AutoRouter, Router, and hand-written IttyRouter pipelines are each built as a single-file ESM with zero external imports. The build audit rejects source maps, Node built-ins, file URLs, dynamic imports, `require`, `globalThis.tjs`, and platform-global definitions; each bundle must be smaller than 96 KiB.

## Build and verification

```sh
npm ci --ignore-scripts --prefix vendor/txiki.js
npm ci --ignore-scripts --prefix examples/itty-router-reference

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --target \
  test-itty-router-worker-driver test-module-denial
ctest --test-dir build -L itty-router --output-on-failure
```

96 deterministic request vectors run against both the unmodified Node reference and the real worker. The same vector set independently covers the three router variants, with independent absolute assertions.

## Verified

- default router, `{ fetch }`, named `fetch`, and hand-written pipeline;
- GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS/all/PURGE;
- fixed/named/optional/file/wildcard/greedy routes, base, and priority;
- single-valued, empty, encoded, and duplicate query, plus cross-request isolation;
- linear handlers and before/route/catch/finally ordering;
- JSON/text/urlencoded/multipart/File, body clone, and cross-credit upload;
- JSON/text/HTML/image/binary/Blob/stream, 204/304, status/header;
- CORS, nested routes, request context;
- direct fetch, concurrency, three cancellation types, async/CPU timeout, and reuse;
- process, Buffer, Deno, Bun, and `globalThis.tjs` remain absent.

The differential compares status, sorted lowercase headers, exact body, params/query, middleware trace, error classification, and CORS. `Date` may be normalized; stream transport chunk counts are not compared, but total bytes and checksum must match.

## Support and exclusions

Applications can bundle the validated v5 router variants and Web-standard helpers into one ESM. They cannot depend on provider ambient bindings.

Exclusions: Cloudflare bindings/ExecutionContext/cache/Durable Object, Node/Bun server adapters, WebSocket server/upgrade, filesystem static serving, external/remote/file modules, process/Worker, and txiki private APIs. Seven `itty-router-excluded` tests verify these boundaries.

## Upgrade process

After updating the exact dependency and lockfile, rebuild/audit the three entries and run the differential, lifecycle, excluded-import, global-surface, P0, and sanitizer matrices. Vector or exclusion changes must be explicitly reviewed.
