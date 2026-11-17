# How to Write `capsid.json` (Tutorial)

> Reviewed against the v0.3.0-beta.2 parser/compiler on 2026-11-17.
> [Current status](current-status.md) records the source and release boundary.

`capsid.json` describes **one application version**'s requested permissions,
resource needs and pool size. It is used in two contexts:

| Context | Location and authority |
| --- | --- |
| Local `single-worker` / `static-pool` | `--capsid-json <file>` or the existing `./capsid.json`; no `host.json`. The document is the local permission authority, and CLI resource options can override its local settings. |
| Linux `managed` | A version directory under `applicationsRoot`; requests are intersected with the Host's `host.json` limits. |

Start with [getting started](getting-started.md) for a complete local command.
An absent optional local document means a deny-all capability/egress baseline,
not ambient authority. An explicitly named missing document is an error.
Configuration ownership and file-safety checks still apply in trusted local mode.

In managed mode, the document is deployed together with its bundle:

```text
<applicationsRoot>/<app-id>/<version>/
    capsid.json     ← the subject of this article
    bundle.mjs      ← self-contained ESM bundle (required, same directory)
```

For managed mode, machine configuration (listeners, global permission caps and
capacity) lives in [host.json](host-config.md). Requests must also be allowed by
that Host authority; conflicting requests fail deployment. Local modes do not
create a managed coordinator or acquire managed deployment features merely by
reading the same application document.

This article walks through "minimal usable → add features step by step → full
example." All fields follow the implementation in `src/host/config.cc` and
`managed_host.cc`.

## Step 1: Minimal usable version (3 fields is enough to run)

```json
{
  "apiVersion": "capsid/app-v1",
  "pool": {
    "minReady": 2,
    "maxWorkers": 2
  }
}
```

- `apiVersion`: use `capsid/app-v1` for the baseline schema, or
  `capsid/app-v2` when declaring Host Bindings; any other value is rejected;
- `pool.minReady` and `pool.maxWorkers`: **both required**, and the values must be
  equal (the v1 pool is fixed-size). `2` means this version always maintains 2
  workers.

This is a minimal managed application document, not a complete standalone
launch: the bundle, valid Host configuration and deployment prerequisites are
still required. It requests no `capsid:` modules or egress Fetch targets. In
local modes the actual worker count comes from the mode/`--workers` CLI, not an
implicit managed pool. Add only the authority you need below.

## Step 2: Module permissions (`permissions.modules`)

For every `capsid:` module the app `import`s, you must list it here, otherwise
the import fails directly (`module is not authorized`):

```json
{
  "permissions": {
    "modules": ["capsid:env", "capsid:fs", "capsid:storage", "capsid:stdio"]
  }
}
```

Available modules are listed in the
[Modules and Permissions Reference](module-permissions.md). Rules:

- Only public `capsid:*` names are accepted; writing `tjs:*` or `tjs:internal/*`
  is **directly rejected** — these are permanently forbidden, not open items;
- Pure utility modules (`capsid:assert`, `capsid:hashing`, etc.) must also be
  listed individually;
- Standard `fetch()` does not go through this section — outbound networking uses
  `permissions.fetch` below.

## Step 3: Environment variables (`permissions.env`)

The environment variables the app reads **come only from here** (the worker
process environment is cleared). There are two sources, **exactly one of which
must be chosen**:

```json
{
  "permissions": {
    "env": {
      "APP_MODE": { "value": "production" },
      "DATABASE_URL": { "valueFrom": "db-url" }
    }
  }
}
```

- `value`: write a literal value directly;
- `valueFrom`: references a secret file. The key id `db-url` corresponds to the
  regular file `<secretRootTemplate replaced with {application}>/db-url`, whose
  contents are the value (see the
  [host.json reference](host-config.md#secret-files)). Local User-environment
  references instead require explicit `--secrets-root`; Binding secrets remain
  unavailable in local modes;
- Key name grammar: **starts with a letter or `_`**, and subsequent characters
  may only be letters, digits, or `_` (no `*`, and it cannot start with a
  digit) — `1APP_MODE` and `APP*MODE` are rejected;
- The same key **cannot have both `value` and `valueFrom`**; writing both or
  neither is rejected;
- Environment value sizes are subject to runtime constraints; oversized values
  fail during snapshot compilation.

## Step 4: Read-only filesystem (`permissions.fs`)

`capsid:fs` can only read, not write. `allow` is the authorization root, and
`deny` takes precedence over `allow`:

```json
{
  "permissions": {
    "fs": {
      "read": {
        "allow": ["/srv/capsid/config"],
        "deny": ["/srv/capsid/config/private.json"]
      }
    }
  }
}
```

- Paths must be canonical absolute paths; under strict sandbox an authorization
  root cannot be a symlink;
- In managed mode, the root must also appear in host.json's
  `permissions.fsReadRoots`. Local mode has no host.json intersection.

## Step 5: Outbound network (`permissions.fetch`)

Controls application Fetch egress; the Host-mediated outbound WebSocket/v4
path also uses effective application egress authority. It does not expose a raw
socket or listener. The syntax is `host` or
`host:p1,p2` (port list):

```json
{
  "permissions": {
    "fetch": {
      "allow": ["api.example.com:443", "metrics.example.com:443"]
    }
  }
}
```

- Every request checks the hostname, **every address** returned by DNS
  resolution, and **every redirect**;
- In managed mode, the target must also be allowed by host.json's
  `permissions.fetchTargets`; local mode uses the application document directly;
- Omitting this section denies outbound Fetch. At the C API layer a null
  direct `egress_policy` also denies Fetch, even if a separate capability net
  policy has grants.

## Step 6: In-memory storage and logging (`permissions.storage` / `permissions.stdio`)

```json
{
  "permissions": {
    "storage": { "namespaces": ["session"] },
    "stdio": ["stdout", "stderr"]
  }
}
```

- `storage.namespaces`: accepts only ASCII letters, digits, `_`, `-`, `.`, up to
  128 characters; each namespace has an independent quota and lives only in a
  single worker;
- `stdio`: accepts only the three strings `stdin`/`stdout`/`stderr`;
  `capsid:stdio` only emits bounded stdout/stderr log events and never touches
  real fds. A parsed `stdin` grant does not implement a stdin API.

## Step 7: Resource limits (`worker`) — optional

Omitting this section = the worker uses the runtime's built-in defaults. Writing
it gives yourself an explicit boundary:

```json
{
  "worker": {
    "jsHeap": "64MiB",
    "processAddressSpace": "256MiB",
    "memoryMax": "256MiB",
    "fileDescriptors": 64
  }
}
```

- Sizes uniformly use one of these suffixes:
  `KiB`/`MiB`/`GiB`/`KB`/`MB`/`GB`; other suffixes (such as bare `256` or `1M`)
  are rejected;
- `jsHeap` and `processAddressSpace` keep separate VM/address-space fields.
  The managed compiler's effective Host memory/budget value is
  `max(memoryMax, jsHeap, processAddressSpace)`; a smaller `memoryMax` must not be
  interpreted as an independently tighter Host ceiling. A stated process ceiling
  below the heap is rejected. macOS rejects nonzero processAddressSpace;
- `pidsMax` is recognized by the current App schema but is not propagated as an
  effective App PID limit. Do **not** rely on it as enforced application policy.
  The separate Linux C API/delegated cgroup resource boundary is documented in
  [Linux sandboxing](linux-sandbox.md); accepted JSON is not proof of enforcement;
- `fileDescriptors` must be ≥ 1;
- All values are capped by host.json `maximums`: exceeding them fails deployment
  (`maximums` value 0 = unlimited). host.json `defaults` are only a whole-host
  declaration and are not injected into the effective configuration.

## Step 8: Request window (`request`)

```json
{
  "request": {
    "timeout": "5s",
    "maxInflightPerWorker": 64,
    "maxStreamingInflightPerWorker": 2,
    "streamIdleTimeoutMs": 60000,
    "writeTimeoutMs": 10000
  }
}
```

- `timeout` is a positive request-level duration (`ms`/`s`/`m`). Managed
  deployments use the App value, then `defaults.request.timeout`, then the
  Runtime's 30s default. `maximums.request.timeout` bounds the selected value;
  an explicit App value above that ceiling is rejected. With only a Host
  ceiling configured, the default is the smaller of 30s and that ceiling.
  Changing the selected duration changes the generation's effective identity.
  After a synchronous HTTP CPU infinite loop is interrupted, the worker is
  treated as non-reusable;
- after a WebSocket Upgrade, each active connection callback or Promise turn
  receives a fresh execution budget from this duration. An exhausted budget
  ends that connection and preserves the realm for subsequent requests. Idle
  connection time follows the separate channel idle-timeout configuration;
- `maxInflightPerWorker` is the per-worker concurrent request window;
- `maxStreamingInflightPerWorker` is the SSE/streaming slot count (default 2),
  and `streamIdleTimeoutMs` is the stream idle timeout — slow clients are covered
  by `writeTimeoutMs`.

## Step 9: Queue (`pool` optional fields)

```json
{
  "pool": {
    "minReady": 2,
    "maxWorkers": 2,
    "queueRequests": 256,
    "queueHeaderBytes": "2MiB",
    "queueTimeout": "10s"
  }
}
```

- Default 0 = queueing disabled; load beyond the window is directly rejected;
- Also capped by host.json `maximums.pool`.

## Step 10: Health check (`healthCheck`)

```json
{
  "healthCheck": {
    "path": "/health",
    "timeout": "2s"
  }
}
```

- `path` is a **worker-internal path** (the Host sends the request directly to
  the worker, not through listener routing), so write the path the app itself
  uses in `fetch()`, such as `/health`;
- Omitting it or using an empty path = no probing; the worker is replaced only
  after consecutive failures exceed host.json `recovery.activeHealthFailures`.

## Complete example

An order app that reads configuration, calls upstream, uses storage, and has a
health check:

```json
{
  "apiVersion": "capsid/app-v1",

  "permissions": {
    "modules": ["capsid:env", "capsid:fs", "capsid:storage", "capsid:stdio"],
    "env": {
      "APP_MODE": { "value": "production" },
      "DATABASE_URL": { "valueFrom": "db-url" }
    },
    "fs": {
      "read": {
        "allow": ["/srv/capsid/config"],
        "deny": ["/srv/capsid/config/private.json"]
      }
    },
    "fetch": {
      "allow": ["api.example.com:443"]
    },
    "storage": {
      "namespaces": ["session"]
    },
    "stdio": ["stdout", "stderr"]
  },

  "worker": {
    "jsHeap": "64MiB",
    "processAddressSpace": "256MiB",
    "memoryMax": "256MiB",
    "fileDescriptors": 64
  },

  "request": {
    "timeout": "5s",
    "maxInflightPerWorker": 64,
    "maxStreamingInflightPerWorker": 2,
    "streamIdleTimeoutMs": 60000,
    "writeTimeoutMs": 10000
  },

  "pool": {
    "minReady": 2,
    "maxWorkers": 2,
    "queueRequests": 256,
    "queueHeaderBytes": "2MiB",
    "queueTimeout": "10s"
  },

  "healthCheck": {
    "path": "/health",
    "timeout": "2s"
  }
}
```

## Deployment in three steps

```sh
# 1. Put capsid.json and the bundle into the version directory (app id: starts with
#    a lowercase letter/digit, [a-z0-9._-], ≤63 chars; version id uses the same grammar)
mkdir -p /srv/capsid/applications/orders/v2
cp capsid.json bundle.mjs /srv/capsid/applications/orders/v2/

# 2. Deploy through the Admin API (Unix socket, same euid as the Host; blue-green:
#    prewarm + health check, atomic switch, failure keeps the old version)
curl --unix-socket /run/capsid/admin.sock \
  -X POST http://localhost/v1/deploy \
  -H 'Content-Type: application/json' \
  -d '{"app":"orders","version":"v2"}'

# 3. Check that exact returned operation, within a finite client deadline
operation_id='replace-with-the-returned-operationId'
curl --unix-socket /run/capsid/admin.sock \
  "http://localhost/v1/operations/${operation_id}"
# Require the matching version and state=active before the first expected request.
# A failed state/deadline is a failure, not a reason to repeat the deploy POST.
```

At deployment time, if the bundle directory also contains `bundle.qjsb`,
`bytecode.json`, and `bytecode.sig`, the trusted bytecode path is used — these
three files are **all-or-nothing**; missing any one rejects deployment.

## Local mode (`--capsid-json`, since v0.1.3): run directly without deploying

`capsid-host --mode single-worker` (and `static-pool`) is the
benchmark/local-development data plane: no blue-green deployment and no Admin
API. In this mode there is no host.json — **the capsid.json document is itself
the permission authority**, and it no longer intersects with host.json:

Use the minimal app/policy from [getting started](getting-started.md), not the
managed full example with memoryMax and managed secrets above. A complete POSIX
trusted-local invocation is:

```sh
CAPSID_BIN="$HOME/.local/bin"
"$CAPSID_BIN/capsid-host" --mode single-worker \
  --worker "$CAPSID_BIN/capsid-worker" \
  --source-bundle ./app.bundle.js \
  --source-name "file://$PWD/app.bundle.js" \
  --application hello --listen 127.0.0.1:8080 \
  --routing path --public-scheme http \
  --public-authority 127.0.0.1:8080 \
  --request-timeout-ms 5000 --strict-sandbox off \
  --ready-fd 1 --capsid-json ./capsid.json
```

The explicit file must exist. Omitting `--capsid-json` uses an existing
`./capsid.json`, or a deny-all baseline when that default is absent. For Binding
development add `--bindings-root ./bindings` and a valid app-v2 declaration;
there is no implicit registry. Use the guide's PowerShell form on Windows.
These local examples do not establish production isolation.

- Permissions still apply as usual: `permissions.modules` / `env` / `fs`
  (including `fs.read.deny`, deny still takes precedence over allow) / `fetch` /
  `storage` / `stdio`, through the **exact same** frozen schema validation and
  compilation pipeline as managed mode (rule ids, digests, canonicalization all
  included); every earlier section of this tutorial applies;
- `pool` is still a schema-required field (`minReady` == `maxWorkers`), but the
  worker-count values are lazy — the worker count is decided by the CLI
  (`--workers`);
- The runtime sections below are honored locally in the current interface, with the CLI as the
  override — an explicit CLI flag always wins over the document:
  - `entry` names one plain bundle file inside the capsid.json directory
    (no separators, drive letters, or `..` components); with no
    `--source-bundle`/`--source-name` the Host derives both from it, so a
    production capsid.json runs unchanged;
  - `worker.jsHeap` / `processAddressSpace` / `fileDescriptors` map onto the
    same worker spawn fields as the managed spawn; `memoryMax` is rejected in
    local mode because there is no local cgroup budget to apply it to;
  - `request.timeout` / `maxInflightPerWorker` / `maxStreamingInflightPerWorker`
    / `streamIdleTimeoutMs` / `writeTimeoutMs` fill the request window;
  - `pool.queueRequests` / `queueHeaderBytes` / `queueTimeout` arm the bounded
    admission queue (document presence decides; 0 = queueing disabled);
  - an armed `healthCheck` gates the READY record on one startup probe
    through the real listener path (non-2xx fails startup);
- env `valueFrom` resolves against an explicit `--secrets-root` directory
  (one regular file per key id, the managed layout); each value follows the
  managed secret contract (at most 16 KiB, no embedded NUL, valid UTF-8).
  Without the root the document is rejected at the CLI phase — there is no
  implicit secret store on this path, and a value is never silently empty
  or silently truncated;
- `capsid/app-v2` Binding declarations are supported for Binding development.
  They use the managed path's Registry scan, Manifest ∩ App permission
  proof, pre-bundle load ordering and READY proof. A declaration without an
  explicit `--bindings-root` fails startup; Binding `secrets.valueFrom` also
  fails because local mode has no secret provider;
- The policy must be a bounded regular file; symlink/reparse/directory/FIFO and
  concurrent replacement checks apply. POSIX additionally requires the current
  effective UID. Windows has no POSIX UID comparison; do not confuse that with
  the separate Binding registry's explicit owner/ACL trust checks.

In static-pool mode the Host reads and compiles the local policy once and
shares that immutable result with every shard; each worker loads the same
Binding set, and if any shard fails to load, the whole pool fails startup.

## Common errors (all fail closed)

| Invalid input | Result |
| --- | --- |
| Local `capsid/app-v2` declares a Binding without `--bindings-root` | Rejected at startup: Binding requires an explicit Host Registry |
| Local Binding declares `secrets.valueFrom` | Rejected at startup: local mode has no Binding secret provider |
| `"modules": ["tjs:assert"]` | Rejected: `tjs:*` is permanently forbidden |
| `"minReady": 2, "maxWorkers": 4` | Rejected: cross-field values must be equal |
| Only `minReady`, no `maxWorkers` | Rejected: missing required field |
| `"env": { "1APP": {...} }` | Rejected: environment key names must start with a letter/`_` |
| An env entry with both `value` and `valueFrom` | Rejected: exactly one is required |
| `"fetch": { "allow": ["example.com:70000"] }` | Rejected: port out of range |
| `"stdio": ["log"]` | Rejected: only stdin/stdout/stderr are accepted |
| `"storage": { "namespaces": ["a/b"] }` | Rejected: invalid character in namespace |
| `"worker": { "jsHeap": "64" }` | Rejected: sizes must have a KiB/MiB/GiB/KB/MB/GB suffix |
| Duplicate key (same object appears twice, e.g. `pool`) | Rejected: JSON_REJECT_DUPLICATES |
| Any unlisted field (such as `"cpu": 2`) | Rejected: unknown configuration field |
| Request exceeds host.json `maximums` | Deployment rejected |
| Bundle directory has only `bundle.qjsb` without a signature | Rejected: bytecode must be all-or-nothing |
| Local mode with `worker` / `request` / `healthCheck` / `entry` sections | Applied locally in the current interface; an explicit CLI flag wins over the document |
| Local mode with `pool.queue*` | Applied locally: document presence arms the bounded admission queue; 0 = queueing disabled |
| Local mode env with `valueFrom` but no `--secrets-root` | Rejected at startup: valueFrom is unavailable without an explicit secret store |
| Local mode env `valueFrom` file with NUL, invalid UTF-8, or over 16 KiB | Rejected at startup: value must be at most 16384 bytes of NUL-free UTF-8 text |
| Local mode `entry` with `/`, `\`, `:`, or a `..` component | Rejected at startup: entry must be a plain file name inside the capsid.json directory |
| `--capsid-json` is a symlink/reparse path, directory, or a POSIX file owned by another UID | Rejected by the applicable platform file-safety/ownership checks |
| `--capsid-json` points to a nonexistent file | Rejected at startup: cannot find … (except a missing default `./capsid.json`, which is the no-permission baseline) |

All validation completes before deployment; errors are never silently skipped at
runtime.
