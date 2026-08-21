# Host Capability Policy

The public ABI provides an optional and immutable `capsid_capability_policy`. It
is a policy snapshot given by the host at worker startup, not a JavaScript
permission prompt.

## Three-layer gate

Capabilities are evaluated independently in the following order:

1. The module or operation must exist in the restricted build;
2. The module must appear in `allowed_modules`;
3. The concrete resource must match an allow rule and must not match any deny
   rule.

Unknown descriptors, unknown modules in `allowed_modules`, duplicate or zero
rule IDs, non-canonical resources, and unsupported policy versions all make
`capsid_worker_spawn()` return `CAPSID_INVALID_ARGUMENT`. spawn validates
synchronously and copies all nested strings and rules before returning, so a
policy change requires creating a new worker.

`allowed_modules` accepts only known public `capsid:*` names. All `tjs:*` and
`tjs:internal/*` are permanently forbidden implementation namespaces and cannot
be opened through policy; writing them into the allowlist makes spawn fail. For
the complete specifier determination and API→permission mapping, see the
[JavaScript Modules and Permissions Reference](module-permissions.md).

## Currently available scope

The current restricted build provides twelve explicitly authorizable modules:

- `capsid:permissions`: read-only query of the host capability policy;
- `capsid:env`: reads the immutable environment snapshot the host explicitly
  provides, authorized per key;
- `capsid:system`: reads only the compile-time runtime version and feature
  flags;
- `capsid:storage`: in-memory key-value storage authorized per namespace, alive
  only in a single worker;
- `capsid:stdio`: converts allowed stdout/stderr writes into bounded host log
  events;
- `capsid:fs`: reads allowed canonical host paths; no write or watcher support;
- `capsid:assert`, `capsid:getopts`, `capsid:hashing`, `capsid:ipaddr`,
  `capsid:utils`, `capsid:uuid`: pure utilities with no ambient authority.

Utility modules still must be listed one by one in `allowed_modules`; "built" is
not the same as "importable by an application." They have no resource
operations, so they cannot bypass the third-layer operation gate, and they gain
no `globalThis.tjs`, process, filesystem, environment, or network capabilities.
`capsid:hashing` only lets the loader resolve the trusted
`tjs:internal/core` hashing primitive on its behalf once; direct application
import of that internal module is always rejected, including when it is already
in the module cache.

Standard `fetch()` reaches the operation gate through `net_policy`;
`capsid:env.get()`, `capsid:system.get()`, `capsid:storage`, and
`capsid:stdio.write()` operations reach the same gate through
`CAPSID_PERMISSION_ENV`, `CAPSID_PERMISSION_SYS`, `CAPSID_PERMISSION_STORAGE`,
and `CAPSID_PERMISSION_STDIO` rules respectively.

`fetch()`'s domain rules are the authorization boundary: after a domain and port
are allowed at the host stage, the addresses that client-side DNS resolves for
that domain (including private, loopback, and link-local addresses) are also
considered allowed; callers do not need to — and should not — enumerate IPs that
change with DNS. Resolved addresses still match explicit IP/CIDR rules, and any
explicit deny takes precedence over a domain allow. Requests made directly to a
numeric IP continue to undergo protected-range protection; private, loopback,
link-local, and similar addresses need an explicit IP/CIDR allow to be accessed
directly by numeric IP. Therefore, regardless of whether
`internal-api.example:443` is treated as an internal or public domain, as long
as it has been authorized, all addresses it actually resolves to — including
protected ranges such as `10/8` — are accessible; this determination does not
depend on a "public/internal" classification of the domain.

Rejected `fetch()` errors distinguish three causes: the host/port has no
matching authorization rule, the address is in a protected range and was not
explicitly authorized, or an explicit deny rule was hit. Error text is for
diagnostics only; policy decisions still follow the rules and deny precedence,
and applications should not parse error strings to enforce authorization.

The `write`, `ffi`, `rawSocket`, and `engine` matchers can already be parsed and
tested, but the corresponding operations are not built; JavaScript queries
return `unavailable`. `read`, `env`, and `storage` are built; `stdio` provides
only stdout/stderr output, stdin remains `unavailable`; `sys` has only
`runtimeVersion` and `featureFlags` available, other resources remain
`unavailable`.

The capability policy current version is 2; the decoder still accepts version 1
without an environment snapshot field, preserving its original semantics.
Unknown versions, version 1 carrying environment data, or version 2 missing the
snapshot section all fail closed.

The machine-readable authoritative list is
[`capability-manifest.json`](capability-manifest.json). CMake computes SHA-256 at
configure time and writes that value into every audit record.

process, worker, HTTP/WebSocket server, WASI, internal runtime modules, remote
import, and file/path import are permanently forbidden and cannot be enabled
through a capability descriptor. `capsid:path` is not built yet because the full
upstream API's `resolve()`/`relative()` would implicitly read `tjs.cwd`; it can
be reconsidered only after a capability-scoped virtual cwd is introduced.

All other known extensions remain fail closed, and the reasons and reopening
conditions are also recorded in the machine manifest:

- `capsid:net` does not reuse the upstream POSIX socket; it would bypass the
  resolved-address egress hook. HTTP(S) client needs should use standard
  `fetch()`, which already covers the hostname, every DNS address, and every
  redirect;
- `capsid:websocket` must wait until the client-only path also completes
  DNS/address rechecking, queue limits, cancel, and request ownership;
  server/upgrade is out of product scope;
- `capsid:sqlite`'s benchmark-only fixed read-only database is not a product
  API. Before official release it must disable the extension loader, add an SQL
  authorizer and memory/row-count/execution-time quotas, and allow only in-memory
  databases or files authorized by a path capability;
- `capsid:readline` depends on terminal stdin, which is explicitly closed; input
  should be provided by the host through FetchRPC;
- `capsid:fs.write` would reopen the seccomp/Landlock write boundary, and
  `capsid:fs.watch` would introduce cross-request callbacks; both remain
  unavailable until their mutation/ownership designs are complete.

These entries are not "allowed but not yet documented"; they are explicit
non-provision conclusions guaranteed by manifest audit, per-module startup
rejection tests, and final binary negative controls.

## C embedding example

```c
capsid_egress_rule net_rule;
capsid_egress_rule_init(&net_rule);
net_rule.action = CAPSID_EGRESS_ALLOW;
net_rule.target = "api.example.com";
net_rule.port_start = net_rule.port_end = 443;
net_rule.rule_id = 1001;

capsid_egress_policy net;
capsid_egress_policy_init(&net);
net.rules = &net_rule;
net.rule_count = 1;

const char *modules[] = {
    "capsid:permissions",
    "capsid:env",
    "capsid:hashing"
};
capsid_permission_rule env_rule;
capsid_permission_rule_init(&env_rule);
env_rule.action = CAPSID_PERMISSION_ALLOW;
env_rule.permission = CAPSID_PERMISSION_ENV;
env_rule.resource = "APP_MODE";
env_rule.rule_id = 1002;

capsid_env_entry environment;
capsid_env_entry_init(&environment);
environment.name = "APP_MODE";
environment.value = "production";

capsid_capability_policy capability;
capsid_capability_policy_init(&capability);
capability.application_identity = "tenant-a";
capability.allowed_modules = modules;
capability.allowed_module_count = 3;
capability.rules = &env_rule;
capability.rule_count = 1;
capability.env_entries = &environment;
capability.env_entry_count = 1;
capability.net_policy = &net;

capsid_worker_config config;
capsid_worker_config_init(&config);
config.worker_path = "/path/to/capsid-worker";
config.capability_policy = &capability;
```

The C++11 header provides `capsid::CapabilityPolicyBuilder` to hold temporary
strings and descriptors before calling `capsid_worker_spawn()`; environment
snapshots are added with `.environment("APP_MODE", "production")`.

If both `capsid_worker_config.egress_policy` and
`capsid_capability_policy.net_policy` are configured, the effective policy is
the intersection: the request hostname, every DNS-resolved address, and every
redirect must all be allowed. The native HTTP client remains inside the worker
and does not introduce a host HTTP broker.

## JavaScript query

An authorized bundle can only query the immutable effective state:

```js
import { permissions } from "capsid:permissions";

permissions.query({
  name: "net",
  host: "api.example.com",
  port: 443,
}); // "granted", "denied", "partial" or "unavailable"
```

The exported object is frozen and provides no `request()`, `revoke()`, prompt,
or mutation API. Import errors distinguish:

- `module is forbidden`: the category can never be enabled;
- `module is unavailable`: the category is known, but the current build does not
  implement it;
- `module is not authorized`: the module exists but is not in `allowed_modules`.

## Environment snapshot

```js
import { env } from "capsid:env";

env.get("APP_MODE"); // "production", "" or undefined
```

The worker process environment is always cleared; the runtime never calls
`getenv()` and never enumerates the host environment. Only keys explicitly
provided in `capsid_env_entry` and simultaneously covered by a valid allow rule
can enter HELLO. deny rules take precedence; unauthorized keys, duplicate keys,
wildcard key names, null pointers, oversized values, and snapshots for an
unauthorized `capsid:env` all make spawn fail. Values are deep-copied before
spawn returns, so later modifications to the caller's original buffers do not
affect the worker; different workers hold independent snapshots.

`env.get()` re-executes the operation gate on every access: authorized-but-not-
provided keys return `undefined`, denied keys throw, and neither falls back to
the host ambient environment.

## Runtime metadata

```js
import { system } from "capsid:system";

system.get("runtimeVersion"); // "0.2.1"
system.get("featureFlags");   // frozen compile-time capability object
```

This module does not call uname/gethostname and does not read users, network
interfaces, load, uptime, or memory state. Even if such sys allow rules exist in
the policy, the operations still return `unavailable`; this keeps "the rule
parser recognizes a resource" from being mistaken for "the build implements it."

## Worker in-memory storage

```js
import { storage } from "capsid:storage";

storage.set("tenant-a", "session", "value");
storage.get("tenant-a", "session"); // "value" or undefined
storage.keys("tenant-a");           // frozen array sorted by key
storage.delete("tenant-a", "session");
storage.clear("tenant-a");
```

Each operation revalidates the exact namespace rule; namespaces allow only ASCII
letters, digits, `_`, `-`, `.`, at most 128 bytes, and no wildcards. Keys must be
non-empty, at most 256 UTF-8 bytes, and cannot contain NUL; values are at most
16 KiB. Each namespace holds at most 256 entries, with keys and values totaling
at most 64 KiB. Unauthorized access, invalid input, single-value overruns, and
quota rejections all produce operation deny audits. Successful access records an
allow only the first time each worker uses that namespace, so normal key-value
operations do not drown out subsequent denial events.

State lives only in `capsid-worker`'s private memory: subsequent requests in the
same worker can see it, different workers do not share it, and destroying the
worker clears it. The module does not open files, read paths, or reuse
txiki.js's SQLite-backed localStorage, so there is no implicit disk or directory
permission.

## Bounded standard output

```js
import { stdio } from "capsid:stdio";

stdio.write("stdout", "started");
stdio.write("stderr", "warning");
```

`write()` never touches the worker's real fds. It encodes the stream name and at
most 16 KiB of string into a `CAPSID_EVENT_LOG` carrying the current request ID;
the host decides whether and how to persist it. stdout and stderr must each have
an exact allow rule; stdin stays `unavailable` even if configured with allow.
Invalid streams, over-limit messages, and unauthorized writes all fail closed and
produce audit. A successful stream records allow only once per worker to keep
log churn from crowding out denial events.

Messages are measured in UTF-8 bytes and embedded NULs are preserved. Output is
also bounded by the worker's bounded IPC queue; when the queue is full, writes
throw synchronously rather than blocking the process or falling through to real
standard output.

## Read-only filesystem

```js
import { fs } from "capsid:fs";

fs.readText("/srv/app/config.json");
fs.stat("/srv/app/config.json"); // frozen { type, size }
fs.list("/srv/app/assets");      // frozen sorted name array
```

Only canonical absolute paths are accepted, and every call performs
`CAPSID_PERMISSION_READ` allow/deny matching. Strict sandbox synchronously adds
effective allow roots to read-only Landlock rules; if a configuration root is a
symlink, the worker fails startup before executing the bundle. Actual opens use
`openat2(RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS)`, so symlinks in both final
and intermediate path components are never followed, and there is no
check-then-open race window.

`readText()` accepts only regular files and reads at most 1 MiB; `stat()` reports
type and size only for regular files or directories; `list()` returns at most
1024 entries and does not follow directory entries. All APIs are synchronous and
bounded, and returned objects are frozen. Writing, deletion, rename, mkdir, and
fswatch are not in this module; `CAPSID_PERMISSION_WRITE` continues to query as
`unavailable`, and Landlock remains globally write-denying.

## Audit events

Capability decisions are delivered to the host through `CAPSID_EVENT_AUDIT` and
decoded with `capsid_audit_record_decode()`. Records include worker, application
identity, request ID, stage, decision, rule ID, policy version, module,
capability, canonical resource, and capability manifest SHA-256.

The audit view points into the event buffer and is valid only until the same
worker's next event API call. For the same consecutive non-allow decision, a
worker records at most 8 occurrences and caps the overall rate at 64 per second.
The host must keep draining events and must not treat this channel as an
unbounded log.

The capability rule is the application authorization boundary; seccomp,
Landlock, namespaces, cgroups, and host firewall are independent and stronger
process boundaries. If FFI or raw sockets are opened in the future, the host
must accept that they can bypass ordinary JavaScript policy.

### Escape-level capability gate

`capsid:ffi` and `capsid:raw-socket` are not ordinary capabilities. They can
bypass path, DNS, redirect, and per-operation authorization, so the current
security conclusion is "not provided," not "open cautiously with rules."

- `CAPSID_ENABLE_FFI_CAPABILITY` and
  `CAPSID_ENABLE_RAW_SOCKET_CAPABILITY` explicitly exist and default to `OFF`;
  setting either switch to `ON` fails closed at configure time because the
  project has no standalone ABI, OS sandbox profile, or complete negative
  controls yet;
- Passing `BUILD_WITH_FFI=ON` directly to txiki is also rejected by the
  top-level configuration and cannot bypass the Capsid switch; the restricted
  txiki overlay does not package FFI, POSIX sockets, or related bytecode, and
  the final worker must additionally pass symbol, translation unit, and module
  specifier audits.

Automated evidence: `escape_capability_defaults` (both switches default OFF and
txiki FFI is not silently enabled),
`escape_capability_configure_negative_controls` (enabling fails configure),
`worker_binary_audit` and its negative controls (dangerous
initializers/translation units/loader specifiers do not enter the final worker,
and the auditor can catch injection), `worker_sandbox_enforcement` (real process
strict seccomp/Landlock, including raw socket denial), and the capability
manifest rejection matrix (importing either module from an application returns
`unavailable`).

If the product genuinely needs either capability in the future, a new security
design and ABI version should be opened, covering at least library path and
symbol constraints, socket family/type/protocol, DNS/redirect bypass, fd
passing, resource quotas, cross-request/cross-tenant isolation, and a standalone
OS sandbox. The current fail-closed switches cannot be changed to
"experimentally available" to circumvent these prerequisites.
