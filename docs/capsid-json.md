# How to Write `capsid.json` (Tutorial)

`capsid.json` describes **one application version**'s permissions, resource needs, and pool size. It lives in the version directory and is deployed together with the bundle:

```text
<applicationsRoot>/<app-id>/<version>/
    capsid.json     ← the subject of this article
    bundle.mjs      ← self-contained ESM bundle (required, same directory)
```

Host configuration (listener, global permission caps, capacity) lives in
[host.json](host-config.md). Permissions on both sides are intersected: what
`capsid.json` requests must also be allowed by `host.json`, otherwise deployment
fails.

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

That is already deployable, but the app cannot import any `capsid:` module, nor
can it make egress fetch calls. Add what you need section by section below.

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
  [host.json reference](host-config.md#secret-files));
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
- The root must also appear in host.json's `permissions.fsReadRoots`.

## Step 5: Outbound network (`permissions.fetch`)

Controls the targets of standard `fetch()`. The syntax is `host` or
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
- The target must also be allowed by host.json's `permissions.fetchTargets`;
- Omitting this section = all outbound fetch is denied (same as host.json's
  `egress_policy == NULL`).

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
  `capsid:stdio` only emits bounded log events and never touches real fds.

## Step 7: Resource limits (`worker`) — optional

Omitting this section = the worker uses the runtime's built-in defaults. Writing
it gives yourself an explicit boundary:

```json
{
  "worker": {
    "jsHeap": "64MiB",
    "processAddressSpace": "256MiB",
    "memoryMax": "256MiB",
    "fileDescriptors": 64,
    "pidsMax": 8
  }
}
```

- Sizes uniformly use one of these suffixes:
  `KiB`/`MiB`/`GiB`/`KB`/`MB`/`GB`; other suffixes (such as bare `256` or `1M`)
  are rejected;
- `jsHeap` limits the QuickJS heap, `processAddressSpace` limits the process
  address space, and `memoryMax` is the overall memory limit — the three are
  independent and do not stand in for one another; the effective memory limit is
  `max(memoryMax, jsHeap, processAddressSpace)`;
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

- `timeout` is the request-level timeout (`ms`/`s`/`m`); after a synchronous CPU
  infinite loop is interrupted, the worker is treated as non-reusable;
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
  -d '{"application":"orders","version":"v2"}'

# 3. Check status
curl --unix-socket /run/capsid/admin.sock http://localhost/v1/apps/orders
```

At deployment time, if the bundle directory also contains `bundle.qjsb`,
`bytecode.json`, and `bytecode.sig`, the trusted bytecode path is used — these
three files are **all-or-nothing**; missing any one rejects deployment.

## Local mode (`--capsid-json`, since v0.1.3): run directly without deploying

`capsid-host --mode single-worker` (and `static-pool`) is the
benchmark/local-development data plane: no blue-green deployment and no Admin
API. In this mode there is no host.json — **the capsid.json document is itself
the permission authority**, and it no longer intersects with host.json:

```sh
# Default reads ./capsid.json in the current directory; absent = fall back to the
# v0.1.2 no-permission baseline (all denied)
capsid-host --mode single-worker --source-bundle bundle.mjs

# Explicit path: the file must exist; a missing file fails startup (not silently skipped)
capsid-host --mode single-worker --source-bundle bundle.mjs \
  --capsid-json ./my-policy.json

# Binding development: the App imports capsid:binding/mongo while the Host
# supplies the trusted package from this explicitly scanned Registry
capsid-host --mode single-worker --source-bundle bundle.mjs \
  --capsid-json ./capsid.json \
  --bindings-root ./bindings
```

- Permissions still apply as usual: `permissions.modules` / `env` / `fs`
  (including `fs.read.deny`, deny still takes precedence over allow) / `fetch` /
  `storage` / `stdio`, through the **exact same** frozen schema validation and
  compilation pipeline as managed mode (rule ids, digests, canonicalization all
  included); every earlier section of this tutorial applies;
- `pool` is still a schema-required field (`minReady` == `maxWorkers`), but the
  worker-count values are lazy — the worker count is decided by the CLI
  (`--workers`);
- The runtime sections are honored locally (v0.2.x), with the CLI as the
  override — an explicit CLI flag always wins over the document:
  - `entry` names the bundle file inside the capsid.json directory; with no
    `--source-bundle`/`--source-name` the Host derives both from it, so a
    production capsid.json runs unchanged;
  - `worker.jsHeap` / `processAddressSpace` / `fileDescriptors` map onto the
    same worker spawn fields as the managed spawn (`memoryMax` stays budget
    accounting);
  - `request.timeout` / `maxInflightPerWorker` / `maxStreamingInflightPerWorker`
    / `streamIdleTimeoutMs` / `writeTimeoutMs` fill the request window;
  - `pool.queueRequests` / `queueHeaderBytes` / `queueTimeout` arm the bounded
    admission queue (document presence decides; 0 = queueing disabled);
  - an armed `healthCheck` gates the READY record on one startup probe
    through the real listener path (non-2xx fails startup);
- env `valueFrom` resolves against an explicit `--secrets-root` directory
  (one regular file per key id, the managed layout); without the root the
  document is rejected at the CLI phase — there is no implicit secret
  store on this path, and a value is never silently empty;
- `capsid/app-v2` Binding declarations are supported for Binding development.
  They use the managed path's Registry scan, Manifest ∩ App permission
  proof, pre-bundle load ordering and READY proof. A declaration without an
  explicit `--bindings-root` fails startup; Binding `secrets.valueFrom` also
  fails because local mode has no secret provider;
- The policy file must be a regular file owned by the current user
  (symlinks/directories/FIFOs are not accepted), at most 1 MiB, and concurrent
  replacement while it is being read rejects startup.

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
| Local mode with `worker` / `request` / `healthCheck` / `entry` sections | Applied locally (v0.2.x); an explicit CLI flag wins over the document |
| Local mode with `pool.queue*` | Applied locally (v0.2.x): document presence arms the bounded admission queue; 0 = queueing disabled |
| Local mode env with `valueFrom` | Rejected at startup: valueFrom is unavailable in local mode |
| `--capsid-json` points to a symlink / directory / file not owned by the user | Rejected at startup: not a regular file / not owned |
| `--capsid-json` points to a nonexistent file | Rejected at startup: cannot find … (except a missing default `./capsid.json`, which is the no-permission baseline) |

All validation completes before deployment; errors are never silently skipped at
runtime.
