# Capsid Host v1 Detailed Design

> Status: authoritative v1 design; the M1 data plane and secure deployment loop are delivered (as evidenced by the current source and tests).
> static-pool/managed are runnable benchmark/integration modes, not production deployment interfaces.
> Runtime authoritative interface: [runtime.h](../include/capsid/runtime.h); integration constraints are documented in [Third-party host integration guide](host-integration.md).

## 1. Document Scope and Frozen Conclusions

This document is the sole authoritative design for the first-party Host. Old planning documents, review processes, and daily status pages have been deleted; whether a capability is complete is determined by the current source and tests, and cannot be inferred from the existence of this design.

Frozen core decisions:

- Host is an independent first-party host process; Runtime continues to own only the single worker and FetchRPC;
- One Host ceiling intersected with one App request; the App cannot expand permissions;
- Each worker belongs to exactly one immutable App Version for its entire lifetime;
- A new version is switched to only after prewarming succeeds; any failure keeps the old version;
- Each worker is permanently owned by one event-loop owner, which fully executes credit, cancel, and drain;
- v1 uses HTTP/1.1; TLS/HTTP/2 are left to mature reverse proxies;
- No io_uring, shared-memory IPC, or custom HTTP parser is introduced based on speculative performance.

v1 also freezes the following constraints:

1. v1 supports both source and trusted bytecode; `bundle.qjsb` may enter the trusted bytecode API only after passing signature provenance, digest, exact source name, and Runtime compatibility ID checks;
2. The secret value read by `env.valueFrom` enters the worker as an immutable `capsid:env` snapshot; this is an explicit v1 security contract, not an implementation leak;
3. `host.json` contains the listener, admin socket, global capacity, and queue hard limits;
4. Permission intersection is not simple string-set intersection; it requires a typed, normalized Policy Compiler;
5. The deployment API promises blue-green semantics from the first phase, so staging, prewarming, atomic switchover, and drain must form a vertical loop in that first phase;
6. `active.json` needs explicit atomic write and recovery semantics, but v1 **does not need a database**;
7. compatibility identity and the attestation verifier are provided by the M0 core; structured startup errors and non-blocking worker reclamation must be complete before the data plane is wired in, and reclamation must not block the reactor;
8. Host uses a single HTTP framing authority and fixes hop-by-hop headers, streaming bodies, slow clients, and automatic retry rules;
9. active generation uses worker crash replacement, exponential backoff, cross-App fairness, and a crash budget; exceeding the budget fails closed;
10. Explicit decommissioning is expressed through a retire management action and crash-safe tombstone, not by deleting a directory;
11. The worker-observable absolute URL, path rewrite, and `Forwarded`/`X-Forwarded` trust boundary must be frozen by the single normalization contract in 8.2; listeners must not derive another rule set;
12. After deployment, use minimal continuous health probes; SSE long connections have separate capacity protection;
13. The residual permission boundary between the public C++ Host and the global Admin socket is explicitly written into the threat model.
14. Linux is the v1 production target; native macOS/Windows development is a separate product contract; Host only decides and verifies isolation capabilities, Runtime is responsible for platform process/transport/sandbox, and unsupported production isolation must fail closed.

v1 technology stack:

| Area | Choice | Reason |
| --- | --- | --- |
| Host language | C++20, used only by Host targets | Directly calls the existing C ABI; reuses CMake and C++ project experience |
| Event loop and HTTP/1 | Boost.Asio + Boost.Beast | Covers epoll/kqueue/IOCP; the platform adapter owns the worker event source; provides incremental HTTP/1 parser/serializer |
| Config JSON | Jansson | Small API; can explicitly use `JSON_REJECT_DUPLICATES`, suitable for security-sensitive config |
| Digest and signature verification | OpenSSL `EVP` SHA-256 / Ed25519 | Does not hand-write hash or signature implementations; can be reused later if TLS is needed |
| Persistent state | Plain files + `fsync` + atomic `rename` | Single process, single writer is sufficient; no SQLite |
| Metrics | Built-in fixed metrics + `/metrics` text endpoint | v1 does not introduce a full telemetry SDK; avoids high cardinality and exporter failures |
| TLS/H2 | External nginx/Caddy/Envoy | Converge publishing, scheduling, and isolation first without maintaining an edge protocol stack |

Options not selected:

| Option | Why v1 did not select it |
| --- | --- |
| raw epoll + hand-written HTTP state machine | Duplicates parser, timer, partial-packet, and lifecycle work; zero security benefit |
| Rust/Tokio/Hyper | The memory-safety advantage is real, but the current repository has no Rust foundation; it would add a second build system, FFI, and CI. If the team's core competence shifts to Rust, it can be reevaluated |
| Continue Go/cgo | Kept as the A/B baseline; whether the first-party Host is faster because it directly owns worker fds and the ABI is still to be proven by data |
| SQLite/other database | v1 is single-process single-writer; it needs only one active-version pointer, with no database queries or transaction requirements |
| Boost.JSON as the security-sensitive config parser | Duplicate keys use last-wins, which cannot directly satisfy fail-closed configuration requirements |

The integration constraint that remains in effect: `capsid_worker_destroy()` is a synchronous bounded reclamation path that can wait hundreds of milliseconds in the worst case, so it cannot run in a data-plane reactor callback. The host should first remove the worker from its owner shard and shut it down, then hand the sole handle to a bounded reaper executor.

## 3. Key Contract Additions

### 3.1 Product Boundary

The boundary "Runtime owns the single worker; Host owns HTTP, routing, pools, publishing, and overload" is correct. `capsid-host` is a standalone executable, and Host internal components use `capsid_host_core`, but v1 does not promise a second public stable ABI.

### 3.2 Two-Layer `host.json` / `capsid.json` Model

A two-layer model rather than a multi-layer realm/tenant/policy-directory model helps v1 converge. However, "intersection" must be defined as a typed partial order:

- module: exact name-set containment;
- env: exact keys or a single trailing wildcard rule allowed by Host;
- fs: ancestry is determined by normalized path components, not string prefix matching;
- fetch: compiled by hostname/IP/CIDR, port range, and deny priority;
- storage/stdio: exact resource sets;
- numeric limits: App values are less than or equal to Host maximums;
- sandbox feature: only Host decides; App has no override fields.

The Policy Compiler must output a normalized `effective.json`, a Runtime descriptor, and a stable rule ID lookup table. Deployment failures should return a JSON pointer, for example:

```json
{
  "code": "PERMISSION_EXCEEDS_HOST",
  "field": "/permissions/fs/read/allow/0",
  "requested": "/srv/capsid/data/orders",
  "allowedBy": "/permissions/fsReadRoots"
}
```

### 3.3 Fetch Scheme

In an early config sample, the Host ceiling was written as `*.internal.example.com:443` while the App request was written as `https://orders-api.internal.example.com:443`. The current Runtime egress check only accepts host/IP/CIDR and port; it cannot distinguish `http` from `https`.

The v1 config syntax is uniformly `host:port` and explicitly does not promise a URI scheme. If HTTPS-only is needed later, the worker/ABI must first be extended so policy evaluation includes the scheme; config cannot express a security promise that Runtime cannot enforce.

### 3.4 Secret Semantics

v1 explicitly accepts `API_TOKEN` entering a worker through `capsid:env`: Host puts the token value into the HELLO environment snapshot, and the app reads it only through approved `capsid:env` keys. The boundary is fixed as:

- Do not pass secret file paths to the worker;
- Do not expose secrets as process environment variables;
- Do not write them into `effective.json`, logs, errors, or metrics;
- The worker receives only the key/value snapshot the App explicitly requested and Host explicitly allowed;
- Each value is at most 16 KiB, at most 256 entries, and all name + value bytes total at most 48 KiB, consistent with existing Runtime validation; values cannot contain NUL;
- Secret changes do not mutate READY workers in place; instead, create a new generation, prewarm it, and atomically replace;
- Host clears the temporary read buffer as soon as possible after spawn returns, but does not claim it can erase every copy already created in allocators, IPC, or the worker.

The pure Policy Compiler boundary frozen by M0.3 does not receive paths or ambient environment. It receives only App env requests, the Host environment allowlist, and `(keyId, value, opaqueRevision)` tuples produced by a later safe-read provider. App env names use Runtime's ASCII identifier syntax; the Host allowlist additionally permits one trailing `*`. Exactly one of `value`/`valueFrom` must appear. Secret key IDs are at most 128 bytes and allow only ASCII letters, digits, `_`, `-`, and `.`, with no `..`; opaque revisions are at most 256 bytes and allow only ASCII letters, digits, `.`, `_`, `:`, `@`, `+`, and `-`. Provider output must exactly equal the requested distinct key set; missing, duplicate, or extra material all fail closed. All config strings reject NUL. When an object member name contains NUL, Host follows Jansson's upstream strict parsing behavior and reports `kInvalidJson` at the document root rather than relaxing the vendored parser to obtain a finer path.

This is a deliberate capability model: application code granted that key permission can read the value. v1 no longer promises both "use secrets through `capsid:env`" and "secret content never enters the worker," which were mutually exclusive goals.

### 3.5 Trusted Bytecode Trust Chain

The public header explicitly states that attacker-controlled, corrupted, or incompatible QuickJS bytecode can cause memory corruption. A compatibility ID only addresses whether the format matches; it cannot make untrusted bytes trusted.

Therefore v1 keeps both source and bytecode, but bytecode must satisfy the full provenance contract:

- `bundle.mjs` is always the required semantic source and compatibility fallback;
- `bundle.qjsb`, `bytecode.json`, and `bytecode.sig` must appear as a group;
- `capsid-bytecode-compile` must come from the same release as the target Host and link the same QuickJS configuration; the tool generates bytecode and the attestation to be signed directly from source, and does not offer a "re-sign any existing qjsb" mode;
- The build pipeline signs with an offline Ed25519 private key; Host configures only key ID to public key trust roots;
- The attestation always includes schema, App, Version, exact `sourceName`, source SHA-256, bytecode SHA-256, Runtime/QuickJS compatibility ID, and key ID;
- The signature covers a binary message with a domain separator, fixed field order, and length prefixes; it does not depend on JSON object key order or ad hoc canonicalization rules;
- After Host securely copies the three files, it first rejects duplicate/unknown fields, then verifies the key, signature, both digests, App, Version, and `sourceName`; any provenance or digest failure makes deployment fail;
- If the signature is valid but the compatibility ID does not match the current worker, record the reason explicitly and fall back to the same Version's `bundle.mjs`; call `capsid_worker_load_trusted_bytecode_named()` only when it matches;
- Key revocation affects future deployments and restart recovery; READY workers are not mutated in place and follow generation replacement rules.

The v1 shape of `bytecode.json` is fixed as:

```json
{
  "schema": "capsid-bytecode-v1",
  "application": "orders",
  "version": "2026-07-31-002",
  "sourceName": "bundle.mjs",
  "sourceSha256": "sha256:...",
  "bytecodeSha256": "sha256:...",
  "compatibilityId": "sha256:...",
  "keyId": "release-2026"
}
```

`bytecode.sig` is exactly 64 bytes of raw Ed25519 signature. The signed message starts with `"capsid-bytecode-attestation-v1\0"`, followed by each field's 32-bit big-endian length and UTF-8 bytes in the order listed above. Ordinary claims must each be at most 1024 UTF-8 bytes; `schema` must be exactly `capsid-bytecode-v1`, and the three SHA-256 fields must be exactly `sha256:` followed by 64 lowercase hex digits. These structural/syntax checks run before signature verification; after verification, Host compares deployment-time expected claims, actual source/bytecode digests, and the current compatibility ID. The diagnostic path for unknown JSON keys must apply RFC 6901 escaping. Host rebuilds the message and verifies the signature with OpenSSL EVP.

In the M0.2 selector input, source is always present; `bundle.qjsb`, `bytecode.json`, and `bytecode.sig` are expressed as all-or-none optional files. When all three are absent, select source; if only some are present, reject immediately. Select trusted bytecode only when all three are complete, provenance is valid, and identity matches. Only when the signature, claims, and both digests are valid but identity differs, return the source fallback with a `/compatibilityId` reason. Any unsigned identity tampering must surface as a signature failure, not masquerade as a compatibility fallback. The verification result also keeps the safe key ID and the original attestation SHA-256 for generation identity.

The selector has exactly four outcomes:

| Version directory state | Result |
| --- | --- |
| None of the three bytecode files exist | Load source |
| All three are present, provenance is valid, identity matches | Load trusted bytecode |
| All three are present, provenance is valid, only identity mismatches | Record the reason and load source |
| Files are incomplete or any provenance check fails | Deployment fails; the old version remains active |

The current repository already has the trusted bytecode Runtime API, the worker loading path, the official compiler target, compatibility identity, and the attestation verifier. The Host data plane may hand only artifacts that fully pass this section's trust chain to the trusted API.

### 3.6 Deployment Vertical Loop

The minimal vertical loop for `/v1/deploy` is fixed as:

```text
secure read → validate/compile → internal snapshot → spawn/load/READY
       → optional health check → atomic active switchover → old pool drain
```

Autoscaling, full reload, TLS/H2, and multiple transports may be deferred, but atomic switchover and keeping the old version on failure cannot be deferred.

### 3.7 Listener Configuration

The current `host.json` sample has no listener, but later sections support subdomain, path, and trusted header routing at the same time, which cannot determine the bind address, routing mode, or trust boundary. Each listener must configure exactly one primary routing mode to avoid implicit priority.

Header routing is allowed only on a Unix socket or a separate internal TCP listener with mTLS/source allowlist. Public listeners must delete same-named control headers supplied by clients.

### 3.8 Whole-Machine Resource Limits

`memoryMax` and `cpuQuota` are per-worker limits; `maxWorkers` multiplies into App and whole-machine resource usage. Host also needs:

- Global worker count and startup concurrency;
- Global committed memory;
- Total READY/starting/draining workers per App;
- Temporary capacity for two pools during blue-green;
- App/global queued requests and queued header bytes;
- Per-listener connection, header, body, and timeout limits.

A memory/startup permit must be acquired before spawning a worker; if the permit fails, refuse to prewarm. Do not over-spawn first and then wait for a cgroup OOM.

### 3.9 Defaults Must Be Calibrated by Evidence

Values such as `maxInflight=32` and `maxWorkers=16` in the sample are discussion values only and cannot be frozen directly as `host-v1` defaults. The existing performance documentation also emphasizes that the optimal worker/inflight values differ by workload.

During the v1 alpha phase, require an explicit pool size first, run capacity scans on fixed workloads, and only then freeze defaults. Runtime's own ABI defaults are a bottom-line fallback and should not automatically become Host product defaults.

### 3.10 No-Config Startup

"Runs without a `host.json`" must not mean "automatically opens a public listener." The recommended default behavior is:

- strict sandbox and deny-all capability/egress;
- no TCP data listener bound;
- only a local Unix admin socket with mode `0600`;
- explicit failure when the state/app root is missing or has unsafe ownership;
- failure without degradation when the target Linux lacks a required isolation feature;
- operators must explicitly configure a listener before serving external traffic.

This still allows a self-contained App to deploy with zero permissions while avoiding an accidental network entry point from missing configuration.

### 3.11 `capsid:storage` Is Not Shared Persistent Storage

Currently `capsid:storage` exists only in single-worker memory. When an App enables storage and the pool has more than one worker, Host validation must emit an explicit warning; scaling, worker crashes, and version switchovers all lose or fork state. v1 does not provide affinity to hide this fact, and namespace naming must not be interpreted as a cross-worker database.

### 3.12 Host Config Reload Deferred

When the security ceiling tightens, old READY workers still hold the old snapshot; "per-App smooth reload" would create a window where old and new security policies coexist. v1 does not perform in-process security reload; it only provides config validate/plan, and changes are applied through a controlled Host restart or an external dual-instance switchover. If reload is implemented later, it must explicitly decide whether tightening immediately cancels old requests or allows a bounded drain window; it cannot choose implicitly.

## 4. Target Architecture

```text
                    Local Admin endpoint
                   (Unix socket in v1 production)
                                │
                         Admin HTTP/1 API
                                │
                  ┌──────── Control Plane ────────┐
                  │ config / policy / deploy      │
                  │ artifact snapshot / registry  │
                  └──────────────┬────────────────┘
                                 │ immutable snapshot
        ┌────────────────────────┼────────────────────────┐
        ▼                        ▼                        ▼
  Reactor shard 0          Reactor shard 1          Reactor shard N
  listener/client fd       listener/client fd       listener/client fd
  local worker pool        local worker pool        local worker pool
  request state            request state            request state
        │                        │                        │
        └── WorkerEventSource adapter / FetchRPC / credit ──┘
                                 │
                bounded bootstrap + reaper executors
                                 │
                         isolation boundary
              delegated cgroup / Host network environment
```

### 4.1 Processes and Threads

`capsid-host` uses:

- 1 control thread: configuration, deployment/recovery state machine, `active.json`, and registry publication;
- `N` reactor threads: one `boost::asio::io_context` per thread;
- A small bounded bootstrap executor: secure file copy, spawn, load, and wait for READY;
- A small bounded reaper executor: destroy/wait operations that may wait on a child;
- 1 bounded log output thread; when the queue is full, count by event category and apply an explicit drop policy.

`N` initially is the smaller of `capsid_recommended_worker_count()` and the Host configured ceiling, but the final default must be calibrated by first-party Host A/B testing.

### 4.2 Ownership Rules

Each worker's exclusive ownership transitions as follows:

```text
bootstrap executor
    → post once to the target shard after READY
    → shard exclusively owns all Runtime API calls
    → remove, stop reads/writes, and clear the view
    → reaper executor exclusively owns destroy
```

At any moment exactly one thread calls a given `capsid_worker`. Payload, header, and audit views are copied into Host-owned bounded objects before the next `capsid_worker_next_event()`.

### 4.3 Why Asio/Beast Instead of Raw epoll

Asio uses epoll on Linux and can manage the existing `capsid_worker_fd()` through `posix::stream_descriptor`; it uses kqueue on macOS, and the Windows backend can use IOCP. Beast provides cross-platform incremental HTTP/1 parsing and serialization. Host retains the owner-shard model without reimplementing fd/HANDLE registration, timers, HTTP framing, or partial-packet state machines.

To prevent the current ABI from locking Host into POSIX, only the `WorkerEventSource` platform adapter is allowed to call `capsid_worker_fd()` directly. Pool, routing, request, credit, and lifecycle code observes only "readable/writable/closed" semantics and contains no `_WIN32` or POSIX branches. On Windows, Runtime internally splits process creation, worker transport, and sandbox into platform backends; any new waitable/event-source C ABI must extend ABI v7 additively, and its concrete types, ownership, and wakeup semantics are first frozen by Windows RED tests.

Beast is not a full web server: Host still implements routing, header policy, body credit, timeouts, and error mapping. That is precisely Capsid product logic rather than a reimplementation of a general protocol parser.

The Runtime target remains C++11; only `capsid-host` and `capsid_host_core` set `CXX_STANDARD 20`. Host upgrades must not break the existing ABI or compatibility tests.

## 5. Configuration Scheme

### 5.1 Revised `host.json` Outline

The fields below are structural suggestions; ordinary capacity values still need profile calibration. `recovery` and streaming values are v1 candidates and must be validated with fake-clock, crash-loop, SSE soak, and fault-injection tests before being frozen:

```json
{
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/capsid/apps",
  "stateRoot": "/var/lib/capsid",
  "secretRootTemplate": "/run/capsid/secrets/{application}",
  "admin": {
    "unix": "/run/capsid/admin.sock",
    "mode": "0600"
  },
  "listeners": [
    {
      "name": "public",
      "tcp": "127.0.0.1:8080",
      "publicScheme": "https",
      "routing": {
        "mode": "subdomain",
        "suffix": ".apps.example.com"
      },
      "limits": {
        "connections": 4096,
        "headerBytes": "64KiB",
        "headerTimeout": "5s",
        "bodyIdleTimeout": "30s",
        "streamIdleTimeout": "60s"
      }
    }
  ],
  "permissions": {
    "modules": ["capsid:permissions", "capsid:stdio"],
    "environmentNames": [],
    "fsReadRoots": [],
    "fetchTargets": [],
    "storageNamespaces": [],
    "stdioStreams": ["stdout", "stderr"]
  },
  "isolation": {
    "mode": "strict",
    "required": [
      "no_new_privs",
      "landlock",
      "seccomp",
      "user_namespace",
      "mount_namespace"
    ],
    "cgroupRoot": "/sys/fs/cgroup/capsid-host"
  },
  "trustedBytecodeKeys": {
    "release-2026": "/etc/capsid/bytecode-keys/release-2026.pub"
  },
  "defaults": {
    "worker": {},
    "request": {
      "maxStreamingInflightPerWorker": 2
    },
    "pool": {}
  },
  "maximums": {
    "worker": {},
    "request": {
      "maxStreamingInflightPerWorker": 2
    },
    "pool": {}
  },
  "capacity": {
    "workersTotal": 128,
    "startupsConcurrent": 4,
    "queuedRequestsTotal": 4096,
    "queuedHeaderBytesTotal": "64MiB",
    "workerMemoryCommitTotal": "24GiB"
  },
  "recovery": {
    "crashBudget": {
      "maxEvents": 5,
      "window": "60s"
    },
    "restartBackoff": {
      "initial": "250ms",
      "maximum": "30s",
      "jitter": "20%"
    },
    "replacementsConcurrentPerApp": 1,
    "activeHealthInterval": "30s",
    "activeHealthFailures": 2
  }
}
```

Compared with the existing sample, the main changes are:

- admin and listeners are explicitly defined;
- per-worker, per-request, pool, and host capacity are separated;
- each listener owns its connection, header, and timeout limits;
- Host-level global budgets cannot be overridden by an App.

### 5.2 App Configuration

The App side is also advised to keep boundaries clear:

```json
{
  "apiVersion": "capsid/app-v1",
  "entry": "bundle.mjs",
  "permissions": {},
  "worker": {
    "jsHeap": "64MiB",
    "processAddressSpace": "256MiB",
    "memoryMax": "256MiB",
    "fileDescriptors": 64,
    "pidsMax": 8
  },
  "request": {
    "timeout": "3s",
    "maxInflightPerWorker": 8,
    "maxStreamingInflightPerWorker": 2
  },
  "pool": {
    "minReady": 4,
    "maxWorkers": 4,
    "queueRequests": 128,
    "queueHeaderBytes": "2MiB",
    "queueTimeout": "250ms"
  },
  "healthCheck": {
    "path": "/_capsid/health",
    "timeout": "1s"
  }
}
```

The static-pool phase requires `minReady == maxWorkers`. Only after bounded autoscaling is implemented and passes stress tests may the two differ, so v1 config does not expose semantics that do not exist yet.

### 5.3 Parsing and Validation

The fixed configuration processing order:

1. Limit file size and JSON nesting;
2. Use standard JSON mode; reject comments, trailing commas, and NaN/Infinity;
3. Reject any duplicate key;
4. Reject unknown fields;
5. Check types, units, ranges, and cross-field relationships;
6. Expand `{application}` templates;
7. Normalize paths, hosts, CIDRs, ports, and resource units;
8. Compile the effective policy and generate a stable digest;
9. Pass only normalized results to later stages.

v1 uses the same parsing resource limits for `host.json` and `capsid.json`: raw input is at most 1 MiB (inclusive) and must be checked before creating the JSON DOM; JSON value nesting is at most 64 levels, with the root value counted as level 1. Exceeding either limit returns the stable `kResourceLimit` and root JSON Pointer `""`, without proceeding to unknown-field or value validation. Nesting is enforced by Jansson's own parser depth limit; do not add a simpler bracket-counting JSON scanner in front of it. `{`, `[`, and escaped content inside strings must not affect depth determination.

M0.1 completes recursive type checking for the Host/App structures shown in 5.1/5.2, permission containers, arrays, and dynamic key maps in one pass. Unit syntax and normalization, Host/App ceiling intersection, listener routing conditions, `value`/`valueFrom` exclusivity for secrets, and attestation semantics are handled by their respective later contracts; they are no longer split into per-field M0.1 sub-milestones.

The single critical reason for choosing Jansson is that it can reject duplicate keys directly through a public flag. Boost.JSON's DOM parser keeps the last value for duplicate keys and is unsuitable as the sole security-sensitive config parser. Configuration is not on the hot path, so parse performance is not a selection criterion.

## 6. Version Snapshot and Persistent State

### 6.1 Why `active.json` Is Enough

`active.json` is not user configuration; it is Host's internal single-writer record of App serving state. An active form looks like:

```json
{
  "schema": "capsid-active-v1",
  "app": "orders",
  "state": "active",
  "version": "2026-07-31-002",
  "generation": "sha256:8f3a9c..."
}
```

Explicit decommissioning uses a retired tombstone instead of deleting the file:

```json
{
  "schema": "capsid-active-v1",
  "app": "orders",
  "state": "retired",
  "previousVersion": "2026-07-31-002",
  "previousGeneration": "sha256:8f3a9c..."
}
```

When the crash budget is exceeded, save `state: "quarantined"` and keep the corresponding Version/generation and stable reason code. All three states share the same atomic replacement protocol; no second state file is added.

```json
{
  "schema": "capsid-active-v1",
  "app": "orders",
  "state": "quarantined",
  "version": "2026-07-31-002",
  "generation": "sha256:8f3a9c...",
  "reason": "CRASH_BUDGET_EXCEEDED"
}
```

M0.4 freezes this internal file as a strict JSON object of at most 16 KiB: duplicate/unknown fields, NUL, wrong types, and trailing input are rejected; `schema` must be exactly `capsid-active-v1`; `app` must exactly match the App being recovered; App/Version IDs follow the ASCII rules in 5.2; generation must be `sha256:` followed by 64 lowercase hex digits. active allows and requires only `version/generation`; retired allows and requires only `previousVersion/previousGeneration`; quarantined allows and requires only `version/generation/reason`, with the v1 reason fixed to `CRASH_BUDGET_EXCEEDED`. The normalized output is single-line JSON without newlines, with field order fixed as `schema/app/state` followed by state-specific fields.

The v1 constraints are:

- one Host process;
- one control-plane writer;
- at most one deploy/retire state-change operation per App at a time;
- the request hot path only reads the in-memory Registry and never reads state files;
- no cross-App atomic transactions or complex history queries.

Under these conditions, a database would not improve online request correctness; it would only add dependencies, schema migration, backup, and corruption-recovery surface area. Plain files are sufficient.

### 6.2 State Directory

The recommended layout is:

```text
/var/lib/capsid/
├── apps/
│   └── orders/
│       ├── active.json
│       ├── versions/
│       │   ├── 2026-07-31-001.json
│       │   └── 2026-07-31-002.json
│       └── generations/
│           ├── 8f3a9c.../
│           │   ├── COMPLETE
│           │   ├── capsid.json
│           │   ├── effective.json
│           │   ├── bundle.mjs
│           │   ├── bundle.qjsb
│           │   ├── bytecode.json
│           │   └── bytecode.sig
│           └── 729abe.../
│               └── ...
└── staging/
    └── <operation-id>/
```

`versions/<version>.json` records the immutable mapping from the external Version ID to a generation digest. When the same App/Version is deployed again:

- If the content and effective-config digest are the same: the Version mapping is idempotent and reuses the existing generation. Whether a deploy can short-circuit still depends on the serving state per 7.3; only an already-active state returns directly, while retired/quarantined must prewarm again;
- If the digest differs: return `VERSION_IMMUTABILITY_CONFLICT`; the old mapping must not be overwritten.

### 6.3 Generation Identity

Bundle bytes alone cannot be the generation identity. The same source must produce a different worker pool when permissions, resources, Host configuration, or the secret revision changes.

```text
generationDigest = SHA-256(binaryRecord)
```

`binaryRecord` always starts with `"capsid-generation-v1\0"` (including the trailing NUL), followed by ten fields encoded in the order above; each field is a 32-bit big-endian byte length plus raw UTF-8 bytes. `selectedArtifactKind` allows only the stable ASCII values `source` or `trusted-bytecode`; when there is no attestation, the third field is encoded as an empty string. The final public form is `sha256:` followed by 64 lowercase hex digits. Length prefixes remove concatenation ambiguity; any change to a field, including secret revision and the actually selected artifact, produces a different generation.

Even when this deployment falls back to source because of a compatibility mismatch, record the verified attestation, fallback reason, and actual selected artifact. A restart must not silently switch from source to another bytecode, or back, without changing the generation identity.

The secret revision does not store plaintext or a bare digest of the secret value. Secret providers should return an opaque revision that contains no secret; the simplest file backend can compose a revision from the opened file's device, inode, size, and `ctime`, and re-check them before and after reading. If rewriting identical content creates a new generation, that is acceptable and simpler than introducing a separate Host key-management system.

The aggregate revision for multiple secrets is fixed as `sha256:` followed by the SHA-256 of this binary record: start with `"capsid-secret-revision-v1\0"` (including the trailing NUL), encode the App ID first, then sort by environment name, and for each secret env encode env name, key ID, and provider opaque revision in order; each item again uses a 32-bit big-endian length prefix. Literal envs do not enter the secret revision because their values are already covered by the normalized App config digest. Request JSON ordering must not change the aggregate revision.

### 6.4 Secure Copy

When reading a version from `applicationsRoot`:

1. First validate the App/Version ID character set and length;
2. From the pre-opened root dirfd, use `openat2` with `RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS`;
3. Accept only regular files; reject symlinks, devices, FIFOs, sockets, and out-of-root paths;
4. Set hard limits on config, source, and total version size;
5. Read from the already-open fd and compute SHA-256;
6. Compare `fstat` identity, size, and mtime/ctime before and after copying; any change fails;
7. Write into `stateRoot/staging/<operation-id>`, calling `fdatasync` per file;
8. Write the final `COMPLETE` marker and sync the directory;
9. Atomically rename into the generation directory and `fsync` the parent directory.

The request-processing phase only accesses in-memory bundle/metadata or Host-internal generations; it never re-reads the upload directory.

### 6.5 Atomic `active.json` State Switchover

The switchover must happen on the same filesystem and in the same directory:

1. Acquire the per-App deploy mutex;
2. Confirm the generation has `COMPLETE` and all workers in the new pool are READY;
3. Create `active.json.tmp.<operation-id>` with `O_CREAT|O_EXCL`;
4. Write the normalized, complete JSON;
5. `fdatasync` the temporary file;
6. `renameat` over `active.json`;
7. `fsync` the App state directory;
8. Publish the new in-memory Registry snapshot;
9. Return deployment success and begin draining the old pool.

Retire and quarantine reuse steps 3–8: first block new routing in memory, then write the corresponding tombstone/state. They do not require a new generation to be READY, but they must still `fdatasync`, atomically rename, and `fsync` the parent directory. Their write-failure semantics differ: if retire fails before rename, restore the original Registry and let the old version continue serving; if quarantine fails to write, stay fail closed in memory, stop replacement, keep retrying the write, and emit a non-discardable alert. Never resume crash-loop traffic just to report the write failure. Do not proactively restart Host until storage recovers; if the process still disappears due to node failure, the old active state may trigger replacement again on the next start. Therefore an unwritable disk is a durability incident requiring operator intervention, not a normal path that can silently degrade.

The order must be "publish the in-memory pointer only after the durable pointer succeeds." If the process crashes between the two, restart recovers from the new `active.json`; if it crashes before rename, the old version remains active. Temporary files are cleaned during startup recovery.

A successful rename followed by a failed parent-directory `fsync` must not masquerade as an ordinary "not committed" error. In that case Host does not publish the new state in memory, the operation enters `DURABILITY_UNCERTAIN`, and subsequent state writes for the same App are blocked until the control plane reconciles or the process exits as a durability incident. If the node crashes before reconciliation, the filesystem may restore either the old or the new complete atomic directory entry; both are allowed, but recovery must still verify that the generation referenced by active has `COMPLETE`. Temporary files or half generations must never be activated. Commit success may be returned only after the parent directory sync completes.

Startup recovery trusts only a complete `active.json`: `active` must reference an existing, complete generation; `retired` does not restore a pool; `quarantined` does not automatically restart workers. An invalid state fails the App closed and reports a clear error; Host must not scan directories and arbitrarily choose "the latest version." A missing `active.json` means the App has no serving state and generations are not scanned; retired/quarantined recovery also does not require the old generation to be retained. Cleaning `active.json.tmp.*` at startup is best effort: failure produces an ops alert but does not change the state derived from a valid `active.json`.

### 6.6 Secret Snapshot Read

`value` and `valueFrom` are mutually exclusive. The former is an ordinary config value; the latter accepts only one restricted secret key ID, which cannot contain `/`, `..`, or an empty component. The file backend's fixed flow is:

1. At Host startup, securely open the static root of `secretRootTemplate`, verifying directory type, owner, and mode;
2. Open the App subdirectory using the already-validated App ID, then open the key with `openat2` and `RESOLVE_BENEATH`, `RESOLVE_NO_SYMLINKS`, and `RESOLVE_NO_MAGICLINKS`;
3. Use `O_RDONLY|O_CLOEXEC|O_NONBLOCK`; accept only regular files and reject group/world writable files, symlinks, FIFOs, devices, and sockets;
4. Read at most 16 KiB + 1 byte; exceeding that fails. Reject NUL and invalid UTF-8; do not auto-trim newlines. Binary secrets must be encoded as text by the publisher first;
5. Compare the fd's device, inode, size, and mtime/ctime before and after reading; if changed, retry once, and if still changed, deployment fails;
6. Only keys that pass the Host environment allowlist, the App env request, and the `capsid:env` module gate produce a `capsid_env_entry`; total entry count and total bytes are re-checked against Runtime limits;
7. After Runtime deep-copies the descriptor, Host immediately clears that bootstrap task's temporary value buffer; persistent metadata records only the opaque revision, never the value or its bare digest.

The pure compilation phase outputs an owning snapshot sorted by env name and a temporary `capsid_env_entry[]` view. The environment fragment of `effective.json` is fixed as single-line canonical JSON recording only name/source; secret entries additionally record key ID and opaque revision. For example:

```json
{"environment":[{"name":"API_TOKEN","source":"secret","keyId":"orders-api-token","revision":"file-v1:11:22:41:1700000000"},{"name":"APP_MODE","source":"literal"}]}
```

Neither literal nor secret values enter that JSON, revisions, error paths/messages, admin responses, logs, or metrics. A compilation failure must atomically return an empty snapshot; it cannot keep partially processed entries.

File mode is not a complete secret-management system; mounts, backups, node swap/core dumps, and the secret root lifecycle remain the deployment environment's responsibility. Host's responsibility is minimal reading, minimal forwarding, and not leaking plaintext into the control plane.

## 7. Deployment, Retire API, and State Machine

### 7.1 Admin Surface

The default admin entry point is a dedicated Unix socket:

```text
/run/capsid/admin.sock
```

Requirements:

- mode defaults to `0600`, with an optional fixed admin group;
- validate the local UID/GID with `SO_PEERCRED`;
- do not share the socket with a public data listener;
- request body, headers, and processing time have small fixed limits;
- never write secrets, bundle bytes, or the full environment into responses or logs.

The Admin socket is a deliberate global trust boundary: a UID or admin group approved through `SO_PEERCRED` can deploy, roll back, and retire all Apps on this Host. v1 does not claim per-App administrative authorization; mutually untrusted operator parties must use separate Host instances and admin sockets.

The only operation that can activate a Version remains:

```http
POST /v1/deploy
Content-Type: application/json

{"app":"orders","version":"2026-07-31-002"}
```

Deployment usually exceeds ordinary HTTP handler latency, so return `202 Accepted` with an operation ID:

```json
{
  "operation": "01J...",
  "app": "orders",
  "version": "2026-07-31-002",
  "status": "warming"
}
```

Explicit decommissioning uses `POST /v1/apps/{app}/retire`, but it cannot choose another Version. Read-only `GET /v1/operations/{id}`, `GET /v1/apps/{app}`, and health/metrics endpoints cannot change versions.

### 7.2 State Machine

```text
RECEIVED
  → VALIDATING
  → STAGING
  → COMPILING_POLICY
  → WARMING
  → HEALTH_CHECKING
  → ACTIVATING
  → ACTIVE

Any pre-transition state → FAILED (old version unchanged)
Old active pool → DRAINING → RETIRED

ACTIVE → RETIRING → RETIRED
ACTIVE → DEGRADED → QUARANTINED
QUARANTINED → RETIRING → RETIRED
QUARANTINED/RETIRED → explicit deploy → WARMING → ACTIVE
```

Operation records may be in-memory objects plus a bounded JSONL ops log; no database is needed to query them. After a Host restart, only the active generation must be recovered accurately; an interrupted deploy can report `ABORTED_BY_RESTART` and the caller can retry idempotently.

M0.6 clearly separates durable state from in-memory phase. `active.json` still has only the M0.4-frozen `active|retired|quarantined` states; `RETIRING`, `QUARANTINING`, or `DURABILITY_UNCERTAIN` are not new on-disk schemas. The in-memory phases are frozen as:

```text
ABSENT | ACTIVE | RETIRING | RETIRED | QUARANTINING | QUARANTINED
       | DURABILITY_UNCERTAIN | FAILED_CLOSED
```

Route derivation must fail closed: only structurally complete `ACTIVE` may serve traffic and allow automatic replacement; `ABSENT`, `RETIRED`, and `RETIRING` (which has already committed the retired tombstone but is still draining) return 404; every other phase returns 503. After retire begins and before the tombstone is committed, return 503 and stop accepting new traffic; after the commit succeeds, even while the old pool is still draining, return 404. Unknown phases, phase/document type mismatches, or recovery action/document mismatches enter `FAILED_CLOSED`; a default `else` branch must never make the app active.

When M0.4 persist results enter the in-memory state machine, they are interpreted by the fixed table below:

| Operation | Persist result | In-memory result | Routing/follow-up action |
| --- | --- | --- | --- |
| retire | Failed before rename | Restore source `ACTIVE` or `QUARANTINED` | Active source resumes serving; quarantined remains 503; operation fails |
| retire | Rename + directory sync succeeded | Keep `RETIRING`, document becomes retired | Immediately 404 and begin drain; `RETIRED` after drain completes |
| quarantine | Failed before rename | Keep `QUARANTINING` | 503, forbid replacement, and keep retrying the same quarantined document write |
| quarantine | Rename + directory sync succeeded | `QUARANTINED` | 503, forbid replacement, and begin bounded drain |
| Any operation | Rename succeeded but durability uncertain | `DURABILITY_UNCERTAIN` | 503, block subsequent state writes, await reconciliation/process-level handling |
| Any operation | Persist result internally contradictory | `DURABILITY_UNCERTAIN` | Fail closed as an implementation/adapter defect; do not guess commit state |

Restart recovery allows only M0.4's `kActivate` to start a pool, and the active route may be published only after the recovered pool is READY/healthy; `kNone`, `kKeepRetired`, `kKeepQuarantined`, and any recovery error spawn no workers. This keeps retired/quarantined safety independent of volatile operation state.

### 7.3 Concurrency and Idempotency

- deploy/retire for the same App are serialized;
- different Apps may run concurrently, but share the global startup/memory permit;
- same App/Version/generation already active: return active directly;
- same generation already quarantined: an explicit deploy goes through prewarming again and resets the instability budget only after success; it does not short-circuit as active;
- App already retired: an explicit deploy creates/recovers the pool normally and atomically writes active back;
- same Version already mapped to a different generation: `409`;
- identical in-flight warming requests join the same singleflight;
- new requests never spawn a separate pool per call.

The pure decision table for explicit deploy is also frozen in M0.6: only the same Version/generation that is already active may return already-active directly; the same Version pointing at a different generation is always an immutability conflict; absent, retired, and quarantined (including an explicit redeploy of the exact same quarantined generation) must go through `WARMING → active.json commit → ACTIVE`. The quarantine budget can be reset only after the new active pointer has been committed and published; it cannot be cleared early on receiving a deploy or when warming starts. `RETIRING` and `QUARANTINING` return busy; `DURABILITY_UNCERTAIN` blocks deploy.

### 7.4 Health Checks

If a health check is configured:

- Run it at least once against every new worker, not just any worker in the pool;
- Allow only `GET`, no body, and a small fixed response-body limit;
- Expect `200..299`; fully consume or discard the body and correctly return credit;
- Use an independent timeout and request ID;
- Do not switch if any `minReady` worker fails;
- The health check executes real application code; documentation must warn against business side effects.

After activation, reuse the same probe for low-cost active health awareness:

- Round-robin across the pool with jitter so each worker with a free inflight slot is checked about every 30 seconds; at most one health check is concurrent per App;
- Workers that are busy or have full streaming are skipped this round; health checks must not preempt business slots, and the skip is recorded. A skip is neither a health success nor a reset of the consecutive-failure counter; that worker's inflight remains covered by passive signals such as request deadline, stream idle timeout, synchronous CPU timeout, IPC/protocol failure, and process EXIT, and any passive failure removes it immediately per this section;
- After 2 consecutive timeouts, non-`2xx` responses, protocol errors, or abnormal exits, the worker enters `UNHEALTHY`, is removed from the scheduler first, then handed to the reaper;
- Synchronous CPU timeout, IPC/protocol failure, and unexpected EXIT remove the worker immediately without waiting for a second probe;
- Both health removals and crashes enter the same generation instability budget, preventing "probe failure → unbounded recycle" from bypassing the crash budget;
- Apps without a configured `healthCheck` rely only on passive health signals such as Runtime/IPC/timeout and do not fake business probes.

### 7.5 Drain

After activating a new generation:

1. Registry stops sending new requests to the old pool;
2. The old pool continues processing inflight requests;
3. When inflight reaches zero, issue `shutdown` and continue flush/read until EXIT;
4. When the drain deadline expires, cancel all requests;
5. After a short cancel grace period, terminate;
6. Hand the handle to the reaper executor for destroy;
7. Record total drain time and the number of forced cancellations.

### 7.6 Explicit Retire

```http
POST /v1/apps/orders/retire
```

This operation has no policy body, returns `202 Accepted` with an operation ID, and repeated retires are idempotent success. The fixed order is:

1. Acquire the per-App operation mutex, set the in-memory state to `RETIRING`, and immediately stop accepting new requests;
2. Atomically replace `active.json` with the `state: "retired"` tombstone using the 6.5 protocol; if it fails before rename, restore the old Registry and let the old version continue serving;
3. Publish a Registry snapshot with no active pool; ordinary data requests uniformly return `404`, while the admin API still shows retired and the previous generation;
4. Drain all pools per 7.5; the operation becomes `RETIRED` after all workers exit;
5. Generation and Version mappings are retained according to retention/GC rules; retire itself does not delete rollback artifacts;
6. A later explicit deploy of any existing or new Version can reactivate the App.

A Host crash before the tombstone rename recovers the old active state; after rename it recovers retired. Host never infers retirement from whether an upload directory exists, and never deletes `active.json` and then arbitrarily selects the latest Version.

## 8. Routing and HTTP Boundary

### 8.1 One Listener, One Routing Mode

Supported modes:

- `subdomain`: exact DNS label suffix;
- `path`: fixed `/@capsid/{app}/` prefix;
- `header`: trusted internal listeners only.

Rules:

- App IDs use only the ASCII rules in section 5.2;
- The `Host` header is matched on label boundaries after removing a legal port; bare string suffix matching is not allowed;
- path mode identifies the App on the original path segments; encoded slash, backslash, dot segments, and invalid percent encoding are rejected;
- request parameters only query the Registry; they are never concatenated into disk paths;
- Version, generation, or worker cannot be selected through URL/header;
- control headers are removed before constructing FetchRPC headers.

v1's `header` mode is fixed to a single `Capsid-App` header. It may be enabled only on internal listeners already proven trusted by a Unix socket, mTLS, or source allowlist; "configured as header mode" is not itself proof of trust. A missing, duplicate, or App-ID-invalid `Capsid-App` fails closed. Other routing modes only delete this header if received; they never use it.

### 8.2 Worker-Observable URL and Proxy Headers

`Request.url` is the public App contract; v1 fixes it to be constructed by Host and never guessed from forwarded headers:

```text
request.url = publicScheme + "://" + validatedAuthority + rewrittenTarget
```

- Every TCP data listener must explicitly configure `publicScheme: "http"|"https"`; this denotes the external scheme users see and does not require Host to terminate TLS itself;
- For subdomain routing, authority is the request `Host` after suffix-rule validation; path/header routing must configure a fixed `publicAuthority` so an arbitrary Host cannot change the app-visible origin;
- Only HTTP origin-form request-targets are accepted; absolute-form, authority-form, and `*` are rejected on v1 data listeners;
- `Host` participates only in authority validation and URL construction; it is not passed to the worker as an ordinary Fetch header;
- Query bytes are preserved verbatim, not decoded and re-encoded; invalid percent encoding is rejected before routing.

The v1 authority grammar is deliberately narrow and auditable: accept only ASCII DNS/IPv4-style hosts with an optional non-empty decimal port `1..65535`; lowercase the host and render the port as decimal without leading zeros. DNS labels are at most 63 bytes and the full host at most 253 bytes. Userinfo, empty labels, trailing dots, leading/trailing label hyphens, bracketed IPv6 literals, and overlong authorities are forbidden. The subdomain `suffix` must start with `.`, must not include a port, and the request `Host` must be exactly "one App DNS label + suffix". The suffix alone, extra leading labels, and bare string suffix matches are all rejected. App IDs that are legal but not DNS labels (for example, containing `_`) use path/header mode. path/header mode still requires exactly one syntactically valid `Host`, but the worker origin uses only the normalized fixed `publicAuthority`; the request `Host` cannot change it. If v1 needs an IPv6 public origin, an external proxy should expose a DNS authority first; do not temporarily relax the grammar in the implementation.

The v1 request-target grammar is also fixed: ASCII origin-form starting with `/`, at most 16 KiB, with raw control, space, backslash, fragment, and non-ASCII bytes forbidden; every `%` must be followed by two hex digits. The path part rejects percent-encoded `/` and `\\`, and also rejects segments that decode to exactly `.` or `..`; legal escapes such as `%2F` in the query may be preserved. path mode extracts the App only from the raw `/@capsid/{app}` segment and does not decode the App; `/@capsid/{app}` and trailing `/` are rewritten to `/`, with original query bytes reattached. The normalizer does not perform Unicode, dot-segment, or percent canonicalization, so distinct inputs cannot silently collapse into the same routing key.

Routing rewrite table:

| Mode | Client target | worker `Request.url` path/query |
| --- | --- | --- |
| subdomain | `/api/orders?x=1` | `/api/orders?x=1` |
| path | `/@capsid/orders` | `/` |
| path | `/@capsid/orders/api?x=1` | `/api?x=1` |
| header | `/api/orders?x=1` | `/api/orders?x=1` |

v1 applies one fail-closed proxy-header rule to all data listeners: after entering Host, always delete `Forwarded`, all `X-Forwarded-*`, and `X-Real-IP`; they are neither used for routing/URL nor passed to the worker. Public clients therefore cannot forge scheme, authority, or client IP; v1 also does not expose the real client IP to Apps. An external TLS proxy must set a legal `Host`, and Host's `publicScheme` is configured as `https`. If a real need appears later, design a separate trusted-forwarding contract with peer CIDR/mTLS proof; v1 does not imply that trust.

Before entering the worker, Host also builds an atomic owning snapshot: header names are validated as ASCII tokens and lowercased; values accept only HTAB and visible ASCII; input order and original value bytes are preserved. At most 128 fields, with raw name/value bytes totaling at most 64 KiB. In addition to `Host`, `Capsid-App`, and the proxy headers above, standard hop-by-hop fields are removed, as are all fields named by comma-separated, ASCII case-insensitive tokens in any `Connection` header; empty or invalid `Connection` tokens are rejected outright. `X-Forwardedness` is not `X-Forwarded-*` and should not be deleted by mistake. Successful results do not reference Beast buffers; failed results do not publish partial App, URL, or header data.

### 8.3 HTTP/1 Security Rules

Host must handle the following uniformly before entering the app:

- Reject conflicting or duplicate `Content-Length`;
- Reject `Transfer-Encoding` conflicting with `Content-Length`;
- Accept only HTTP/1 framing explicitly recognized by Beast;
- Delete connection-nominated headers and all hop-by-hop headers;
- Delete all proxy forwarding headers per 8.2;
- Forbid passing `Connection`, `Keep-Alive`, `Proxy-Connection`, `TE`, `Trailer`, `Transfer-Encoding`, and `Upgrade` through to the worker unchanged;
- Validate header name/value, total bytes, and field count;
- v1 forbids WebSocket upgrade and CONNECT;
- v1 allows only one application request in flight per connection; HTTP pipelining concurrency is not implemented yet;
- header, body-idle, queue, Host request, and response-idle timeouts are timed separately.

The pure normalizer from 8.2 receives the target/header view already parsed by Beast. It is responsible only for semantic validation, routing, URL construction, and scrubbing; it must not reinterpret `Content-Length`/`Transfer-Encoding` or decide message framing. Framing conflicts, duplicate `Content-Length`, and chunked validity are always decided authoritatively by the same Beast parser. `Transfer-Encoding` is still removed from the worker header snapshot as a hop-by-hop field; that is not a second framing decision.

Host cannot assume Runtime's response header decoder has applied HTTP semantic filtering; the current decoder mainly validates the FetchRPC binary structure. Responses must therefore also pass hop-by-hop, length, and illegal-value checks.

### 8.4 `Expect: 100-continue`

Send `100 Continue` to the client only after routing, admission, worker assignment, and a successful begin request. Rejected requests do not read the full body.

Clients without `Expect` may send the body early; body bytes that Beast read past the header boundary into a buffer must count toward queued bytes, and the buffer itself has a hard limit.

## 9. Request/Response Credit Mapping

### 9.1 Request Direction

```text
client readable
  → parse header
  → route + admission + choose local worker
  → capsid_worker_begin_request
  → wait for REQUEST_CREDIT
  → read at most the remaining credit body each time
  → capsid_worker_write_request
  → capsid_worker_end_request after body completes
```

Do not continue application-level socket reads without request credit. If Runtime's global write queue makes `write_request` return `WOULD_BLOCK`, keep the current bounded chunk, listen for worker fd writable, and continue after flushing.

### 9.2 Response Direction

```text
CAPSID_EVENT_RESPONSE_BODY
  → copy payload before next_event
  → async_write to client
  → write completion succeeds
  → grant_response_credit(actual bytes written)
```

Credit must not be returned early when the socket write is submitted. If the client is slow, disconnects, or hits a write timeout, cancel the request immediately; late events continue to be drained per the existing ABI but are not forwarded.

Each request's Host buffering limit should be no larger than the Runtime response window granted, and should count against both the App and Host unacknowledged byte budgets.

### 9.3 SSE and Streaming

- After receiving the response head, check `Content-Type` first; `text/event-stream` must acquire that worker's streaming permit before emitting the head to the client;
- v1 defaults to `maxStreamingInflightPerWorker=2`; Apps may request only lower values. The value must be less than `maxInflightPerWorker` to keep at least one ordinary request slot; only when `maxInflightPerWorker == 1` may both be 1, explicitly giving up concurrency reservation;
- If the permit is full and the head has not yet been sent to the client, Host cancels the worker request and synthesizes `503`; it must not send `200 text/event-stream` first and then disconnect;
- Write body frames incrementally; do not buffer until response end;
- `text/event-stream` forbids whole-response compression and proxy buffering;
- Return credit only after the downstream write succeeds;
- The default stream idle timeout is 60 seconds; apps must keep the stream alive with comments/heartbeats, and timeout cancels;
- v1 imposes no mandatory maximum duration: the streaming permit already forms a deterministic capacity boundary, and deployers may set `maxStreamDuration` separately;
- Client disconnect cancels immediately.

The streaming permit counts against both App and Host connection/inflight budgets and must be returned exactly once when the worker exits, cancels, or the response ends. Ordinary chunked/large responses still obey credit, idle timeout, and ordinary inflight constraints; they do not automatically consume an SSE permit just because there is no `Content-Length`.

### 9.4 Request ID

Each shard uses monotonically increasing 64-bit non-zero IDs and tracks the outstanding set per worker. On wrap, only IDs not in that worker's active set may be reused; in practice, rotate workers when approaching the ceiling to avoid complex reuse logic.

## 10. Worker Pool and Scheduling

### 10.1 Shard-Local Pool

Connections and workers are pinned to a shard. When a new pool is created, workers are allocated per shard, and scheduling prefers only local shard workers so request and response bodies are not moved across threads.

When a shard temporarily has no worker capacity:

- Requests enter that App's bounded queue on that shard;
- A worker is not temporarily transferred to another shard;
- A future cross-shard request handoff, if needed, must be justified by profiling and designed separately.

A listener may use `SO_REUSEPORT` so each shard accepts its own connections; where unsupported, the acceptor performs a one-time connection handoff.

### 10.2 Choosing a Worker

The static pool in v1 uses simple Power of Two Choices: pick two random READY workers on the local shard and compare:

```text
inflight
+ response bytes awaiting client
+ unhealthy penalty
```

When the pool is small, taking the minimum is also fine. Do not use round-robin alone, because a single SSE or slow client can keep a worker asymmetrically loaded for a long time.

### 10.3 Admission Control

Fixed order:

1. listener connection/header gate;
2. Host global inflight/queue gate;
3. App inflight/queue gate;
4. local shard pool capacity;
5. worker `max_inflight_requests` hard boundary.

Error mapping:

| Scenario | HTTP |
| --- | --- |
| App does not exist | 404 |
| App is retired | 404 |
| active generation is quarantined | 503 |
| App's own queue/quota is full | 429 |
| Host globally overloaded, pool has no READY worker | 503 |
| Queue or Host deadline expires | 504 |
| App normally returns 5xx | Pass through the app response unchanged |
| worker/IPC fails before response head | 503 or limited retry |

### 10.4 Automatic Retry

v1 allows exactly one retry and only when all of the following hold:

- no response head has been sent to the client;
- the failure comes from worker crash/IPC/protocol, not an application HTTP 5xx;
- the method is GET or HEAD;
- the request has no body;
- the Host deadline still has sufficient budget;
- the new worker belongs to the same active generation.

PUT/DELETE have protocol-level idempotency connotations, but the already-streamed body was not saved by Host, so it cannot be replayed automatically. POST is not automatically retried even with an Idempotency-Key, unless a clear App opt-in contract is added later.

### 10.5 Worker Crash Replacement and Generation Quarantine

When an active worker has an unexpected EXIT, cgroup OOM, synchronous CPU timeout, IPC/protocol failure, or is removed by the 7.4 continuous health check, the fixed procedure is:

1. The owner shard immediately removes the worker from the scheduling set and hands the handle to the reaper executor;
2. Count the event into the generation's rolling instability budget before deciding any retry or replacement: at most 5 events in 60 seconds. Unexpected exits, recycles caused by consecutive active-health failures, and replacement spawn/load/READY failures all count; normal drain, Host shutdown, and operator retire do not count;
3. If this count exceeds the budget, jump directly to the `QUARANTINING` flow below and do not perform further retry or replacement; failed inflight requests may be retried per 10.4 only if the generation is still active;
4. When the pool is below the target READY count, create a replacement singleflight; each App has at most one replacement spawn at a time, and it must re-acquire the global startup/memory permit;
5. Active replacement uses exponential backoff: start at 250 ms, double each time, maximum 30 seconds, plus ±20% jitter; reset the App/generation backoff after a replacement worker stays READY and stable for 60 seconds;
6. The global startup permit uses a per-App fair queue, with replacement and deploy counted in separate lanes; a crash-looping App cannot keep jumping ahead of other Apps' deploys.

The pure controller in M0.7–M0.9 further freezes these boundaries so implementation does not re-decide semantics:

- fake clock uses monotonic milliseconds; clock rollback fails closed. The rolling window is `(now - window, now]`; events exactly window age have expired. `maxEvents: 5` means the 6th counted event still inside the window triggers quarantine;
- a backoff attempt increments only when a new replacement is actually scheduled; it does not increment when a per-App replacement singleflight already exists, no worker needs to be added, or the generation is no longer active. `maximum` constrains the exponential base first, then applies bounded signed jitter, so `30s + 20%` has a final upper bound of 36s;
- a replacement worker staying READY through the stability period resets only the backoff attempt; it does not clear the rolling crash budget;
- the instability controller counts a single result and decides `BEGIN_QUARANTINE` before handing the result to request-retry decisions; the quarantine result type itself forbids retry and replacement, and callers must not need to remember an extra order;
- the startup permit avoids granting the same App consecutively whenever another App is waiting, and keeps FIFO within the selected App; deploy/replacement share the permit but are counted separately, with no implicit lane priority; queued replacement requests for the same App/generation join a singleflight and do not add queue items.

When the budget is exceeded:

- The in-memory Registry first enters `QUARANTINING` and stops new traffic. From the moment that state takes effect, 10.4 automatic retry is explicitly disabled: inflight requests without a response head are all canceled and synthesized as `503`; they cannot be reassigned to remaining READY/draining workers, and no replacement may be started for them. Inflight requests that already emitted a response head may only drain in a bounded way, then cancel/disconnect on timeout;
- The control plane writes `active.json` as `state: "quarantined"` using the 6.5 atomic protocol, including Version, generation, and the stable reason code `CRASH_BUDGET_EXCEEDED`;
- All automatic replacement stops, data requests return `503`, and a non-discardable high-priority event is emitted;
- Host restart recovers only quarantine and does not re-enter a crash loop because counters were lost;
- Operators can explicitly deploy an old Version, or explicitly redeploy the same immutable Version, to clear quarantine and start a fresh budget. That action goes through full prewarming; it is not an implicit resume.

v1 does not default to automatic rollback. The old generation may already be destroyed after drain, and the app may have produced state incompatible with old code; switching back without App opt-in is not generally safe. If activation-guard rollback is added later, it must separately define the old-pool retention window, dual-pool capacity, and external-state compatibility contract.

## 11. Permission Compilation and Isolation

### 11.1 Compilation Outputs

The Policy Compiler generates per generation:

- `allowed_modules`;
- `capsid_permission_rule[]`;
- `capsid_env_entry[]`;
- direct `capsid_egress_policy`;
- capability `net_policy`;
- `capsid_resource_limits`;
- sandbox required feature bits;
- Landlock read-only path rules;
- a stable rule ID to JSON pointer mapping;
- `effective.json` without secret plaintext.

Rule IDs are recommended to be sequential from 1 after sorting normalized rules by `(stage, capability, resource, action)`, which is easier to make collision-free and reproducible than a truncated hash.

### 11.2 File Paths

- Both Host roots and App paths are first normalized as absolute path components;
- `/a/b` is an ancestor of `/a/b/c`, but not a prefix of `/a/bad`;
- `.`, `..`, empty components, NUL, and non-absolute paths are rejected;
- deny takes priority, and App allows must fall entirely inside Host allow roots;
- the Runtime operation rule, Landlock, and the actual `openat2` layer are all generated from the same effective rule;
- if a required root does not exist or cannot be opened safely, deployment fails; it is not silently ignored.

### 11.3 Network

Runtime checks hostname, DNS results, and redirects. Host does not configure or switch network namespaces; workers naturally use the same network environment as Host. v1's responsibilities are:

- Runtime policy enforces exact hostname/IP/CIDR + port;
- Runtime denies loopback, link-local, metadata, and RFC private ranges by default unless Host policy explicitly opens them with exact CIDRs;
- the Host/App schema does not expose network namespace, veth, route, or firewall configuration;
- if operators place the entire Host inside an additional network boundary provided by systemd, a container, or Kubernetes, workers naturally use that boundary with Host; this is an optional deployment measure, not a Capsid prerequisite.

The existing Runtime capability of a pre-opened netns fd remains available to other embedding hosts, but the first-party Host does not use it and does not expose a corresponding control-plane field.

### 11.4 cgroup Hierarchy and Capacity

Recommended hierarchy:

```text
capsid-host/
└── apps/<app>/<generation>/
    └── workers/<worker-id>/
```

- the App/generation parent controls aggregate CPU, memory, and PID;
- worker leaves use the current Runtime `sandbox_cgroup_path` and resource limits;
- Host creates and removes directories; Runtime only writes leaf limits, reads them back, and attaches children;
- the parent controller and `cgroup.subtree_control` are pre-delegated by the deployment environment;
- a Host memory/startup permit is acquired before spawning a worker;
- blue-green prewarming must count both old and new pools; insufficient capacity fails the deployment without affecting the old version.

The v1 memory permit accounts the full per-worker `memoryMax`. This is much more conservative than the measured ~6 MiB READY PSS, but it guarantees that the committed total does not depend on historical averages and cannot suddenly oversell when workloads change. M4 can evaluate two-level admission of "hard-ceiling commitment + measured working-set/PSS soft budget": it must keep cgroup `memoryMax` and the Host hard ceiling, use a high-percentile working set updated by workload/profile, growth headroom, and OOM negative controls. It cannot directly replace hard accounting with one benchmark's average PSS.

### 11.5 External Isolation Boundary

The Host main process runs as a dedicated non-root user and obtains a restricted cgroup subtree through systemd `Delegate=yes` or equivalent container configuration. Host creates App/generation/worker subdirectories only under the delegated and verified root.

The first-party Host **does not implement and does not plan** a privileged supervisor: the repository has no root helper target, supervisor socket, netns creation protocol, or nftables management logic. If the target environment requires independent netns, veth, or firewalling, systemd, the container runtime, Kubernetes CNI, or an ops system must provide it before Host starts. If that boundary cannot be provided, change the deployment shape instead of letting Host temporarily elevate privileges.

## 12. Observability

### 12.1 Metrics

v1 has built-in fixed, low-cardinality metrics using controlled labels such as `app`, `generation`, `listener`, and `result`; request IDs, URLs, free-form Version text, hostnames, or error messages must not become labels.

At minimum:

- worker: starting/ready/busy/unhealthy/draining/crash/replacement;
- recovery: instability budget, backoff, replacement permit, quarantine, and retire;
- request: inflight/queued/rejected/cancel/timeout/retry;
- latency: queue, startup, worker, time-to-head, total;
- stream: request/response credit, unacknowledged bytes, SSE permit, slow-client cancel;
- deploy: validate/stage/spawn/load/health/activate/drain times and results;
- isolation: required/applied features, delegated cgroup failure, external network boundary validation results;
- log/audit queue drop;
- process and child RSS/PSS/cgroup memory/CPU.

The Prometheus text endpoint by default binds only to the admin Unix socket or loopback. Although OpenTelemetry C++ signals are stable, v1 does not need a full SDK/exporter for a local Host; when OTLP is needed, a sidecar can scrape, or an optional adapter can be added later.

### 12.2 Structured Logs

All logs use one JSON object per line with fixed fields:

```text
timestamp, level, event, app, version, generation,
worker_id, request_id, operation_id, stage, result, duration_ms
```

Forbidden to record:

- secret values;
- sensitive headers such as Authorization/Cookie;
- raw request/response bodies;
- unsanitized application errors as structured fields;
- high-cardinality paths into metrics.

Runtime LOG and AUDIT must continue to drain. A slow log sink must not block the reactor: use a bounded queue, where app logs may be dropped and counted; deployment, security, and process-lifecycle events enter a separate high-priority lane. If full compliance auditing is required later, a local durable spool must be designed separately; ordinary stderr must not be presented as exactly-once.

`CRASH_BUDGET_EXCEEDED`, active state entering quarantine/retired, retire drain timeout, and admin authorization failures are non-discardable control-plane events; logs must not contain unsanitized URLs, forwarded headers, or health response bodies.

## 13. Runtime Integration Requirements

### 13.1 Implemented: Structured Build/Compatibility Identity

Trusted bytecode uses read-only build info that includes at least:

```text
Capsid runtime version
ABI version
FetchRPC version
QuickJS commit
txiki overlay key/manifest
compile flags relevant to bytecode
architecture/endianness/pointer width
bytecode format identity
capability manifest hash
```

The library-side `capsid_runtime_build_info()`, compiler identity target, and attestation verifier are already frozen by M0.2. The actual worker HELLO/READY must return the same identity; Host must compare the library, compiler attestation, and worker, and must not trust only the library linked into Host.

v1 build info is an additive ABI v7 interface that does not change existing struct layouts. It always exposes runtime/ABI/FetchRPC versions, QuickJS commit, txiki overlay key/manifest, bytecode-relevant compile flags, target architecture, endianness, pointer width, bytecode format identity, capability manifest hash, and the final compatibility ID. The final ID is `sha256:` followed by lowercase hex; the hash input is a `key=value\n` UTF-8 record with fixed field order documented in the public header comments, including the trailing newline, and does not use JSON canonicalization or locale formatting.

`quickjsCommit` must be the locked gitlink commit of `vendor/txiki.js/deps/quickjs`, not the outer `vendor/txiki.js` commit; the outer vendor, all submodules, patches, and overlay content are covered by `txikiOverlayKey`/`txikiOverlayManifest`. When configuring a worker build, compare the locked QuickJS gitlink against the actual checkout to avoid mismatches between field names and real inputs.

The actual worker's READY payload must carry the same ASCII compatibility ID; the official `capsid-bytecode-compile --print-compatibility-id` outputs only that ID and a newline. If any of the three differs, trusted bytecode is forbidden. The real source → bytecode → worker round-trip is still covered by M1 integration tests.

### 13.2 P1: Structured Errors

Current spawn can only return coarse results such as `INVALID_ARGUMENT` and `SYSTEM_ERROR`. The first-party Host needs to distinguish:

- config validation;
- socketpair/posix_spawn;
- cgroup write, read-back, and attach;
- child exec;
- HELLO/sandbox;
- bundle parse/evaluate;
- required feature mismatch.

Add size-negotiated `capsid_error_info` and `capsid_worker_spawn_ex()` with a stable code, stage, optional `errno`, and safe message. Do not rely on thread-local "last error"; it is hard to use correctly across multiple shard/bootstrap threads.

### 13.3 P1: Non-Blocking Lifecycle

Short term: use ownership handoff to the reaper executor. Long term, consider splitting:

```text
request_shutdown → poll EXIT → send_signal → reap → free handle
```

into APIs that never wait, so Host can express the full lifecycle inside the event loop. Before designing the ABI, validate the need with the first-party Host implementation first; do not extend the ABI and then discover the executor was already sufficient.

## 14. Build and Dependency Governance

Existing CMake option:

```text
CAPSID_BUILD_HOST=ON|OFF
```

Current and planned targets:

```text
capsid_host_core      C++20 internal library
capsid-host-tests     unit/integration targets
capsid-host           executable (future data-plane slices)
```

Dependency principles:

- Runtime and public headers do not depend on Boost/Jansson/OpenSSL;
- Host dependencies link only Host targets;
- pin reviewed source releases and SHA-256; do not implicitly fetch floating branches at build time;
- production images lock OS, compiler, Boost, Jansson, and OpenSSL patch versions;
- generate an SPDX SBOM that preserves license and source provenance;
- enable existing `-Wall -Wextra -Wpedantic -Werror`, LTO, ASan, and UBSan;
- add a TSan job for Host concurrency core;
- even when TLS is terminated by an external proxy, OpenSSL is used only for SHA-256 and Ed25519 verification, keeping a small EVP API surface.

Versions should not be hard-coded into architecture contracts. The final manifest pins the reviewed patch release actually used; the documentation records selection criteria only and does not turn a development-environment dependency version into a permanent contract.

### 14.1 Residual Risk of the Public C++ Host

The first-party Host parses attacker-controlled HTTP in C++20, which is the largest single-process memory-safety residual risk in this design. Beast reduces the hand-written parser surface but does not reduce this risk to zero. If a data listener is remotely exploited, the attacker gains the App/state/secret surface readable by the Host service account, the in-memory Registry, and in-process global deployment authority. The absence of a privileged supervisor means this is not directly root, but it is already a breach of that Host's security boundary.

v1 fixes the following mitigations without describing them as formal proofs:

- Beast is the only HTTP framing authority; Host applies semantic gates only on parsed results and does not implement a second Content-Length/chunked parser, avoiding parser disagreements on boundaries;
- config normalization, path/authority, CIDR, attestation signed-message, and header scrubbing are written as side-effect-free pure functions with table/property tests and dedicated fuzz targets;
- the HTTP parser/serializer, URL rewriting, and lifecycle state machines continuously run ASan/UBSan; owner-shard and handoff paths run TSan; smuggling corpora and random fragmentation enter the release gate;
- Host runs as a dedicated non-root account with minimal file permissions and a separate admin socket; different trust domains use different Host processes to limit the lateral blast radius of one exploit.

If continuous fuzzing still exposes unacceptable parser/lifetime defects, keep the option to split the public HTTP frontend into a lower-privilege transport process connected to the control/worker Host through bounded, versioned IPC. Moving only the config parser into a helper cannot isolate a public HTTP exploit and is not the primary mitigation for this risk.

## 15. Testing and Acceptance

### 15.1 TDD Is the Global Delivery Rule

Host, Runtime prerequisite changes, compiler tools, and ops scripts all follow the same loop:

1. First commit an automated test that fails because the target behavior is missing; for security gates, write the rejection cases first, then the allowed cases;
2. Implement only the minimal production code that makes the current slice green; do not pre-build generic frameworks not driven by tests;
3. Refactor while tests remain green, and turn newly discovered edge cases into regression tests;
4. A slice includes tests, implementation, necessary documentation, and observable errors at the same time; "merge the feature first, add tests later" is not accepted;
5. Unit tests use fake clock, fake filesystem/adapter, and deterministic scheduler; real Linux kernel, real worker, and crash tests are in a separate integration suite and cannot be replaced by mocks;
6. coverage is only a hint; the merge gate is whether contracts, negative controls, state-machine invariants, and fault injection have been executed.

Every PR/commit description must name the `RED` test, how it initially failed, and what `GREEN` proved. When fixing a bug, the reproduction test must first fail on the unfixed code.

### 15.2 Unit and Property Tests

- JSON duplicate keys, unknown fields, depth, over-limit, and unit parsing;
- monotonicity property that an App request can never expand Host permissions;
- path ancestor, deny, wildcard hostname, CIDR, and port intersection;
- generation digest and rule ID reproducibility;
- subdomain/path/header routing positive and negative controls;
- URL rewrite golden tests for all three routing modes, plus property tests that all Forwarded/X-Forwarded headers are stripped;
- pool selection, queue, permit, and error mapping;
- crash rolling window under fake clock, exponential backoff/jitter bounds, per-App replacement singleflight, and startup fairness;
- SSE permit acquire/release exactly once, with the stream cap always preserving ordinary request slots;
- all illegal deploy/worker state-machine transitions;
- atomic recovery of active/retired/quarantined states.

### 15.3 HTTP and Flow-Control Integration

- chunked, Content-Length, TE/CL conflicts, and smuggling corpora;
- header count/bytes, slow headers, slow bodies, and early bodies;
- client-forged `Forwarded`/`X-Forwarded-*` does not affect URL or worker headers; `publicScheme=https` behind a proxy produces a stable absolute URL;
- unauthorized peers are rejected by every Admin socket endpoint; an approved UID/group has the same global deploy/retire authority over all Apps and is not mistakenly tested as a per-App ACL;
- `Expect: 100-continue` accept and reject paths;
- a large request body is not read further when credit=0;
- response credit is returned only after client write completion;
- slow clients, SSE, full stream permit returning 503 before head, disconnect, cancel, and late events;
- multiple request ID interleaving;
- different worker-crash semantics before and after response head;
- consecutive active-health failures remove the worker, replacement backoff, budget quarantine, and a crash-looping App does not block another App's deploy;
- the event that reaches the crash budget first transitions to `QUARANTINING`, does not retry to a remaining READY worker, and does not start replacement;
- a continuously busy worker can skip active probes, but passive request/stream deadline or IPC/EXIT failures still remove it and count against the budget;
- shutdown/drain/terminate do not block the reactor.

### 15.4 Deployment Fault Injection

Force-kill Host and restart after every step:

- mid source copy;
- before/after generation fsync;
- before/after COMPLETE;
- before/after pool READY;
- before/after active temp write, fsync, rename, and parent fsync;
- before/after Registry publish;
- before/after retire tombstone rename and Registry removal;
- retire tombstone rename and idempotent recovery for a quarantined App;
- before/after quarantine state rename and replacement stop;
- during old pool drain.

Acceptance invariant: after restart, only the old active, a complete new active, retired, or quarantined state is possible. Host must never point at half a generation, and must never revive a retired/quarantined pool.

Also test: symlink/magic link/device/FIFO, concurrent in-place modification, digest mismatch, ENOSPC, read-only directory, same Version with different content, concurrent deploy, and secret changes.

### 15.5 Trusted Bytecode and Secret

Trusted bytecode is implemented in the following order:

1. compatibility identity golden fails first, then library/worker/compiler three-way consistency is implemented;
2. compiler round-trip fails first, then prove that bytecode from the same source and same `sourceName` can be loaded by a real worker and behaves consistently with source;
3. attestation verifier table tests for per-field tampering, digest mismatch, duplicate/unknown fields, unknown/revoked keys, invalid signatures, and wrong App/Version/sourceName; only then implement the verifier;
4. real deployment tests cover the trusted bytecode path, the no-bytecode source path, compatibility-mismatch source fallback, and that provenance failure never falls back;
5. fuzz the attestation parser and signed-message reconstruction; under ASan/UBSan, isolate random bytes before the trusted API.

Secret is implemented in the following order:

1. schema negative controls cover unknown keys, unauthorized env names, path characters, duplicate keys, overlong values, NUL, and total-size overflow;
2. safe-read tests first construct symlinks, FIFOs, devices, inode/size changes, out-of-root paths, and modification during read; only then implement dirfd/openat2-based reading;
3. Policy Compiler goldens prove that only `Host allow ∩ App request` key/value pairs enter the descriptor, and `effective.json` contains only keys and opaque revisions;
4. real worker integration proves authorized code reads the exact value, unauthorized/duplicate keys fail startup, different workers and Apps do not see each other's values, and there is no ambient environment fallback;
5. rotation tests prove old READY workers keep the old snapshot, the new generation prewarms and switches atomically, and captured admin responses, logs, and metrics contain no secret canary.

### 15.6 Isolation Tests

- READY flags must cover the effective required bits;
- cgroup parent/leaf, limit write-back, and child membership;
- workers naturally share Host's network namespace; Host does not open or pass netns fds;
- workers have no ambient env/fd;
- App path permissions are consistent with Landlock/openat2;
- build artifacts and runtime files contain no supervisor socket, root helper, or netns creation entry points;
- skipped delegated environments remain non-evidence.

### 15.7 Performance Acceptance

#### Current Evidence Boundary

The current tree has no benchmark runner, raw A/B, or verifiable gateway/worker layered profile, so this design records no historical QPS and sets no percentage improvement target for the C++ Host. M1 must first restore the runner, regenerate the Go baseline and both-side profiles; any capacity reasoning belongs in a run report with input parameters and must not become a product promise. General rules are in [Performance evidence rules](performance-benchmarks.md).

#### Profile Gate for Each Performance Slice

Every Host data-plane milestone and every PR claiming a performance improvement must complete function-level TDD and the following before/after evidence:

1. Identical bundle, Runtime/worker build, worker count, inflight, connections, response size, cgroup CPU/memory, CPU affinity, loadgen, warmup, duration, and arrival model;
2. At least 3 measured runs, keeping all raw output; report median, dispersion, completion rate, QPS, p50/p95/p99, and loadgen schedule lag;
3. Gateway and workers use separate cgroups or equivalent process grouping; record each side's `usage_usec`, CPU/response, RSS/PSS, context switches, and page faults; machine-wide CPU alone is not enough;
4. Save one sampled profile before and after the change: Go baseline uses pprof, C++ Host/worker uses `perf record`/flamegraph or the target platform's equivalent; also save `perf stat` cycles, instructions, IPC, branches, branch-misses, cache-misses, task-clock, and migrations;
5. Host trace records queue wait, worker execution, time-to-head, IPC read/write wakeups, bytes/frame, credit stall, cross-shard delivery, and allocator counts; separate headline benchmarks from diagnostic runs so instrumentation overhead does not pollute the main result;
6. The report must identify the dominant stack/counter before the change, why the code targeted it, and whether that cost dropped after the change. Do not start implementation without a profile pointing at the target path.

Raw commands, environment manifest, commit, build flags, data, and reports must be traceable from the current tree. A profile only proves where time is spent; an A/B benchmark proves whether user-visible results improved. If either is missing, a performance conclusion cannot be merged.

When comparing the Go gateway with the first-party Host, use the exact same bundle, Runtime build, worker count, inflight, connections, response size, cgroup, loadgen, warmup, duration, and arrival model. Results must include at least QPS, completion rate, p50/p95/p99, CPU/response, Host RSS, worker PSS, queue wait, time-to-head, IPC bytes/syscalls, and cancel/error.

Record the baseline first, then freeze the regression threshold; do not start from "C++ must be faster." Optimize the event loop/HTTP layer only while profiles keep pointing there; io_uring, zero-copy, or shared memory each still need their own independent before/after profiles.

#### M1-perf: Minimal Single-Worker Host A/B Checkpoint

Do not wait for full deployment, blue-green, static pool, request body, streaming, cancel, or timeout work. As soon as M1's single-worker path listener, GET/HEAD without body, URL/header scrubbing, response credit, keep-alive, and content-correctness loop pass, run the first round immediately:

```text
same loadgen ─┬─ Go capsid-http-gw ─┬─ same capsid-worker
              └─ C++ capsid-host ────┘
```

The first round tests only the public intersection already implemented; unimplemented capabilities must not pollute the data:

- one pre-READY worker with fixed `workerCount=1`; no cold start, deploy, or autoscaling;
- one path route: first GET/HEAD with no request body and a fixed 1 KiB response, then add one real CPU/template workload;
- baseline/candidate use the exact same bundle digest, Runtime/worker binary, connections, inflight, CPU set, cgroup, warmup, measurement duration, and arrival model;
- at least 3 headline runs plus same-condition diagnostic runs; headline runs disable profile instrumentation;
- save Host/gateway and worker grouped CPU, CPU/response, QPS, completion rate, p50/p95/p99, schedule lag, RSS/PSS, context switches, queue wait, time-to-head, and IPC bytes/syscalls;
- Go saves pprof, C++ Host saves `perf record`/equivalent profile; if either side's profile or raw A/B output is missing, the report may only be marked `INCOMPLETE_EVIDENCE` and cannot form a performance conclusion.

The first report establishes a repeatable baseline and does not preset a win/loss threshold. Only after at least two independent repeats show stable dispersion may a regression threshold be frozen for later Host PRs. If the C++ Host is slower, locate the cause with profiles first; do not manufacture a win by loosening the workload, reducing validation, or disabling credit.

After request-body bidirectional credit, streaming, disconnect cancel, and timeout are complete, use the same runner to add the second data-plane checkpoint. The two checkpoints' workloads are not mixed; the early GET/HEAD baseline remains a regression line and is not overwritten by later, more complex scenarios.

The current tree has no `bench/`, so M1-perf's first test is fixed as `host_single_worker_ab_emits_complete_evidence`: first use fake baseline/candidate/loadgen to verify the runner can enforce identical conditions, three raw rounds, and both-side profiles, then connect real processes. Restoring the runner must not reverse-engineer or generate raw data from historical summaries in this document.

### 15.8 Release Gate

- Release/LTO, ASan, UBSan, TSan, and fuzz are all green;
- Host HTTP/deployment/fault-injection matrix is green;
- crash-loop quarantine, retire tombstone, continuous health, and SSE permit matrix is green;
- positive evidence for delegated cgroup and Runtime egress policy;
- config schema, examples, Policy Compiler, and Runtime descriptor goldens agree;
- SBOM, dependency hashes, worker/library/Host/build identities are pinned;
- A/B reports contain raw data and are traceable from the current tree;
- upgrading an old Host version can recover active state and App Version;
- ops docs cover backup, rollback, drain, disk full, external network boundary, and cgroup delegation failure;
- the threat model explicitly records the residual permission boundary between the public C++ Host and the global Admin socket.

## 16. Implementation Order

All of the following are v1 internal slices; they do not defer tests, trusted bytecode, or secrets to v2. Each slice first lands a test that fails observably, then the minimal implementation.

### M0: Executable Contracts

1. `host_config_rejects_network_namespace_field` fails first; revise the Host/App schema, add listener, capacity, queue, and trusted bytecode keys, and reject netns configuration fields;
2. M0.2 is executed together: `runtime_worker_compiler_identity_matches` and `bytecode_attestation_rejects_one_bit_tamper` fail together first; add library/worker/compiler three-way compatibility identity, attestation signed message, and Ed25519 verifier in one pass;
3. `secret_value_never_appears_in_effective_config` and rotation/generation goldens fail together first; freeze env schema, Host/App permission intersection, owning snapshot, Runtime descriptor view, canonical redacted metadata, opaque revision, and generation digest in one pass; the real safe-read `openat2`/file-type/concurrent-modification implementation is still driven by M1 filesystem negative controls;
4. `active_recovery_never_selects_incomplete_generation` fails first; freeze `active.json`, fsync, crash recovery, and fake filesystem interfaces;
5. `request_url_ignores_all_forwarded_headers` fails first; freeze public scheme, authority, URL rewrite, and proxy header rules;
6. `retired_or_quarantined_app_never_reactivates_on_restart` fails first; freeze retire and crash state machine;
7. `crash_loop_does_not_starve_other_app_deploy` fails first; freeze replacement backoff, budget, and permit fairness;
8. `quarantining_never_retries_to_a_remaining_worker` fails first; freeze the order that budget determination precedes retry;
9. Establish the Host test target, fake worker, fake clock, sanitizer job, and dependency lock.

Completion criteria: every v1 public contract has goldens and negative controls, and the compiler becomes an official target; production Host code may still be small, but there must be no security branch not expressed by a test.

### M1: Artifacts, Secrets, and Single-Worker Loop

M1 merges into four logical gates, but M1A + M1B are delivered as one implementation batch to avoid repeated round-trips for the runner and single-worker helper. The order forces a Linux performance baseline before the full data plane and deployment loop, but does not defer trusted bytecode, secrets, or admin to a later version. The Windows implementation is not part of the M1 release gate; M1A only preserves the platform adapter boundary so new Host code does not lock further into POSIX:

1. **M1A: the benchmark-minimal single-worker data plane.** An explicit single-worker startup mode used only for M1/benchmark loads a local source bundle directly; implement Boost.Asio/Beast HTTP/1, keep-alive, multiple request IDs on a single worker, one path listener, M0 URL/header normalization, GET/HEAD begin/end without request body, response head/body/end, response credit, a content-correctness gate, and a bounded reaper. Requests carrying a request body or another method return a fixed error before entering the worker; no implicit buffering or partial support. Also establish the POSIX `WorkerEventSource` adapter and use source audit to forbid other Host modules from calling `capsid_worker_fd()` directly. This mode is not a deployment API, does not write `active.json`, and must not be documented as a production release path.
2. **M1B: Performance evidence.** `host_single_worker_ab_emits_complete_evidence` fails first; restore the minimal Go baseline/loadgen, three-round interleaved A/B runner, correctness gate, raw samples, manifest hash, and Go/C++/worker profiles in one pass. The runner can be implemented in parallel with M1A, but real processes start only after the M1A correctness gate is green. Run the first 15.7 baseline immediately after green, without waiting for M1C/M1D.
3. **M1C: Single-worker data plane completeness.** While preserving the first GET/HEAD baseline, implement request body reads, bidirectional request/response credit, streaming, disconnect cancel, Host + Runtime request timeout, slow-client backpressure, and bounded shutdown. This batch also adds a separate `CAPSID_ENABLE_TSAN` build and Host concurrency regression; it does not block the M1B first-round Release benchmark, but M1C must not be accepted while TSan is failing. After completion, use the same runner to record the second data-plane checkpoint without overwriting the first-round samples.
4. **M1D: Secure deployment loop.** In one pass, merge compiler round-trip, artifact safe-read, signature/digest/`sourceName`/compatibility selection, secret symlink/FIFO/NUL/unauthorized-access negative controls, Policy Compiler, `capsid_env_entry[]` snapshot, and Unix admin deploy; cover the four paths of source, trusted bytecode, compatibility-mismatch source fallback, and secret into worker, and remove any dependency that treats single-worker fixture mode as a deployment interface. The acceptance order is fixed as: basic compiler/read/provider/policy contracts → managed real worker deploy/retire/recover → Unix Admin API → cross-platform and sanitizer gate → zero-probe performance regression. The Admin API must not be frozen before the coordinator's real-worker loop is closed, because it does not complete or reinterpret the deployment state machine. Any deploy path may return Active only after trusted inputs are verified, the generation is durably committed, a real worker is READY, and the canonical `active.json` has been successfully published; failures before that point must keep the old active generation.

The executable test entry frozen by M1A is:

```text
capsid-host
  --mode single-worker
  --worker <capsid-worker>
  --source-bundle <absolute-path>
  --source-name <absolute-file-URL>
  --application <AppId>
  --listen 127.0.0.1:0
  --routing path
  --public-scheme http
  --public-authority <authority>
  --initial-stream-window <positive-integer>
  --strict-sandbox on|off
  --ready-fd <inherited-fd>
```

`strict-sandbox off` is allowed only for explicit test/benchmark/native-dev builds; production Release builds must reject it. Only after the listener is bound and the worker is verified READY does Host write one line of canonical JSON to `ready-fd`: `{"schema":"capsid-host-ready-v1","app":"...","address":"127.0.0.1","port":N}`. stdout does not carry the readiness protocol. After receiving SIGTERM, the process stops accepting, cancels outstanding requests, and exits in a bounded way after handing blocking destroy to the reaper.

The CLI above is M1A's first POSIX path. Host business layers must not directly call POSIX signal or fd APIs as a result. Future Windows out-of-band readiness and shutdown/terminate/reap semantics will be frozen by RED tests once real Windows machines/hosted runners are available; M1 does not preset HANDLE/named-pipe/event ownership without evidence.

M1C adds `--request-timeout-ms <positive-integer>` to the same CLI and opens the already-frozen request-body/streaming semantics. M1A does not accept this option, so unimplemented contracts do not appear on the early benchmark executable's surface.

Completion criteria: single-worker end-to-end tests prove the v1 contracts for bytecode and secret; any signature/digest error fails closed, and secret canaries do not appear in Host output; M1-perf evidence is replayable from the current commit; the first-round data establishes only a baseline and does not advertise improvements not supported by both profile and A/B. TSan may come after that first baseline, but must precede M1C acceptance and the M2 multi-worker implementation.

### M2: Static Pool and Reliable Deployment Loop

Prerequisite gate: a separate TSan build already covers M1's HTTP event loop, worker threads, command/event handoff, disconnect/cancel, timeout, and bounded shutdown; any data race in first-party code is a blocking defect. TSan does not share a build with ASan/UBSan and is not used for performance measurement; third-party suppressions must be precise, commented, and must not cover first-party symbols.

1. Start from pool/queue state-machine failing tests; implement fixed `minReady == maxWorkers`, shard owner, admission, slow clients, and SSE permit;
2. Start from crash tests at every persistence boundary; implement stage → prewarm → health → active rename → drain;
3. Start from rotation tests; implement secret revision changes producing a new pool;
4. Start from bytecode key rotation/restart tests; implement provenance frozen with the generation;
5. Start from crash-loop/fake-clock tests; implement replacement, backoff, instability budget, quarantine, and cross-App startup fairness;
6. Start from active health and retire crash matrix; implement continuous probes, retired tombstone, and bounded drain;
7. Add structured logs, fixed metrics, and explicit fallback reasons.

Completion criteria: failure always keeps the old version, restart recovers only complete generations, and requests are bounded end to end; bytecode and secret keep the same semantics across blue-green, rollback, and restart; crash loops cannot monopolize permits, and retired/quarantined Apps cannot be revived by restart.

### M3: Production v1 Release Gate

1. subdomain and trusted-header listeners;
2. delegated cgroup hierarchy; verify Host contains no netns configuration, supervisor, or network-management code;
3. full Host/global/App admission control;
4. idempotent operation queries, explicit rollback, generation retention/GC;
5. structured Runtime errors;
6. full security, fuzz, sanitizer, soak, performance A/B, and crash matrix;
7. systemd unit, external network boundary, Admin trust boundary, permissions, key/secret rotation, retire, quarantine, upgrade, and ops documentation.

Completion criteria: strict isolation, trusted bytecode, and secret positive/negative proofs are completed on the target Linux environment, and the production traffic/release failure gates pass. Only then is the design called v1.

### M4: Data-Driven Follow-Up Capabilities

- bounded autoscaling with `minReady < maxWorkers`;
- endpoint/application circuit breaker and explicit opt-in activation-guard rollback;
- two-level memory admission based on profile/PSS high percentiles while preserving the hard ceiling;
- security ceiling reload;
- optional OTLP adapter;
- HTTP/2, built-in TLS, or third-party transport adapter;
- io_uring/zero-copy only after profiles justify it;
- start a Windows native-dev track on available real Windows machines or hosted runners: MSVC/CMake, process/transport/reap, additive event-source ABI, loopback-only Host, local Admin identity/ACL, and real integration for source/bytecode/env/request/stream/cancel/crash;
- Windows native production isolation: first freeze a separate threat model and semantic feature bits, then evaluate Job Object, Restricted Token, AppContainer, and deployment network boundaries; do not reuse the Linux seccomp/Landlock bits, because seccomp/Landlock bits express different guarantees and Windows-native feature bits must be defined from Windows mechanisms first.

## 17. Confirmed Decisions

1. **Both source and trusted bytecode enter v1**: bytecode must pass signature provenance, digest, `sourceName`, and compatibility identity checks; source is always retained for compatibility fallback;
2. **Secrets enter workers through `capsid:env`**: an immutable snapshot is generated by permission intersection; rotation produces a new generation, and no Host output contains plaintext;
3. **C++20 + Asio/Beast**: retain the C++ owner-shard model but do not hand-write epoll/HTTP parsers;
4. **Keep it simple**: `active.json` is a single-writer atomic state file; active remains a generation pointer, retire/quarantine use tombstones in the same file, SQLite is not introduced; static pool comes first, autoscaling later;
5. **No Host netns supervisor**: Host/App have no netns configuration; workers naturally use Host's network environment, and additional network isolation is entirely an optional deployment-environment measure;
6. **Crash loops fail closed**: v1 automatically replaces with backoff and enforces the generation budget; after the budget is exceeded, persist quarantine and 503, and do not automatically roll back by default;
7. **Explicit retire**: `POST /v1/apps/{app}/retire` atomically writes a tombstone, stops routing, and drains; decommissioning is not expressed by deleting a directory;
8. **URL does not guess proxy semantics**: listeners explicitly declare public scheme/authority, and v1 deletes and does not trust all Forwarded/X-Forwarded headers;
9. **Minimal continuous health and streaming isolation**: when a health path is configured, sample periodically and count failures into the instability budget; SSE uses a separate per-worker permit that preserves ordinary request slots;
10. **Windows native development is retained but deferred**: not implemented on Windows machines/hosted runners until real hardware is available, and cross-compilation or WSL2 must not fake a pass. M1 only establishes the Host `WorkerEventSource` adapter boundary to prevent POSIX dependencies from spreading into the data plane.

## 18. External Selection References

- [Boost.Beast HTTP documentation](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_http.html):
  incremental HTTP/1 parsing, serialization, and buffer-oriented interfaces;
- [Boost.Asio POSIX stream descriptor](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview/posix/stream_descriptor.html):
  take over an existing POSIX fd and perform async read/write/wait;
- [Jansson decoding API](https://jansson.readthedocs.io/en/latest/apiref.html):
  `JSON_REJECT_DUPLICATES` can directly reject duplicate keys in security-sensitive config;
- [OpenSSL release strategy](https://www.openssl-library.org/policies/releasestrat/):
  select a series still under upstream support and pin a patch release;
- [systemd cgroup delegation](https://systemd.io/CGROUP_DELEGATION/):
  boundary for a non-root service managing a delegated cgroup subtree.

## 19. Final Recommendation

Deliver v1 as one provable vertical loop:

> securely snapshot the source directory, compile typed permissions, prewarm a fixed pool, atomically switch App state, drain the old pool in a bounded way, back off and persistently quarantine crash loops, retire explicitly, fix the stable worker URL and proxy-header contract, provide continuous health and separate SSE capacity protection; trusted bytecode enters the worker through the full trust chain, secrets enter the worker through the least-privilege `capsid:env` snapshot, and requests and responses are always bounded by credit, queues, deadlines, and Linux isolation.

Follow the M0-to-M3 TDD slices to make this loop small and rigorous; after v1 is complete, use real profiles to decide autoscaling, HTTP/2, and the more complex control plane. This keeps the product simple and consistent with Runtime's actual capability boundary.
