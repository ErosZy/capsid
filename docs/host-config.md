# host.json and capsid.json Configuration Reference

Managed mode (`capsid-host --mode managed`) uses two JSON files to describe the whole
machine and each application version. Both files fail closed: duplicate keys, unknown
fields, invalid enums, and out-of-range values cause startup or deployment to fail
instead of silently passing through with defaults. This document follows the current
implementation in `src/host/config.cc`, `host_config_model.cc`, and `managed_host.cc`.

## Host modes

`capsid-host` has three startup modes. `single-worker` and `static-pool` share one
CLI surface and read at most one local `capsid.json`; `managed` is a separate machine
with its own config file, directory layout, and lifecycle. All three fail closed at
argument time: unknown flags, missing required flags, and invalid values exit before
anything is spawned or bound.

| | `--mode single-worker` | `--mode static-pool` | `--mode managed` |
| --- | --- | --- | --- |
| What it is | One worker serving one app version from a local bundle; the process *is* the service | An arbitrary-size worker pool (each worker owns one shard) sharing one listener over the same bundle — local scale-out | A coordinator machine: many apps and versions, blue-green deployment, Admin API, crash budgets, recovery |
| Config inputs | CLI + optional local `--capsid-json` (the document is the permission authority; there is no host.json) | same as `single-worker` | `--host-config` host.json + per-version capsid.json under `applicationsRoot` (Host ∩ App intersection) |
| Worker count | 1 | `--workers` — any positive integer; each shard costs one worker process plus an Asio loop | per-version capsid.json `pool` (`minReady` / `maxWorkers`) |
| Listener | one `--listen`, listener-level CORS via `--cors-*` | one `--listen` (each shard answers CORS itself), sharded by `SO_REUSEPORT` (Linux/macOS) or a pool-level shared acceptor (Windows) | host.json `listeners` (with CORS per listener) |
| Lifecycle | process lifetime; SIGTERM-bounded shutdown | pool keeps the pool-level READY contract; SIGTERM-bounded shutdown | coordinator + supervised workers; `active.json` generations, retirement, quarantine, crash budgets |
| Deployment | none — direct local run | none — direct local run | staged blue-green: Registry scan → config/artifact snapshot → warm-up/health → atomic switch |
| Production path | no (development/benchmark) | no (benchmark) | yes, Linux only: the managed coordinator requires the strict sandbox |
| Platform | Linux / macOS / Windows | Linux / macOS / Windows | Linux only — macOS and Windows exit at the CLI with a message |

### `single-worker` / `static-pool` CLI

Required:

| Flag | Meaning |
| --- | --- |
| `--mode single-worker` or `--mode static-pool` | mode selection (managed is not valid here) |
| `--worker <path>` | the `capsid-worker` binary |
| `--source-bundle <file>` | the ESM bundle, or with no `--source-bundle` the entry named by the local capsid.json (a production capsid.json runs unchanged) |
| `--source-name <absolute-file-URL>` | module name for `--source-bundle`; required exactly when the flag is given |
| `--application <id>` | App ID (routing + policy); `[a-z0-9._-]`, ≤63 |
| `--listen <host:port>` | listener address |
| `--routing path` \| `subdomain` \| `header` | how a request selects the App |
| `--routing-suffix <suffix>` | required exactly with `--routing subdomain` |
| `--routing-trusted on` | required exactly with `--routing header` |
| `--public-scheme http` \| `https` | the external URL scheme the listener advertises |
| `--public-authority host[:port]` | the external authority |
| `--strict-sandbox on` \| `off` | worker sandbox profile |
| `--ready-fd <fd>` | readiness descriptor the Host writes the READY record to |
| `--workers <positive-integer>` | `static-pool` only, required; any positive count ≤ uint32, each shard is one worker process |

Optional (each keeps the data-plane default when omitted):

| Flag | Meaning |
| --- | --- |
| `--capsid-json <file>` | local permission policy. Default: `./capsid.json` is read when it exists; an explicit path must exist. Absent document = the no-permission baseline (all denied). See [capsid-json.md](capsid-json.md) |
| `--secrets-root <dir>` | local secret store for `env.valueFrom`: one regular file per key id. Without it, `valueFrom` is rejected at policy compile time |
| `--bindings-root <dir>` | scanned immutable Binding registry for local Binding development; capsid.json may only request packages from this snapshot |
| `--request-timeout-ms <ms>` | per-request deadline |
| `--max-inflight-per-worker <n>` | in-flight request cap (0 = unlimited) |
| `--queue-requests <n>` | request queue depth |
| `--queue-header-bytes <size>` | header queue budget (e.g. `2MiB`) |
| `--queue-timeout <duration>` | queued request deadline |
| `--max-streaming-inflight <n>` | concurrent streaming requests per worker (0 = unlimited) |
| `--stream-idle-timeout <ms>` | idle stream deadline |
| `--write-timeout <ms>` | slow-client write deadline (0 = unlimited) |
| `--initial-stream-window <bytes>` | streaming response window (default 64 KiB) |
| `--cors-origins <csv>` | listener-level CORS: allowed origins (`*` or exact `http(s)://host[:port]`, comma-separated). Required with `--cors-methods`; together they engage the listener CORS engine |
| `--cors-methods <csv>` | allowed preflight methods (comma-separated, uppercased at parse). Required with `--cors-origins` |
| `--cors-headers <csv>` | allowed preflight headers (comma-separated, lowercased at parse); optional, empty = none allowed |
| `--cors-max-age <seconds>` | `Access-Control-Max-Age` seconds; optional, 0 = header omitted |

The `--cors-*` flags mirror the managed host.json `listeners[].cors` grammar and
semantics exactly (same validation, same preflight/response behavior). Absent =
the App owns CORS entirely — the listener passes `Access-Control-*` through
untouched.

The same listener/routing validation the managed listeners apply before bind also
runs here: the routing policy (suffix grammar, header-mode trust requirement) is
validated before anything is spawned.

### `managed` CLI

`--mode managed` accepts **only** `--mode`, `--host-config <file>`, and
`--worker <path>`; any other flag is rejected at argument time. Everything else
(applications, listeners, CORS, limits, recovery policy) comes from host.json and
the per-version capsid.json documents described below. On macOS and Windows the
mode exits at the CLI with a message — the managed coordinator requires the Linux
strict sandbox.

## Responsibilities of the two configuration layers

## Responsibilities of the two configuration layers

| File | Location | Responsibility |
| --- | --- | --- |
| `host.json` | passed via `--host-config` | Whole machine: application root, state root, secret root, listeners, global permission caps, capacity, recovery policy |
| `capsid.json` | `<applicationsRoot>/<app>/<version>/` | Single app version: entry point, permission requests, worker resources, request windows, pool size |

The Host's `permissions` and the App's `permissions` are an **intersection**; an App
request cannot expand the Host cap. `maximums` caps App requests (0 = unlimited);
`defaults` are only a machine-level declaration and are not injected into effective
config—fields not written in capsid.json use the worker's own defaults.

## Directory layout

```text
<applicationsRoot>/              # host.json: applicationsRoot
  orders/                        # app id (starts with lowercase letter/digit, [a-z0-9._-], ≤63)
    v1/                          # version id (one directory per version)
      capsid.json                # app-v1 config (required)
      bundle.mjs                 # self-contained ESM bundle (required)
      bundle.qjsb                # trusted bytecode: all three or none
      bytecode.json              #   (bytecode.json = digest/source metadata)
      bytecode.sig               #
<stateRoot>/                     # host.json: stateRoot (owned by the Host; do not edit by hand)
  apps/
    orders/
      active.json                # current active generation (atomically written)
      generations/<generation>/  # config and artifact records snapshotted at deploy time
<secretRootTemplate with {application} replaced>   # e.g. secrets/orders/
  API_TOKEN                      # one plain file per secret key id; file content is the value
```

## host.json

Required fields: `apiVersion`, `applicationsRoot`, `stateRoot`,
`secretRootTemplate` (must contain the `{application}` placeholder), `admin.unix`.
All other fields are optional.

```json
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/applications",
  "stateRoot": "/srv/capsid/state",
  "secretRootTemplate": "/srv/capsid/secrets/{application}",

  "admin": {
    "unix": "/run/capsid/admin.sock",
    "mode": "0600"
  },

  "listeners": [
    {
      "name": "public",
      "tcp": "0.0.0.0:8080",
      "publicScheme": "https",
      "publicAuthority": "orders.example.com",
      "trusted": false,
      "routing": { "mode": "path", "suffix": "" },
      "limits": {
        "connections": 512,
        "headerBytes": "32KiB",
        "headerTimeout": "5s",
        "bodyIdleTimeout": "30s",
        "streamIdleTimeout": "60s"
      },
      "cors": {
        "allowedOrigins": ["https://dev.example.com"],
        "allowedMethods": ["GET", "POST", "OPTIONS"],
        "allowedHeaders": ["content-type", "capsid-app", "access-token"],
        "maxAge": "86400s"
      }
    }
  ],

  "permissions": {
    "modules": ["capsid:env", "capsid:fs", "capsid:storage", "capsid:stdio"],
    "environmentNames": ["APP_MODE", "DATABASE_*"],
    "fsReadRoots": ["/srv/capsid/config"],
    "fetchTargets": ["api.example.com:443", "metrics.example.com:443"],
    "storageNamespaces": ["session"],
    "stdioStreams": ["stdout", "stderr"]
  },

  "isolation": {
    "mode": "strict",
    "required": ["cgroup-v2"],
    "cgroupRoot": "/sys/fs/cgroup/capsid"
  },

  "trustedBytecodeKeys": {
    "2026-08": "/etc/capsid/keys/ed25519-2026-08.pub"
  },

  "defaults": {
    "worker": {
      "jsHeap": "64MiB",
      "processAddressSpace": "256MiB",
      "memoryMax": "256MiB",
      "fileDescriptors": 64,
      "pidsMax": 8
    },
    "request": {
      "timeout": "5s",
      "maxInflightPerWorker": 64,
      "maxStreamingInflightPerWorker": 2,
      "streamIdleTimeoutMs": 60000,
      "writeTimeoutMs": 10000
    },
    "pool": {
      "queueRequests": 256,
      "queueHeaderBytes": "2MiB",
      "queueTimeout": "10s"
    }
  },

  "maximums": {
    "worker": { "memoryMax": "512MiB" },
    "request": { "maxInflightPerWorker": 128 },
    "pool": { "queueRequests": 1024 }
  },

  "capacity": {
    "workersTotal": 16,
    "activationSurgeWorkers": 0,
    "startupsConcurrent": 2,
    "queuedRequestsTotal": 2048,
    "queuedHeaderBytesTotal": "16MiB",
    "workerMemoryCommitTotal": "4GiB"
  },

  "recovery": {
    "crashBudget": { "maxEvents": 5, "window": "60s" },
    "restartBackoff": {
      "initial": "100ms",
      "maximum": "10s",
      "jitter": "10%",
      "stableReset": "60s"
    },
    "replacementsConcurrentPerApp": 1,
    "activeHealthInterval": "5s",
    "activeHealthFailures": 3
  }
}
```

### Field notes and hard validation

- `apiVersion` must be exactly `capsid/host-v1`;
- `admin.mode` accepts only the string `"0600"`; the Admin socket accepts only peers
  with the same euid as the Host (`SO_PEERCRED`/`getpeereid`), and all other processes
  immediately get 403;
- `isolation.mode` accepts only `"strict"`; `required` is an array of extra sandbox
  features (such as `"cgroup-v2"`), and `cgroupRoot` is the delegated cgroup parent
  directory;
- `listeners`: `tcp` is `IP:port`; `publicScheme`/`publicAuthority` are the components
  of the worker-observable URL; `trusted` defaults to false and controls the trust
  boundary for proxy headers;
- `permissions`: the Host-wide allowlist. `environmentNames` supports `NAME*` suffix
  wildcards (same grammar as the runtime `valid_env_pattern`); `fetchTargets` syntax is
  `host` or `host:p1,p2` (comma-separated port list); `fsReadRoots` are read-only roots;
- `trustedBytecodeKeys`: free mapping from release id to Ed25519 public key file path;
- `defaults`/`maximums` `worker`/`request` subfields use the same syntax as capsid.json
  (see [How to write capsid.json](capsid-json.md)); `fileDescriptors` must be ≥1;
- `capacity.workersTotal` is the only whole-machine cap on worker count
  (`activationSurgeWorkers` ≥0, default 0 means zero-downtime replacement is refused);
  `workerMemoryCommitTotal` is the total memory commitment across all workers;
- `recovery` defaults: crashBudget 5 per 60s, backoff initial 100ms maximum 10s,
  jitter 10%, stableReset 60s, concurrent replacements 1. `jitter` syntax is `"10%"`
  (percentage) or a bare integer (basis points).
- Sizes uniformly use the `KiB`/`MiB`/`GiB`/`KB`/`MB`/`GB` suffixes; durations use
  `ms`/`s`/`m`.

## capsid.json (each app version)

**For a step-by-step guide, see [How to write capsid.json (tutorial)](capsid-json.md)**—it
starts from a minimal usable version (3 fields) and adds sections up to a full config,
including value domains for every field, a common error table, and the three deployment
steps. This section only keeps a field quick reference:

| Field | Required | Description |
| --- | --- | --- |
| `apiVersion` | ✓ | Must be exactly `capsid/app-v1` |
| `pool.minReady` | ✓ | Fixed pool size; must equal `maxWorkers` |
| `pool.maxWorkers` | ✓ | Fixed pool size; must equal `minReady` |
| `pool.queueRequests` / `queueHeaderBytes` / `queueTimeout` | | Queue; 0 = queueing disabled; capped by `maximums.pool` |
| `permissions.modules` | | Imported `capsid:*` modules (`tjs:*` must never be enabled) |
| `permissions.env` | | Environment variables; key → `{value}` or `{valueFrom}` (exactly one) |
| `permissions.fs.read.allow` / `deny` | | Read-only roots; `deny` takes precedence over `allow` |
| `permissions.fetch.allow` | | Egress targets `host` or `host:p1,p2`; omitted = all denied |
| `permissions.storage.namespaces` | | Read-only storage namespaces (`[A-Za-z0-9._-]` ≤128) |
| `permissions.stdio` | | Only `stdin`/`stdout`/`stderr` accepted |
| `worker.jsHeap` / `processAddressSpace` / `memoryMax` / `fileDescriptors` / `pidsMax` | | Omitted = worker's own defaults; capped by `maximums.worker` |
| `request.timeout` / `maxInflightPerWorker` / `maxStreamingInflightPerWorker` / `streamIdleTimeoutMs` / `writeTimeoutMs` | | Request windows and SSE slots |
| `healthCheck.path` / `timeout` | | Startup probe; managed mode probes worker-internal, local single-worker/static-pool modes probe through the real listener path; empty = no probe |

Artifact rules for the same directory as capsid.json:

- `bundle.mjs` is required; `bundle.qjsb` + `bytecode.json` + `bytecode.sig` are the
  trusted bytecode triplet, **all or nothing**—if any one is missing, deployment is
  rejected;
- Bytecode is accepted only after Ed25519 signature verification with the corresponding
  release in `trustedBytecodeKeys`, plus digest, exact source name, and Runtime
  compatibility ID checks;
- Each deployment snapshots the config, bundle, and verification results into
  `stateRoot/apps/<app>/generations/<generation>/`; the generation identity changes
  whenever any config or artifact changes.

## Secret files

After substituting `{application}` in `secretRootTemplate`, the result is the secret
directory for that app; the key id referenced by `valueFrom` is the file name inside
that directory:

```sh
mkdir -p /srv/capsid/secrets/orders
printf '%s' 'postgres://user:pass@db.example.com/app' \
  > /srv/capsid/secrets/orders/db-url
chmod 0600 /srv/capsid/secrets/orders/db-url
```

- Key id grammar `[A-Za-z0-9._-]`, no `..`, has a length limit;
- Secret files must be regular files (symlinks are rejected); size/ctime are verified
  before and after reading, and modification in between fails; values enter an
  immutable `capsid:env` snapshot, not the worker process environment.

## Startup and operations

```sh
./build-release/capsid-host --mode managed --host-config /etc/capsid/host.json
```

`--host-config` must be a regular file owned by the Host user (O_NOFOLLOW, euid check,
≤1 MiB, mtime verified before and after reading).

The Admin API only uses a Unix socket (same euid as the Host):

```sh
# Deploy (blue-green: staging/warm-up/health check first, atomic switch, old version kept on failure)
curl --unix-socket /run/capsid/admin.sock \
  -X POST http://localhost/v1/deploy \
  -H 'Content-Type: application/json' \
  -d '{"application":"orders","version":"v2"}'
# → 202 {"operationId":"...","application":"orders","version":"v2","status":"..."}

# Query operation status
curl --unix-socket /run/capsid/admin.sock \
  http://localhost/v1/operations/<operationId>

# App status
curl --unix-socket /run/capsid/admin.sock http://localhost/v1/apps/orders

# Explicitly retire (management action; expressed as a tombstone, not by deleting the directory)
curl --unix-socket /run/capsid/admin.sock \
  -X POST http://localhost/v1/apps/orders/retire

# Metrics
curl --unix-socket /run/capsid/admin.sock http://localhost/metrics
```

All endpoint responses are bounded JSON/text; unknown paths return 404, non-deploy
requests with a body return 400, wrong methods return 405, and unauthorized peers
return 403.

## Request routing

The `routing` object currently defines and reads only `mode` and `suffix`; `mode`
supports three values:

| mode | App source | Constraint |
| --- | --- | --- |
| `path` | The `/@capsid/<app>/` prefix in the URL | Strip the routing prefix, then hand the path to the worker |
| `subdomain` | The single DNS label before `suffix` in the Host | `suffix` must be a valid, portless domain starting with a dot, such as `.apps.example.com` |
| `header` | The sole `Capsid-App` request header | The listener must explicitly set `trusted: true`; duplicate or invalid App IDs are rejected |

Each mode requires exactly one valid Host header. `path` and `header` use the
listener-level `publicScheme` and `publicAuthority` to construct the worker-observable
absolute URL; `subdomain` constructs the URL from `publicScheme`, the extracted app,
and `suffix`. `suffix` is only valid for `subdomain`; the header name is fixed to
`Capsid-App` and cannot be customized. HTTP method is not an app routing condition.

`trusted: true` is an explicit declaration of a security boundary in front of the
listener, not TLS, authentication, or a firewall automatically established by Capsid.
Before enabling header routing, deployers must ensure that only a controlled reverse
proxy or trusted source can reach the listener, and that the proxy removes or
overwrites `Capsid-App` carried by external requests. The typical approach is binding
to loopback/controlled internal network addresses with network ACLs; simply setting
`trusted: true` after exposing the listener to the public Internet is not safe. A
header listener with `trusted: false` fails closed before bind; path and subdomain
routing do not require that declaration.

External reverse proxies (nginx/Caddy/Envoy) handle TLS/H2 termination and external
path, Host, or header mapping. The CLI's `--routing` accepts the same matrix
(`path` / `subdomain` / `header`), with `--routing-suffix` required for
`subdomain` and `--routing-trusted on` required for `header`; the single-worker
server validates the policy before bind, exactly like a managed listener. The
CLI serves one App, so an extracted App that differs from `--application` is
404.

## Listener CORS

The optional `listeners[].cors` object makes the listener itself answer
browser CORS preflights — **before routing** — and stamp the matched
`Access-Control-Allow-Origin` on every response the listener serves. It is an
edge/trust-boundary concern, exactly like `trusted`, and exists because a
preflight can never carry the header-routing control field: the custom header
is precisely what the preflight asks about, so without listener-level CORS
header routing is unreachable from any CORS-enforcing browser client.

| Field | Required | Description |
| --- | --- | --- |
| `allowedOrigins` | ✓ | `"*"` (any origin) or exact `http(s)://host[:port]` origins |
| `allowedMethods` | ✓ | HTTP method tokens; matched case-insensitively |
| `allowedHeaders` | ✓ | Header names; matched case-insensitively |
| `maxAge` | | Preflight cache lifetime (`Access-Control-Max-Age`); omitted = no caching hint |

Semantics:

- `OPTIONS` with `Origin` and `Access-Control-Request-Method` is a preflight:
  the listener answers it itself. Origin, requested method and every requested
  header must match the config; a match is `204` with
  `Access-Control-Allow-Origin` (the echoed origin), `Allow-Methods`,
  `Allow-Headers`, `Vary: Origin` and the optional `Max-Age`. A mismatch is
  `403` with **no** `Access-Control-Allow-*` field — the browser reports the
  CORS failure and no CORS decision leaks.
- Any other request records whether its `Origin` is allowed; both response
  paths (Host-synthesized and worker responses) are then normalized by the
  listener. When `cors` is configured the listener owns the
  `Access-Control-Allow-Origin` field: an App-supplied value is removed and
  replaced with the matched Origin (allowed requests) or removed entirely
  (disallowed requests), so the Host allow-list cannot be bypassed by App
  headers. `Access-Control-Allow-Credentials` survives only for an exact
  allowed origin; wildcard `"*"` and disallowed/absent origins strip it, so
  wildcard can never become any-origin credentialed CORS. `Vary` is merged
  token-wise with `Origin`; an App-supplied `Vary: Accept-Encoding` cannot
  suppress the required `Vary: Origin`.
- A duplicate `Origin` header is malformed control input and is rejected
  with `400` before routing.
- An `OPTIONS` without the preflight fields is not a preflight and routes
  normally (the App may handle it).
- Absent `cors`, nothing changes: the App owns CORS entirely.
