# Elysia compatibility

## Status

Capsid Runtime validates pinned **Elysia 1.4.29** as an ordinary bundle. The
exact version and integrity are pinned by
`examples/elysia-reference/package.json` and the lockfile; this conclusion
does not automatically cover other 1.4.x or future versions.

All entry bundles must be single-file ESM with zero external imports and must
pass the build audit. The runtime adds no Elysia global, platform adapter, or
framework-specific branch.

## Build and verification

```sh
npm ci --ignore-scripts --prefix vendor/txiki.js
npm ci --ignore-scripts --prefix examples/elysia-reference

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --target test-elysia-worker-driver
ctest --test-dir build -L elysia --output-on-failure
```

The differential suite covers the same vector categories as the Hono suite,
including multipart `FormData`, JSON/text bodies, streaming, middleware,
cookies, and timeout/abort lifecycle. The reference side calls unmodified
Elysia, while the runtime side runs through a real `capsid-worker`, FetchRPC,
and the same application logic. The reference app uses Elysia's default
AOT (compose) mode, which is what a real application gets out of the box.

## Verified

- `app.fetch()`, default `{ fetch }`, and named `fetch` entries;
- method/path routing, params/wildcard/query, 404 handling;
- onRequest/onBeforeHandle/onError hooks, header/cookie manipulation;
- JSON auto-serialization, binary/streaming responses, redirect boundary;
- request body, multipart FormData, AbortSignal, worker reuse;
- async timeout, sync CPU timeout, concurrency isolation, cancellation;
- txiki.js direct `fetch()` controlled by the normal egress policy;
- Node/Deno/Bun/txiki/platform globals remain absent.

## Pinned compatibility notes

Two Elysia 1.4.29 behaviors are pinned as deliberate application-level
configuration in the reference app; both are required for the runtime
lifecycle and differential suites to pass.

### `sucrose: { gcTime: null }` is required

Elysia's route inference schedules a ~5 minute module-global GC timer on
cache misses and reschedules it on every inference. Under Capsid's
async-context hooks a timer created inside a request captures that request's
token, so the last inference's timer holds the final token open and the
worker is poisoned as a terminal continuation leak.

`new Elysia({ sucrose: { gcTime: null } })` makes `clearSucroseCache` skip the
timer entirely (the inference cache still flushes on the next miss; only the
lazy GC is disabled). The reference app carries this configuration with an
explanatory comment, so the pinned app behaves identically under Node and
under Capsid.

### Multipart parsing must live in a module-scope helper

Sucrose inference scans `handler.toString()` with
`/\w\((?:.*?)?<param>(?:.*?)?\)/` and reads a `<param>,`/`<param>)` hit as
"context passed to a function", which enables body pre-parse in compose.
Under esbuild minification the handler parameter becomes `e`, and a closure
like `map(r => ({ name: r.name, size: r.size }))` puts an `e` inside parens
right before `)` — the exactParameter regex fires, `body:true` is inferred,
compose pre-parses the multipart body, and the handler's own `formData()`
then throws "Already read". Node reproduces the 500 on the minified bundle;
it is not Capsid-specific.

The reference app keeps multipart parsing in a module-scope helper so the
handler reads `e => parseForm(e.request)`, which no longer matches, and the
body is read exactly once. Future handlers that call `formData()` inline
should be checked against this inference interaction before bundling.

## Support boundaries

Applications can bundle the Web-standard Elysia paths above into an ESM and
export Capsid Runtime's normal fetch contract. The host remains responsible
for HTTP/TLS, worker pool, timeout, network, and sandbox policy.

Explicit exclusions:

- Node/Bun/Deno/Cloudflare adapters and server/listener APIs;
- static-file serving that depends on the filesystem;
- provider bindings and context storage;
- external, remote, and `file:` module loading.

Corresponding negative tests use the `elysia-excluded` label. Product
capability changes do not automatically expand this pinned-version
compatibility statement.

## Upgrade process

1. Update the exact dependency and rebuild the lockfile;
2. Do not modify `node_modules` or Elysia source;
3. Rebuild and audit all bundles;
4. Run the differential, lifecycle, excluded-import, global-surface, and
   sanitizer matrices;
5. Review the two pinned notes above — `gcTime` behavior and the multipart
   inference interaction may change across versions.
