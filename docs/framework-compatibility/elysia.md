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

## Plugin coverage

Beyond Elysia core, the pinned app exercises the official `@elysiajs`
plugins and core capabilities, all version-pinned in
`examples/elysia-reference/package.json`:

| Surface | Coverage |
| --- | --- |
| @elysiajs/cors 1.4.2 | allowed-origin echo, preflight 204, rejected origin |
| @elysiajs/bearer 1.4.4 | header, query (`access_token`), missing |
| @elysiajs/jwt 1.4.2 | HS256 sign/verify (jose webapi entry), bad token |
| @elysiajs/stream 1.1.0 | `Stream` class SSE frames, generator SSE |
| schema validation | typebox body (valid/invalid/optional), params |
| guard / derive / resolve | scoped beforeHandle, sync/async context augmentation |

The worker bundle keeps each plugin's real npm dist (jose's webapi entry,
nanoid's browser entry via esbuild alias) inside the single self-contained
ESM; the audit enforces that nothing else enters the graph.

## Verified

- `app.fetch()`, default `{ fetch }`, and named `fetch` entries;
- method/path routing, params/wildcard/query, 404 handling;
- onRequest/onBeforeHandle/onError hooks, header/cookie manipulation;
- JSON auto-serialization, binary/streaming responses, redirect boundary;
- request body, multipart FormData, AbortSignal, worker reuse;
- async timeout, sync CPU timeout, concurrency isolation, cancellation;
- txiki.js direct `fetch()` controlled by the normal egress policy;
- Node/Deno/Bun/txiki/platform globals remain absent;
- @elysiajs cors/bearer/jwt/stream plugins, typebox schema validation,
  guard/derive/resolve scoping (see the plugin table above).

## Pinned compatibility notes

Three Elysia 1.4.29 behaviors are pinned as deliberate application-level
configuration in the reference app; all three are required for the runtime
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

### Lazy getters in global derives need an empty query schema

`@elysiajs/bearer` registers a *global* derive whose result is a lazy
`bearer` getter that reads `context.query`. AOT compose merges derive
results into the context with `Object.assign`, which copies VALUES — the
getter is therefore evaluated on **every** route the derive reaches.
Whether `context.query` exists at all is inference-driven (`hasQuery`),
and sucrose's parameter parser (`removeColonAlias`) is defeated by
minified destructure aliases: `function({ query: i, ... })` leaves the
parameter-map key as `"query:i"` instead of `"query"`, so inference never
sees the query binding, compose skips query parsing, and the getter throws
"cannot read property 'access_token' of undefined". Node reproduces the
500 on the minified bundle; it is not Capsid-specific. The unminified
reference works because `{ query, ... }` parses cleanly.

The reference app scopes everything after `use(bearer())` through
`app.guard({ query: t.Object({}) }, ...)`: the empty schema forces
`hasQuery` regardless of inference, so source and minified builds parse
query identically and the lazy getter always finds a defined object.
Any plugin with lazy accessors reading query (or any route that dereferences
`context.query`) needs an equivalent declared schema once bundling and
minification are involved.

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
5. Review the three pinned notes above — `gcTime` behavior, the multipart
   inference interaction, and the derive-getter/query-schema interaction
   may change across versions.
