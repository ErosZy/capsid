# Capsid Binding v1 Technical Design

> Status: implemented. The acceptance matrix in §8 maps the design to current
> tests. Privileged Linux profile probes run as a mandatory Hosted Validity
> gate: a skipped sandbox test is a failure.

## 1. Purpose and decisions

A Binding hosts capability-bearing code written, installed, and trusted by the
Host operator, such as a MongoDB, MySQL, or Redis client. Untrusted application
code can call only methods explicitly returned by the Binding factory. It
cannot directly or indirectly acquire the Binding Runtime's network,
filesystem, socket, or other native authority.

Binding v1 makes these decisions:

- One worker process contains a User Runtime and, when needed, one Binding
  Runtime per Binding package. Every Binding Runtime has an independent
  QuickJS heap, global, module loader, module cache, and job queue, isolated
  from the User Runtime and from every other Binding.
- Binding Runtimes are created only when the worker declares at least one
  Binding. A zero-Binding worker retains the existing single-runtime path,
  loads no Binding code, allocates no Binding heap, and adds no Binding sandbox
  requirements.
- The package directory name is the Binding ID. The same ID is used as the App
  configuration key and import specifier. There is no `db -> provider:
  mongo` alias layer.
- An App can configure one instance of each Binding ID in v1. Multiple
  instances of one provider require a future protocol version.
- Every User-to-Binding call is asynchronous. The runtimes communicate only
  through bounded C++ neutral values and queues. QuickJS `JSValue` objects,
  object references, and native handles never cross the boundary.
- A package explicitly declares high-level resources and versioned Capsid
  sandbox profiles. Capsid does not infer syscalls from source, imports, or a
  trace, and packages cannot supply raw syscall lists.
- Seccomp, Landlock, namespaces, cgroups, and rlimits are process boundaries.
  Unforgeable native operation gates enforce the finer User/Binding split.
- Servers, listeners, raw sockets, FFI, dynamic libraries, process creation,
  and workers remain permanently unavailable.

```text
capsid-worker process
├── User Runtime
│   ├── untrusted application JavaScript
│   ├── capsid:* User facades
│   └── UserCapabilityPolicy
├── Binding Runtimes                         one per Binding ID, non-empty set
│   ├── Binding A: Host-trusted index.js
│   │   ├── explicitly authorized capsid:* modules
│   │   └── BindingCapabilityPolicy[A]
│   ├── Binding B: Host-trusted index.js
│   │   ├── explicitly authorized capsid:* modules
│   │   └── BindingCapabilityPolicy[B]
│   └── ...
└── Process Sandbox
    ├── seccomp: User requirements ∪ Binding profiles
    ├── Landlock: User paths ∪ Binding paths
    ├── namespace and firewall
    ├── cgroup v2
    └── rlimit
```

The User Runtime plus one runtime per Binding isolate JavaScript objects and
capabilities. They do not provide native-exploit isolation. A threat model that
must survive memory corruption in QuickJS, TJS, or Capsid C++ requires a
separate Binding process; that is outside v1.

## 2. Package and configuration contracts

### 2.1 Host registry

`capsid/host-v2` accepts an optional `bindingsRoot`:

```json
{
  "apiVersion": "capsid/host-v2",
  "bindingsRoot": "/etc/capsid/bindings"
}
```

The directory layout is fixed:

```text
/etc/capsid/bindings/
  mongo/
    manifest.json
    index.js
  redis/
    manifest.json
    index.js
```

The directory name is the public Binding ID:

```text
Package: /etc/capsid/bindings/mongo
Config:  bindings.mongo
Import:  capsid:binding/mongo
```

An ID must match `[a-z][a-z0-9-]{0,62}`. The Host scans direct children only,
and each package contains exactly `manifest.json` and `index.js`.

The scanner enforces these rules:

- symbolic links, hard links, FIFOs, sockets, devices, and extra files are
  rejected;
- the root, package directories, and files must be owned by root or the Host's
  effective UID and cannot be group/world writable;
- scanning starts from an `O_NOFOLLOW` root descriptor, and packages/files
  are opened through `openat` and
  `fstatat(AT_SYMLINK_NOFOLLOW)`;
- device, inode, owner, mode, link count, size, mtime, and ctime are checked
  before and after reads; final directory entries and complete directory
  listings are checked again, so rename/replace, in-place mutation, and entry
  insertion/removal fail closed;
- `manifest.json` is limited to 1 MiB, `index.js` to 16 MiB, and all source
  loaded by one Generation to 64 MiB;
- malformed manifests or directory layouts fail Host startup; JavaScript
  syntax, factory, or method-table failures reject the referencing
  Generation during warm-up;
- startup produces an immutable registry snapshot. v1 has no watcher or live
  package reload; changing a package requires a Host restart;
- the committed `bindings.json` Generation artifact is canonical, sorted by
  ID, strictly bounded, and rejects duplicate/extra fields. Recovery reads
  that snapshot and revalidates manifest, App subset, derived modules and
  profiles, config, secret revisions, and aggregate limits; it does not rescan
  `bindingsRoot`;
- `index.js` is one self-contained ESM file. Third-party dependencies must be
  bundled. Relative, absolute, `file:`, and remote imports are denied. Dynamic
  imports can resolve only a manifest-granted `capsid:*` module under the
  current authenticated Binding context.

Discovering a package does not load it into every worker. Only Bindings
declared by the App enter its Generation and startup protocol.

#### Local single-worker and static-pool development

Binding development does not require the managed coordinator. `single-worker`
and `static-pool` accept the same `capsid/app-v2` document and an explicit
`--bindings-root`:

```sh
# Supply the normal required listen, worker, and bundle arguments as well.
capsid-host --mode single-worker \
  --capsid-json ./capsid.json \
  --bindings-root ./bindings

capsid-host --mode static-pool \
  --workers 2 \
  --capsid-json ./capsid.json \
  --bindings-root ./bindings
```

There is no implicit current-directory lookup. Declaring a Binding without an
explicit registry fails startup. With a registry, the CLI scans once before
spawn. Static-pool also reads, validates, and compiles the App policy once;
every shard shares the immutable registry and effective Binding snapshot while
still running its own worker process and Binding Runtime.

Local modes use the production scanner, manifest validator, manifest/App
intersection, `LOAD_BINDING` ordering, and READY sandbox proof. They do not
have a managed secret provider, so any `secrets.valueFrom` declaration fails
explicitly. Secret integration must be tested in managed mode.

If an App declares no Binding, `--bindings-root` is unnecessary, no
`LOAD_BINDING` message is sent, and no Binding Runtime is created. Secure
local registry scanning is supported on Linux, macOS, and Windows native-dev.
Windows uses reparse-point (symlink/junction) and hard-link rejection plus
an ownership allow-list of the process identity and Administrators/SYSTEM
(broad groups such as Everyone, Users and Authenticated Users are never
trusted), and Everyone/Users writable ACL checks. Sandbox
profiles remain Linux kernel capabilities: Windows runs the same Binding
Runtime and native gates, but profile enforcement must still be validated on
Linux; a Windows-run package should not claim seccomp/Landlock profile
protection.

### 2.2 Binding manifest

```json
{
  "apiVersion": "capsid/binding-v1",
  "sandbox": {
    "requires": ["network-client", "filesystem-read"]
  },
  "permissions": {
    "modules": ["capsid:internal/core", "capsid:utils"],
    "net": {
      "allow": ["*:27017"]
    },
    "fs": {
      "read": ["/etc/capsid/mongo"],
      "write": []
    },
    "env": [],
    "stdio": []
  }
}
```

The manifest is the package's Host-approved maximum authority. It does not
declare the Binding ID, method list, config schema, User-side JavaScript, or an
App instance name. Installing the package in the Host-controlled registry
approves this maximum.

`permissions.modules` is required. Other permissions default to deny.
Unknown fields, duplicate keys, duplicate permissions, unknown modules, and
modules unavailable in the current build are rejected.

The exact module, dependency, profile, and resource rules are normative in
[binding-modules.md](binding-modules.md). The v1 grantable set contains twelve
explicit `capsid:*` modules; there is no wildcard grant and no public `tjs:*`
contract.

### 2.3 App declaration

`capsid/app-v2` keys `bindings` directly by Binding ID:

```json
{
  "apiVersion": "capsid/app-v2",
  "entry": "bundle.mjs",
  "bindings": {
    "mongo": {
      "permissions": {
        "net": {
          "allow": ["127.0.0.1:27017"]
        },
        "fs": {
          "read": ["/etc/capsid/mongo/ca.pem"],
          "write": []
        },
        "env": [],
        "stdio": []
      },
      "config": {
        "database": "orders",
        "tls": true
      },
      "secrets": {
        "password": {
          "valueFrom": "mongo-password"
        }
      }
    }
  },
  "pool": {
    "minReady": 1,
    "maxWorkers": 1
  }
}
```

Rules:

- every key must match a Binding ID in the immutable Host registry;
- `provider`, `alias`, and `instance` do not exist and are rejected as
  unknown fields;
- one App can configure one entry per Binding ID in v1;
- `config` is an opaque JSON object. The Host validates JSON depth and size
  but does not interpret members. The limit is 256 KiB per Binding;
- a secret reference is exactly `{"valueFrom":"<secret-key>"}`; inline secret
  values are forbidden;
- App `net`, `fs`, `env`, and `stdio` entries must be statically
  provable subsets of the manifest. Omission means deny all in that class;
- the manifest, not the App, selects required Capsid modules and sandbox profiles;
- strict `host-v1` and `app-v1` schemas retain their prior behavior.

Network rules describe a final host, IP, or CIDR plus one exact port. Database
schemes such as `mongo://` and `mysql://` are not policy syntax:

```text
db.example.com:27017
*.internal.example.com:443
127.0.0.1:6379
10.0.0.0/8:3306
[::1]:6379
[2001:db8::/32]:443
*:27017
```

The App rule must be statically contained by a manifest rule. DNS answers are
not used to prove containment. `*` includes loopback, private, link-local,
metadata, and IPv6 destinations; it still cannot bypass the deployment's
network namespace, routing, or firewall.

The `env` field is reserved in v1: it is parsed and intersected, but the
Binding Runtime has no gated accessor and the Host injects no values. It should
remain empty until a copied immutable environment API exists.

### 2.4 JavaScript package interface

```js
export default function createBinding({ config, secrets, log }) {
  let client;

  return {
    async find(input, call) {
      client ??= await createClient({
        ...config,
        password: secrets.password
      });

      return client.find(input, { signal: call.signal });
    }
  };
}
```

- The default export is a synchronous factory and cannot return a Promise.
- Factory initialization can perform pure JavaScript computation only. Native
  I/O and timers require an active Binding call token.
- `config` and `secrets` are deeply frozen null-prototype objects.
- `log` is frozen and provides
  `debug/info/warn/error(message, fields?)`; Capsid attaches App,
  Generation, Binding, and request metadata. The object belongs to exactly one
  Binding and carries its Binding ID; a call from any other Binding is rejected.
  Because Binding globals are not shared, another Binding cannot obtain the
  object through JavaScript at all.
- Own enumerable functions in the returned object become public methods. A
  package exports 1–128 methods.
- Method names are valid JavaScript identifiers no longer than 64 bytes.
  `constructor`, `prototype`, `__proto__`, `then`, `catch`, and
  `finally` are rejected.
- A User method takes zero or one structured input. The implementation receives
  `(input, call)`.
- `call` is frozen and contains `requestId`, `deadline`, and a
  Binding-Runtime `AbortSignal`.
- A method may return a value or Promise; User-side completion is always
  asynchronous.
- A closure may own a connection pool. Native handles are owned by the Binding
  ID and can survive across requests for that Binding, but cannot cross to
  another Binding.
- v1 has no explicit shutdown hook. Destroying the worker releases runtimes and
  libuv resources.

The worker generates the User facade; packages do not provide `user.js`:

```js
import mongo from "capsid:binding/mongo";

const rows = await mongo.find({
  collection: "users",
  filter: { active: true }
});
```

The synthetic ESM has one frozen, null-prototype default export. Calls outside
an active request async context return a rejected Promise. Binding code cannot
import `capsid:binding/*` or call another Binding.

### 2.5 Logging boundary

Binding logs do not reuse forgeable User text. The worker sends an exact,
dedicated frame:

```text
binding-id:u16 | level:u16 | message:u32 | fields-json:u32
```

- `fields` must be a plain object. Arrays, proxies, unserializable values,
  and message/fields payloads over 16 KiB are rejected.
- Before emission, current Binding secret plaintext is redacted from both the
  message and JSON-escaped fields.
- The log function authenticates its caller: the Binding ID captured in the
  function must match the authoritative Binding identity (the dispatch window,
  or the loading Binding during factory warm-up). A mismatch throws and emits
  nothing.
- The Host requires the exact frame flag, no trailing bytes, valid ID/level,
  and duplicate-rejecting JSON. It canonicalizes fields as one JSON object, so
  fields cannot inject a second log line.
- Only the Host adds `application` and `generation`; the authenticated frame
  supplies `binding`, and the protocol header supplies `request`.
- Warm-up uses the same decoder. A malformed Binding log fails warm-up and is
  never downgraded to ordinary text. Secret values enter neither snapshots nor
  logs.

## 3. Runtime and capability isolation

### 3.1 Three distinct policy layers

```text
ProcessSandbox
    = User OS requirements
    ∪ every Binding sandbox profile loaded in this worker

UserCapabilityPolicy
    = Host User policy ∩ App User permissions

BindingCapabilityPolicy[bindingId]
    = Binding manifest maximum ∩ App Binding resources
```

Every native operation performs:

```cpp
authorize(origin, operation, resource);
```

The origin comes from native state attached to the `JSContext` or handle:

- User Runtime code consults only `UserCapabilityPolicy`;
- each Binding Runtime has its own `JSContext`; Binding code consults the
  policy for the Binding ID of that context's current dispatch window;
- a missing runtime origin or Binding context fails closed;
- JavaScript arguments cannot set or override origin or Binding ID;
- native handles record runtime domain, Binding ID, and access mode at creation
  and recheck ownership on later operations.

### 3.2 Process union does not grant User authority

Seccomp and Landlock are process-level controls. A Binding filesystem-write
profile can enlarge the worker's kernel syscall/path union, but cannot enlarge
the User JavaScript API:

- User code cannot import Binding-only core, filesystem, SQLite, WASI, or
  POSIX-socket modules;
- `capsid:fs` remains the existing User facade and still needs Host User
  policy authorization;
- a Binding grant adds no method to a User facade;
- User filesystem gates consult only the User policy even when Landlock admits
  a Binding path;
- Binding filesystem gates consult only the current Binding policy;
- File, Socket, SQLite, WASI, and Stream handles cannot be structured-cloned;
- FFI, native add-ons, raw sockets, `createFromFD`, and User WASI remain
  forbidden.

For example, the process Landlock union may include:

```text
User:
  read /srv/apps/orders/public

Mongo Binding:
  read  /etc/capsid/mongo
  write /var/lib/capsid/mongo
```

The User native gate still rejects both Mongo paths, and the Mongo gate rejects
the User path.

### 3.3 Modules and permanently denied capabilities

Module authorization and operation authorization are independent.
`capsid:internal/core` is a hardened Binding surface: client network,
filesystem, watch, SQLite, WASI, global `fetch`, and WebSocket operations
still pass the current Binding policy and native-handle owner checks.

The exact twelve grantable modules and their transitive dependencies are
defined in [binding-modules.md](binding-modules.md). Known forbidden examples
include `capsid:ffi`, `capsid:worker`, `capsid:http-server`,
`capsid:process`, `capsid:signals`, `capsid:internal/worker`, and
`capsid:posix-socket`.

Listeners, `bind/listen/accept`, AF_UNIX, raw sockets, arbitrary descriptors,
dynamic libraries, spawn/exec, workers, process control, signals, and SQLite
extension loading remain unavailable regardless of manifest content.

## 4. Linux sandbox profiles

### 4.1 Explicit profiles, never inferred syscalls

Static source scanning cannot reliably derive all syscalls. Dynamic code paths,
TJS/libc versions, CPU architectures, and error paths make traces incomplete.
Therefore `sandbox.requires` selects only Capsid-defined, versioned profiles:

| Profile | Capability |
| --- | --- |
| `network-client` | DNS and IPv4/IPv6 TCP/TLS/UDP clients |
| `filesystem-read` | read-only files and directories |
| `filesystem-write` | write/create/truncate/sync and controlled rename/remove |
| `filesystem-watch` | inotify/fswatch operations |
| `sqlite` | SQLite file I/O, locks, positional I/O, sync, and truncate |
| `wasi` | controlled WASI preopens and execution |

Profile names and meaning are fixed by the build. Unknown, unsupported, or
unimplemented profiles reject startup. Profiles are tied to the Capsid/TJS
compatibility identity; a vendor upgrade requires a new audit.

`strace` and seccomp audit logs can discover omissions but never generate a
production allowlist automatically. A syscall absent from a profile stays
denied at runtime. Resource/profile consistency is checked before spawn; see
[binding-modules.md](binding-modules.md).

### 4.2 Seccomp and Landlock boundaries

The strict sandbox supports the client-side stream/datagram sockets,
`connect`, send/receive, DNS, TLS, event-loop, and memory operations needed
by ordinary MongoDB, MySQL, and Redis clients under `network-client`.

Every profile union still denies:

- `bind`, `listen`, and `accept`;
- Unix-domain and raw sockets;
- clone, fork, and exec;
- ptrace, BPF, perf, and io_uring;
- mount and namespace mutation;
- executable memory.

Seccomp starts with permanent denies and adds a fixed syscall subset per
profile; no profile overrides a permanent deny. Landlock uses explicit read
and write masks and installs the union of User and Binding path rules.
Directory write access can create, update, delete, and rename ordinary files
inside the authorized directory, but cannot create devices, FIFOs, Unix
sockets, or escape the directory.

`filesystem-write` requires a Landlock ABI that can express the required
mask. An older kernel fails startup instead of weakening protection. Client
profiles do not enable user-selected local bind addresses and do not open the
process-wide `bind` syscall.

Privileged Linux conformance tests execute real read, write, rename, unlink,
mkdir, watch, SQLite, TCP connect, and WASI workloads. Each profile also proves
that fork, AF_UNIX, raw sockets, bind, and executable mappings are denied. CI
runs `ctest -L sandbox` as root and treats Skip 77 as failure.

Network namespaces, firewalls, and cgroups are prepared by the deployment.
Binding manifests cannot create or mutate them.

### 4.3 Startup proof

Before spawn, the Host computes:

```text
EffectiveBindingPolicies
EffectiveSandboxProfiles
LandlockPathRules
SandboxProfileDigest
```

Profiles and kernel paths are unioned across Bindings, but runtime native
policies remain separate. Startup proceeds in this order:

1. receive and validate `HELLO`;
2. receive zero or more Binding descriptors and the App bundle as bounded
   bytes, without executing JavaScript;
3. recanonicalize sandbox requirements and compare the Host digest;
4. enter configured namespaces and set rlimits and `no_new_privs`;
5. install Landlock and the seccomp TSYNC filter;
6. run side-effect-free negative sandbox probes;
7. create the User Runtime plus one Binding Runtime per Binding (or only the
   User Runtime for a zero-Binding worker) and load code;
8. return READY with the sandbox proof.

READY v3 adds:

```text
sandbox_profile_digest
seccomp_mode
landlock_abi
applied_feature_bits
network_namespace_identity (when configured)
```

The Host compares every required field. A successful install and proof show
that the intended profile was applied; profile conformance and Binding
integration tests are still required to prove application behavior. A
zero-Binding worker retains the existing baseline sandbox identity.

## 5. Dual runtimes and asynchronous RPC

### 5.1 Event loop and async ownership

With Bindings, the TJS overlay supports one Capsid-owned `uv_loop_t`, one
independent TJS/QuickJS runtime per Binding attached to that shared loop, plus
the User runtime. Each runtime has a separate Promise-job pump and runtime
teardown that does not close the shared loop. The scheduler pumps User and
Binding jobs fairly and enters only one runtime at a time.

No second thread is created; `clone/clone3` remains denied. Runtime/context
opaque state replaces process-global runtime assumptions. Per-Binding RPC
state (captured Promise/AbortController intrinsics, method table, factory
object) lives in a table keyed by Binding ID; no `JSValue` is stored or
decoded in a runtime that did not create it.

A Binding async context carries:

```text
BindingToken
Optional BindingCallToken
```

The Binding token controls capability and handle ownership. The call token
controls request metadata, deadline, abort, and the result receiver.
Connection-pool continuations retain the Binding token across requests.

### 5.2 Call queues

C++ owns `user_to_binding` and `binding_to_user` queues:

1. the User facade creates a Promise;
2. Capsid validates the active request, Binding, method, deadline, and quotas;
3. input is cloned into a C++ neutral value;
4. the call is queued and the Promise is returned immediately;
5. the scheduler invokes the method in that Binding's own Runtime;
6. `Promise.resolve()` normalizes synchronous and asynchronous returns;
7. result or error is cloned into the return queue;
8. the User Runtime resolves or rejects the original Promise.

Binding code never synchronously re-enters the User Runtime and cannot invoke
another Binding.

### 5.3 Structured clone

Allowed values:

- `undefined`, `null`, Boolean, Number, BigInt, String, and Date;
- ArrayBuffer copied without transfer/detach, and Uint8Array;
- Array, plain object, and null-prototype object.

Rejected values:

- Function, Promise, and Symbol;
- getters and setters; inspection never invokes a getter;
- cycles, Map, Set, and weak collections;
- Error, RegExp, and custom class instances;
- proxies that cannot be safely enumerated;
- every socket, file, SQLite, WASI, stream, or other native handle.

Fixed bounds:

- 1 MiB encoded value in either direction;
- maximum depth 64;
- at most 10,000 aggregate nodes/properties;
- at most 64 outstanding Binding calls per request;
- at most 1,024 outstanding Binding calls per worker;
- default 64 MiB heap per Binding Runtime, allocated and charged only for a
  non-empty Binding set.

Cancellation or deadline expiry aborts related calls. Undispatched calls are
removed; late results from dispatched calls are discarded without destroying a
shared connection pool. An ordinary throw/rejection fails one call. Binding
Runtime OOM, an uninterruptible loop, or fatal runtime error poisons the worker
and uses normal recovery replacement.

## 6. ABI, protocol, and Generation identity

The existing `capsid_worker_config` layout and ABI v7 remain compatible.
Bindings use an additive, versioned descriptor API:

```c
typedef struct capsid_binding_descriptor {
    uint32_t struct_size;
    uint32_t version;
    const char *binding_name;
    capsid_bytes source;
    capsid_bytes config_json;
    const capsid_binding_secret *secrets;
    uint32_t secret_count;
    const capsid_binding_policy *policy;
    const capsid_sandbox_requirements *sandbox;
} capsid_binding_descriptor;

capsid_result capsid_worker_load_binding(
    capsid_worker *worker,
    const capsid_binding_descriptor *binding);
```

The API copies descriptors, source, config, secrets, and policy before return.
`capsid_worker_load_binding()` is valid only before the App bundle.

Worker protocol v3:

```text
HELLO
LOAD_BINDING(mongo...)
LOAD_BINDING(redis...)
LOAD_BUNDLE(app...)
READY + sandbox proof
```

`LOAD_BUNDLE` seals the Binding set. Zero `LOAD_BINDING` messages use the
existing single-runtime path.

Generation Identity v2 includes a sorted `binding_set_digest` over:

- Binding ID;
- manifest and source digests;
- canonical config digest;
- effective permission digest;
- sandbox requirement/profile digest;
- secret key ID and opaque secret revision;
- Binding Runtime compatibility version.

Secret values enter neither digest, disk, logs, nor audit records. Worker
replacement and Host recovery use the committed immutable Binding snapshot,
not later registry contents.

## 7. TDD implementation contract

Every change follows Red → Green → Refactor: add the smallest failing test,
confirm the intended failure, implement the minimum production behavior, then
run the focused suite, full `ctest`, and applicable sanitizers.

### 7.1 Schema and registry

Tests cover the one-to-one directory/config/import ID, rejection of
`provider/alias/instance`, unchanged v1 schemas, secure file ownership and
FD-relative scanning, size limits, profile/module consistency, and
deterministic digests.

### 7.2 Zero-Binding regression

Tests prove no second runtime, heap, scheduler, profile, or Landlock path is
created; undeclared imports do not lazy-load Bindings; the single-runtime ABI
and performance path remain intact; local single-worker/static-pool skip the
registry when no Binding is declared and use the production registry and READY
proof when one is declared.

### 7.3 Origin and permission isolation

Tests prove a Binding filesystem write grant does not widen User access,
Landlock union paths do not bypass native gates, User `capsid:fs` does not
unlock Binding-only modules, forged origins fail, native owners cannot cross runtime or
Binding IDs, and native handles cannot be returned through structured clone.

### 7.4 Sandbox profiles

Tests cover unknown/missing/inconsistent profiles, working database TCP/TLS,
permanent server/socket denies, filesystem path boundaries, SQLite I/O,
complete READY proof comparison, and an unchanged zero-Binding baseline.

### 7.5 Dual runtime

Tests cover heap/global/module/job isolation, fair shared-loop scheduling, no
cross-runtime `JSValue`, async Binding/call token propagation, repeated
cancellation/destruction, and sanitizer checks for leaks and races.

### 7.6 RPC and clone

Tests round-trip every allowed type and reject getters, proxies, cycles,
functions, promises, classes, and handles. They cover concurrency,
backpressure, cancellation, deadline, late result, ordinary error, quotas, and
runtime poison.

### 7.7 Egress and native modules

DNS, TCP, TLS, UDP, fetch redirects, WebSocket clients, forbidden POSIX
sockets, and connection-pool reconnects each require positive authorized-target
and negative pre-syscall denial tests. Untested client entry points cannot join
the grantable surface.

### 7.8 Generation and recovery

Tests prove changes to manifest, source, config, policy, profile, runtime
compatibility, or secret revision change identity; secret values never persist;
replacement uses the committed snapshot; and fatal Binding errors enter the
existing crash budget.

### 7.9 Privileged Linux gate

Real profile workloads run in an environment supporting namespaces, cgroup,
seccomp, and Landlock. A sandbox Skip 77 fails Hosted Validity. Every profile
has positive and permanent-deny probes. TJS vendor upgrades rerun syscall
tracing for audit, profile tests, and sandbox digest golden tests; traces never
become allowlists.

## 8. Acceptance criteria and evidence

- `capsid:binding/<binding-id>` provides asynchronous calls to a Host
  Binding.
- Directory, App configuration, and import use the same Binding ID, with no
  provider alias.
- Zero-Binding workers allocate no Binding Runtimes and add no Binding sandbox
  cost.
- The User Runtime and each Binding Runtime, and any two Binding Runtimes,
  share no heap, global, module cache, JavaScript object, or native handle.
- The manifest is the Host maximum; an App only narrows resources.
- Packages select built-in profiles rather than syscalls.
- Process sandbox rules are unioned, while native gates always use runtime
  origin and Binding ID.
- Binding filesystem write authority never widens User filesystem APIs or
  paths.
- Every client egress path is policy-gated; servers, raw sockets, FFI, process,
  and worker capabilities are unconditionally denied.
- READY contains a Host-verifiable sandbox proof.
- package, policy, profile, config, and secret revision enter immutable
  Generation identity; secret values never persist.
- ABI v7 and the full zero-Binding path remain compatible.

### 8.1 Requirement-to-test matrix

The tests below exercise real native or Host boundaries; parser-only or mock
tests are not sufficient evidence by themselves.

| Acceptance boundary | Primary automated evidence |
| --- | --- |
| Directory, App, and import IDs match; provider aliases are rejected | `host_binding_config`, `host_binding_manifest`, `host_binding_registry`, `host_binding_compile` |
| Local single-worker/static-pool use the production registry/compiler/READY path and shards share immutable compilation | `host_local_capsid_policy`, `host_static_pool_server_binding_local_policy` |
| FD-relative registry scanning, replacement races, owner/mode/link/size checks, immutable recovery | `host_binding_registry`, `host_binding_compile`, `host_secret_snapshot` |
| Zero Bindings preserve the single-runtime and sandbox baseline | `worker_zero_binding_regression` |
| Runtime heap/global/module/job isolation and asynchronous queues | `worker_zero_binding_regression` (32 MiB per-Binding allocation smoke + global/module visibility), `binding_rpc` |
| Structured clone types, getter/proxy/cycle/handle rejection, and quotas | `worker_zero_binding_regression`, `binding_rpc` |
| Factory token absence, deep-frozen config/secrets, safe method discovery | `worker_zero_binding_regression`, `host_binding_compile` |
| User/Binding visibility isolation, same-Binding native-handle reuse, and indirect module denial | `capability_policy`, `worker_zero_binding_regression`, `worker_sandbox_enforcement` |
| DNS/TCP/TLS/UDP/redirect/WS/FS/SQLite/WASI gates; POSIX socket denial | `egress_policy`, `worker_fetch_direct_egress`, `worker_zero_binding_regression`, `txiki_vendor_patch_integrity` |
| 64-bit RPC IDs, deadlines, abort, late results, quotas, and poison | `binding_rpc`, `worker_zero_binding_regression` |
| READY profile digest, feature bits, seccomp, Landlock, namespace proof | `host_binding_compile`, `host_worker_executor_contract`, `worker_sandbox_network_namespace`, `worker_zero_binding_regression` |
| Secret revision/runtime compatibility enter identity; values do not persist | `host_binding_compile`, `host_secret_snapshot`, `worker_zero_binding_regression` |
| Authenticated logs, Host-owned metadata, JSON fields, secret redaction | `structured_log_emits_single_line_json`, `host_worker_executor_contract`, `worker_zero_binding_regression` |
| Linux profile positive operations and permanent fork/AF_UNIX/raw/bind/executable-mmap denies | `worker_binding_sandbox_{read,write,watch,sqlite,network,wasi,union}`, `worker_sandbox_namespaces` |
| TJS overlay version, patch order, audit anchors, and upgrade baseline | `txiki_async_context_inventory_audit`, `txiki_vendor_patch_integrity`, `txiki_overlay_audit_negative_controls` |

With per-Binding runtimes, a Binding cannot obtain another Binding's native
handle or `log` object through JavaScript, so the old direct
cross-Binding-owner-mismatch assertion is replaced by the visibility test;
the native owner-tag and log-identity checks remain in the worker as
defense-in-depth.

### 8.2 Audit evidence recorded on 2026-08-16

- macOS Debug: ten core registry, compile, secret snapshot, IPC, capability,
  Host log/executor, RPC, sandbox, and zero-Binding groups passed.
- macOS ASan with `detect_leaks=0` because Darwin LeakSanitizer is unavailable:
  eight Host/RPC/dual-runtime groups passed with `halt_on_error=1`.
- Linux 6.8/GCC 13 TSan with ASLR disabled: eight
  Host/IPC/policy/RPC/sandbox/dual-runtime groups passed. This is sanitizer
  evidence, not a substitute for real seccomp/Landlock probes.
- Privileged Linux 6.8/GCC 13: Host compile/registry, IPC, capability, RPC,
  zero-Binding, namespace, and seven real profile probes passed 16/16 with no
  skips. After the final WebSocket patch, sandbox enforcement, all seven
  profiles, and zero-Binding regression passed 9/9 with no skips.
- WebSocket coverage used a real `101 Switching Protocols` endpoint and
  proved pre-connect denial, authorized handshake, async Binding-ID
  restoration, and cross-Binding handle rejection.
- The then-current macOS serial regression passed 235/235 after excluding the
  explicitly external WPT configuration and two clean-worktree-only gates;
  package reproducibility passed separately.
- Hosted Validity runs full Release `ctest`, root `ctest -L sandbox` with
  skips forbidden, delegated cgroup/netns checks, and ASan/UBSan/TSan matrices.

This record is historical evidence, not a substitute for current CI. Any TJS,
profile, schema, protocol, or native-entry-point change requires rerunning the
privileged gate and this matrix.
