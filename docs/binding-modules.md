# Binding Modules and Permissions

This reference defines the authority available to Host-authored
`capsid/binding-v1` packages. It does not describe modules available to
untrusted application code; application modules are covered by
[module-permissions.md](module-permissions.md).

The source-of-truth grantable set is duplicated, intentionally, in the Host
manifest validator and the worker policy compiler. A mismatch must fail tests:

- `src/host/config.cc` validates package manifests;
- `src/capability_policy.cc` compiles the worker-side Binding policy;
- `tests/test_worker_zero_binding.cc` imports every grantable module in a
  real Binding Runtime.

## Authority model

A module import is permitted only when both conditions hold:

```text
module import
    = module built into this Capsid/TJS build
    ∩ module named in the Binding manifest
```

Import permission is not operation permission. A native operation is permitted
only when every relevant layer agrees:

```text
native operation
    = permitted runtime origin and Binding ID
    ∩ permitted module surface
    ∩ required sandbox profile
    ∩ resource inside the manifest maximum
    ∩ resource inside the App's narrower grant
    ∩ process sandbox, namespace, and deployment policy
```

The App cannot add modules or profiles. It can only narrow the manifest's
`net`, `fs`, `env`, and `stdio` resources. Omitting a resource class
means deny all resources in that class. Unknown modules, duplicate entries,
unknown profiles, and inconsistent profile/resource combinations fail closed.

The module loader checks every import, including transitive imports. Listing a
top-level module does not implicitly authorize its dependencies.

Package source and manifests use only `capsid:*`. After the public name is
authorized, the Binding loader maps it to the pinned TJS implementation inside
the worker. A small audited edge table resolves implementation imports such as
the hashing module's core dependency back to the corresponding public policy
grant. Package code cannot name or grant the private `tjs:*` side directly.

## Complete grantable module set

Exactly these twelve module specifiers are grantable in Binding v1:

| Module | Intended use | Manifest dependencies | Profiles and resource gates | Security notes |
| --- | --- | --- | --- | --- |
| `capsid:assert` | Assertions used by package code | None | None | Pure JavaScript utility; module gate still applies. |
| `capsid:getopts` | Argument-like option parsing | None | None | Pure JavaScript utility. Binding factories do not receive process arguments. |
| `capsid:hashing` | Incremental cryptographic hashes exposed by TJS | `capsid:internal/core` | None beyond the module gates | The internal-core dependency must be listed explicitly. This does not grant network or file access. |
| `capsid:internal/core` | Hardened client I/O, filesystem, timer, crypto, compression, WebAssembly, and runtime primitives used by trusted package code | None | The individual operation determines whether `network-client`, `filesystem-read`, `filesystem-write`, or `filesystem-watch` and a matching resource grant is required | This is a Capsid-hardened surface, not unrestricted upstream TJS core. Server, raw-FD, process, FFI, and ambient stdio surfaces are removed or denied. |
| `capsid:internal/path` | Low-level path parsing and manipulation | None | None | String/path computation only; it does not access the filesystem. |
| `capsid:ipaddr` | IP address parsing and CIDR operations | None | None | Pure JavaScript utility; it does not grant egress. |
| `capsid:path` | Public path manipulation API | `capsid:internal/path` | None | Both the public module and its internal dependency must be listed. |
| `capsid:readline` | Line-oriented transformation over caller-provided Web Streams | None | No ambient stdio grant | Capsid removes ambient `tjs`/process stdio access. The module can only use streams the caller already possesses. |
| `capsid:sqlite` | SQLite databases | `capsid:internal/core` | `sqlite`; database paths must also be covered by effective `fs.read`/`fs.write` grants as required | Extension loading is forbidden. File ownership and path gates remain active for the lifetime of each database handle. |
| `capsid:utils` | Formatting and inspection helpers | None | None | Pure JavaScript utility. |
| `capsid:uuid` | UUID generation and parsing | None | None | Does not grant other ambient authority. |
| `capsid:wasi` | WASI execution with controlled preopens and streams | `capsid:internal/core` | `wasi`; preopens require matching effective filesystem grants; conventional streams require exact `stdio` grants | No unrestricted host filesystem or inherited process descriptors are exposed. |

There is no wildcard module grant. A package should list only the modules its
single-file `index.js` and all of its transitive imports require.

## Hardened `capsid:internal/core`

`capsid:internal/core` is broad because several useful TJS libraries depend on
it, but broad import surface does not mean broad authority.

While Binding code is running, Capsid derives the current Binding ID from
unforgeable native runtime state. Client TCP, TLS, UDP, DNS, global
`fetch`, redirects, and WebSocket connections are checked against that
Binding's effective egress policy. DNS names are checked before resolution;
the selected address is checked again after resolution. Reconnects and async
continuations retain the Binding owner token.

Filesystem opens, reads, writes, directory operations, and watches are checked
against the current Binding's effective path policy. Native handles record
their runtime domain, Binding ID, and access mode. A handle cannot be cloned to
the User Runtime or operated by another Binding.

The Binding core surface does not expose a usable server or descriptor escape:

- no TCP, TLS, UDP, HTTP, or WebSocket listener;
- no `bind`, `listen`, `accept`, or arbitrary local bind address;
- no raw `fileno`, `createFromFD`, Pipe, TTY, or raw `core.fs.File`;
- no inherited stdin/stdout/stderr descriptor constants or direct print path;
- no `guessHandle`, process control, signals, worker creation, or FFI.

The frozen `log` object passed to the Binding factory is independent of
`permissions.stdio`. It emits authenticated Binding log frames and performs
secret redaction; it is not an ambient file descriptor.

## Sandbox profiles

Profiles are versioned Capsid capabilities. Packages select profile names;
they never provide raw syscall lists.

| Profile | Required when | Kernel-level effect |
| --- | --- | --- |
| `network-client` | `net.allow` is non-empty or package code performs DNS/TCP/TLS/UDP/HTTP/WebSocket client I/O | Enables the fixed client-side syscall subset. It never enables `bind`, listeners, Unix sockets, or raw sockets. |
| `filesystem-read` | `fs.read` is non-empty | Adds fixed read operations and the effective read paths to the process sandbox union. |
| `filesystem-write` | `fs.write` is non-empty | Adds controlled create/write/sync/rename/remove operations and effective write paths. |
| `filesystem-watch` | Package code watches filesystem paths | Adds the fixed watch syscall subset. Each watched path still needs an effective `fs.read` grant. |
| `sqlite` | `capsid:sqlite` is listed | Adds SQLite locking, positional I/O, truncate, and synchronization operations. Files remain path-gated. |
| `wasi` | `capsid:wasi` is listed | Enables the controlled WASI execution profile. Preopens and streams remain resource-gated. |

Profile consistency is validated before a worker starts:

- non-empty `net.allow` requires `network-client`;
- non-empty `fs.read` requires `filesystem-read`;
- non-empty `fs.write` requires `filesystem-write`;
- `capsid:sqlite` requires `sqlite`;
- `capsid:wasi` requires `wasi`.

The process-level seccomp and Landlock policy is the union of User requirements
and all loaded Binding requirements. This union never becomes the authorization
decision for JavaScript. User operations still consult only the User policy,
and Binding operations consult only the current Binding policy.

## Resource permissions

### Network

`permissions.net.allow` contains host, IP, or CIDR targets with one exact
port:

```text
db.example.com:27017
*.internal.example.com:443
10.0.0.0/8:3306
[::1]:6379
*:27017
```

The App rule must be statically provable as a subset of a manifest rule. DNS
results are not used to prove that subset. A wildcard is intentionally broad,
including private, loopback, link-local, metadata, and IPv6 destinations;
deployments should prefer narrow targets and use network namespaces/firewalls
as an additional boundary.

### Filesystem

`permissions.fs.read` and `permissions.fs.write` contain canonical absolute
paths. The App may select an equal path or a provable descendant of a manifest
path. The native operation gate and Landlock both enforce the resulting paths.
Granting a Binding path never grants that path to User JavaScript.

`filesystem-watch` is an operation profile, not a path grant. A watched path
also needs an effective read path.

### Standard streams

`permissions.stdio` uses exact stream names. In v1 it is consumed by
`capsid:wasi` when attaching conventional streams. `capsid:readline` has no
ambient stdio and the factory's `log` object does not use this grant.

### Environment variables: reserved in v1

The manifest and App schemas currently parse, intersect, serialize, and store
`permissions.env`, but Binding Runtime v1 exposes no gated environment
accessor and the Host does not inject environment values. A non-empty list
therefore grants no usable capability.

Packages should keep `env: []`. This field must not be documented as working
until Capsid provides a copied, immutable Host-supplied environment snapshot
and a per-Binding native accessor. A future hardening change may reject
non-empty lists explicitly; it must not silently expose the Host process
environment.

## Permanently forbidden modules and capabilities

The following known module specifiers are permanently ungrantable:

- `capsid:ffi`;
- `capsid:worker`;
- `capsid:http-server`;
- `capsid:process`;
- `capsid:signals`;
- `capsid:internal/worker`;
- `capsid:posix-socket`.

All `tjs:*`, Capsid modules outside the exact twelve-entry grantable set,
relative/absolute paths, `file:`, and remote URLs are denied in Binding package
source. Packages must be bundled as one self-contained `index.js`. A dynamic
import is not an escape hatch: it can resolve only a manifest-granted
`capsid:*` module while an authenticated Binding context is active.

`capsid:posix-socket` is not grantable even for database clients because its
upstream surface combines client operations with `createFromFD`, listeners,
AF_UNIX, and sensitive socket options. A future client-only facade would need
independent egress, owner, and negative-capability tests before becoming
grantable.

These capabilities remain forbidden regardless of profile unions:

- server/listener operations;
- raw and Unix-domain sockets;
- FFI, native add-ons, and dynamic libraries;
- spawn, exec, worker creation, signals, and process control;
- SQLite extension loading;
- cross-runtime or cross-Binding native handles.

## Package recipes

### Pure computation

```json
{
  "apiVersion": "capsid/binding-v1",
  "permissions": {
    "modules": ["capsid:assert", "capsid:utils", "capsid:uuid"],
    "net": { "allow": [] },
    "fs": { "read": [], "write": [] },
    "env": [],
    "stdio": []
  }
}
```

No sandbox profile is required.

### MongoDB, Redis, or MySQL client

```json
{
  "apiVersion": "capsid/binding-v1",
  "sandbox": {
    "requires": ["network-client", "filesystem-read"]
  },
  "permissions": {
    "modules": ["capsid:internal/core", "capsid:utils"],
    "net": { "allow": ["db.internal.example.com:27017"] },
    "fs": {
      "read": ["/etc/capsid/db/ca.pem"],
      "write": []
    },
    "env": [],
    "stdio": []
  }
}
```

Use the database's actual port: MongoDB commonly uses 27017, Redis 6379, and
MySQL 3306. Those are examples, not protocol-specific Capsid syntax. If TLS
credentials arrive through managed secrets and no CA file is read, omit the
filesystem profile and paths.

### SQLite

```json
{
  "apiVersion": "capsid/binding-v1",
  "sandbox": {
    "requires": ["filesystem-read", "filesystem-write", "sqlite"]
  },
  "permissions": {
    "modules": ["capsid:internal/core", "capsid:sqlite"],
    "fs": {
      "read": ["/var/lib/capsid/orders"],
      "write": ["/var/lib/capsid/orders"]
    },
    "env": [],
    "stdio": []
  }
}
```

### WASI

```json
{
  "apiVersion": "capsid/binding-v1",
  "sandbox": {
    "requires": ["filesystem-read", "wasi"]
  },
  "permissions": {
    "modules": ["capsid:internal/core", "capsid:wasi"],
    "fs": {
      "read": ["/opt/capsid/wasm"],
      "write": []
    },
    "env": [],
    "stdio": ["stdout", "stderr"]
  }
}
```

Only effective filesystem paths may become preopens, and only exact granted
stream names may be attached.

## Review checklist

Before installing a Binding package, the Host operator should verify:

1. every listed module is imported directly or transitively;
2. no unused module or sandbox profile remains;
3. every network target has the narrowest practical host/CIDR and exact port;
4. every filesystem path is canonical and no broader than required;
5. `env` is empty in v1;
6. the package contains only `manifest.json` and a self-contained
   `index.js`;
7. positive integration tests exercise each requested capability;
8. negative tests prove adjacent targets, paths, server operations, and handle
   transfer remain denied.
