# JavaScript Modules and Permissions Reference

This article is for users writing bundles and configuring host policy. It
answers two questions:

1. Which modules an application can import;
2. Which host permission each JavaScript API requires.

The machine-readable authoritative list is
[`capability-manifest.json`](capability-manifest.json), and the C ABI
authoritative definition is
[`include/capsid/runtime.h`](../include/capsid/runtime.h).

## The most important rules

- Applications may only import public `capsid:*` modules from the host
  allowlist;
- `tjs:*` and `tjs:internal/*` are permanently forbidden to applications and
  cannot be opened through configuration;
- `allowed_modules` only decides whether a module can be imported; it does not
  automatically allow resource operations inside the module;
- `fetch()` is not `capsid:net`; it uses a separate egress policy;
- Most Web APIs that do not require a module import need no capability rule,
  with the exception of `fetch()`;
- deny rules always take precedence, and unknown or malformed policy makes
  worker startup fail.

This query descriptor family borrows from Deno's permission expression, but it
is not the `Deno.permissions` API and does not provide `request()`, `revoke()`,
or permission prompts. JavaScript can only query the immutable result given by
the host through `capsid:permissions`.

## How module specifiers are determined

| Specifier in the bundle | Configurable? | Result |
| --- | --- | --- |
| Built `capsid:*` | Yes | Must be listed in `allowed_modules`, otherwise `module is not authorized` |
| Known but not built `capsid:*` | Can be listed but cannot be used | `module is unavailable` on import |
| Any `tjs:*` | No | Permanently `module is forbidden` |
| `tjs:internal/*` | No | Permanently `module is forbidden` |
| `node:`, `file:`, `http:`, `https:`, `data:` | No | Permanently `module is forbidden` |
| `/absolute/path`, `./relative/path`, `../relative/path` | No | Permanently `module is forbidden` |
| Unknown specifier | No | `module is unavailable` |

Therefore the following is not a valid configuration:

```cpp
capsid::CapabilityPolicyBuilder policy;
policy.allow_module("tjs:assert"); // error: spawn returns CAPSID_INVALID_ARGUMENT
```

The correct approach is to use a public Capsid name:

```cpp
capsid::CapabilityPolicyBuilder policy;
policy.allow_module("capsid:assert");
```

The equivalent C configuration is:

```c
const char *modules[] = {
    "capsid:assert",
    "capsid:hashing"
};

capsid_capability_policy capability;
capsid_capability_policy_init(&capability);
capability.application_identity = "report-worker";
capability.allowed_modules = modules;
capability.allowed_module_count = 2;

capsid_worker_config config;
capsid_worker_config_init(&config);
config.capability_policy = &capability;
```

If `allowed_modules` contains `tjs:*`, unknown modules, duplicate modules, or
permanently forbidden items, it does not wait for a JavaScript import to fail;
instead `capsid_worker_spawn()` returns `CAPSID_INVALID_ARGUMENT` directly.

Applications must be bundled as self-contained ESM at release time. Relative,
absolute, remote, or npm runtime imports left in the bundle are rejected.

## Public mapping for txiki.js utility modules

The Capsid restricted runtime internally reuses six txiki.js utility
implementations that carry no ambient authority, but applications may only use
the corresponding `capsid:*` names:

| Forbidden direct application import | Authorizable public module | Public exports | Operation permission |
| --- | --- | --- | --- |
| `tjs:assert` | `capsid:assert` | Default assertion object: `equal`, `notEqual`, `is`, `isNot`, `ok`, `notOk`, `fail`, `throws`, `doesNotThrow`, and aliases | None |
| `tjs:getopts` | `capsid:getopts` | Default `getopts(args, options)` | None |
| `tjs:hashing` | `capsid:hashing` | `SUPPORTED_TYPES`, `createHash()`; hash objects provide `update()`, `digest()`, `bytes()` | None |
| `tjs:ipaddr` | `capsid:ipaddr` | Default ipaddr.js object | None |
| `tjs:utils` | `capsid:utils` | `format()`, `inspect()` | None |
| `tjs:uuid` | `capsid:uuid` | Default uuid object | None |

"Operation permission: none" is not the same as "module automatically visible."
These six modules still need to be listed individually in `allowed_modules`;
only after a successful import do their pure computation APIs avoid a resource
rule.

`capsid:hashing`'s implementation needs `tjs:internal/core` once. The loader
only allows `tjs:hashing` to obtain that module while resolving its own
dependencies; an application directly or dynamically importing
`tjs:internal/core` is always rejected, including when it is already in the
module cache. `globalThis.tjs`, `process`, `Deno`, and `Bun` do not exist.

## Complete API-to-permission mapping

| JavaScript API | Module that must be authorized | C/C++ permission configuration | rule resource | Match semantics |
| --- | --- | --- | --- | --- |
| `permissions.query()` | `capsid:permissions` | No operation rule | Determined by the query descriptor | Query only; grants no permission |
| `env.get(name)` | `capsid:env` | `CAPSID_PERMISSION_ENV` | Environment variable name, e.g. `APP_MODE` | exact; a rule may use a single trailing `*` |
| `system.get("runtimeVersion")` | `capsid:system` | `CAPSID_PERMISSION_SYS` | `runtimeVersion` | exact |
| `system.get("featureFlags")` | `capsid:system` | `CAPSID_PERMISSION_SYS` | `featureFlags` | exact |
| `storage.get/set/delete/clear/keys(namespace, ...)` | `capsid:storage` | `CAPSID_PERMISSION_STORAGE` | namespace | exact |
| `stdio.write("stdout", message)` | `capsid:stdio` | `CAPSID_PERMISSION_STDIO` | `stdout` | exact |
| `stdio.write("stderr", message)` | `capsid:stdio` | `CAPSID_PERMISSION_STDIO` | `stderr` | exact |
| `fs.readText(path)` | `capsid:fs` | `CAPSID_PERMISSION_READ` | canonical absolute path | This path and its subtree |
| `fs.stat(path)` | `capsid:fs` | `CAPSID_PERMISSION_READ` | canonical absolute path | This path and its subtree |
| `fs.list(path)` | `capsid:fs` | `CAPSID_PERMISSION_READ` | canonical absolute path | This path and its subtree |
| Global `fetch(url, init)` | None | `capsid_egress_policy`; also recommended to configure capability `net_policy` | hostname/IP/CIDR + port | The two policy layers intersect |
| Six pure utility modules | Corresponding `capsid:*` | No operation rule | None | Module gate only |

`CAPSID_PERMISSION_NET` cannot be written into an ordinary
`capsid_permission_rule`. Network rules must use `capsid_egress_rule` /
`capsid_egress_policy`; the C++ builder counterpart is `.net()`.

The following permission names can be parsed or queried by policy, but currently
have no corresponding product API:

| Permission | C enum | Current status |
| --- | --- | --- |
| `write` | `CAPSID_PERMISSION_WRITE` | unavailable; no file write, delete, rename, or mkdir |
| `ffi` | `CAPSID_PERMISSION_FFI` | unavailable |
| `rawSocket` | `CAPSID_PERMISSION_RAW_SOCKET` | unavailable |
| `engine` | `CAPSID_PERMISSION_ENGINE` | unavailable |
| Other `sys` kinds | `CAPSID_PERMISSION_SYS` | unavailable |
| `stdio`'s `stdin` | `CAPSID_PERMISSION_STDIO` | unavailable |

Adding allow rules for these does not make the operations appear;
`permissions.query()` still returns `unavailable`.

## Host configuration recipes

### Expose pure utilities only

```cpp
capsid::CapabilityPolicyBuilder capability;
capability
    .application_identity("formatter")
    .allow_module("capsid:assert")
    .allow_module("capsid:hashing")
    .allow_module("capsid:utils")
    .allow_module("capsid:uuid");
```

No `allow(CAPSID_PERMISSION_...)` is needed here, and the application does not
gain file, environment, or network access from this configuration.

### Environment variables

The module, operation rule, and environment snapshot are all required:

```cpp
capsid::CapabilityPolicyBuilder capability;
capability
    .application_identity("orders-api")
    .allow_module("capsid:env")
    .allow(CAPSID_PERMISSION_ENV, "APP_MODE", 1001)
    .environment("APP_MODE", "production");
```

```js
import { env } from "capsid:env";

env.get("APP_MODE"); // "production"
```

The worker does not read the host process environment. If `.environment()` has
no matching allow rule, contains duplicate keys, or the module is not
authorized, spawn fails closed.

### Read-only directory with one denied subdirectory

```cpp
capsid::CapabilityPolicyBuilder capability;
capability
    .allow_module("capsid:fs")
    .allow(
        CAPSID_PERMISSION_READ,
        "/srv/capsid/orders",
        1101)
    .deny(
        CAPSID_PERMISSION_READ,
        "/srv/capsid/orders/secrets",
        1102);
```

`/srv/capsid/orders/config.json` is allowed;
`/srv/capsid/orders/secrets/token` is denied. Paths must be canonical absolute
paths; deny takes precedence over allow. Strict sandbox also writes effective
read-only roots into Landlock; if an authorization root itself is a symlink,
startup fails.

### storage namespace

```cpp
capability
    .allow_module("capsid:storage")
    .allow(CAPSID_PERMISSION_STORAGE, "tenant-a", 1201);
```

```js
import { storage } from "capsid:storage";

storage.set("tenant-a", "session", "value");
storage.get("tenant-a", "session");
storage.keys("tenant-a");
storage.delete("tenant-a", "session");
storage.clear("tenant-a");
```

Namespaces must be individually authorized exactly. Data exists only in a single
worker's memory, is not shared across workers, and is lost when the worker is
destroyed.

### Outbound Fetch

It is recommended to write the same target into both the host direct egress
policy and the capability net policy:

```cpp
capsid_egress_rule direct_rule;
capsid_egress_rule_init(&direct_rule);
direct_rule.action = CAPSID_EGRESS_ALLOW;
direct_rule.target = "api.example.com";
direct_rule.port_start = 443;
direct_rule.port_end = 443;
direct_rule.rule_id = 2001;

capsid_egress_policy direct;
capsid_egress_policy_init(&direct);
direct.rules = &direct_rule;
direct.rule_count = 1;

capsid::CapabilityPolicyBuilder capability;
capability.net(
    CAPSID_EGRESS_ALLOW,
    "api.example.com",
    443,
    443,
    2002);

const capsid_capability_policy &descriptor =
    capability.descriptor();

capsid_worker_config config;
capsid_worker_config_init(&config);
config.egress_policy = &direct;
config.capability_policy = &descriptor;
```

When both policies exist, the intersection applies. Every request checks the
initial hostname, DNS-resolved addresses, and redirects. Protected addresses
such as loopback, private, and link-local also need explicit IP/CIDR allows.

## JavaScript permission query

First authorize the query module:

```cpp
capability.allow_module("capsid:permissions");
```

Then the application can query the effective state:

```js
import { permissions } from "capsid:permissions";

permissions.query({
  name: "net",
  host: "api.example.com",
  port: 443,
});

permissions.query({
  name: "read",
  path: "/srv/capsid/orders/config.json",
});

permissions.query({
  name: "env",
  variable: "APP_MODE",
});

permissions.query({
  name: "sys",
  kind: "runtimeVersion",
});

permissions.query({
  name: "stdio",
  stream: "stdout",
});

permissions.query({
  name: "storage",
  namespace: "tenant-a",
});
```

Queries return `"granted"`, `"denied"`, `"partial"`, or `"unavailable"`. The
supported descriptor fields are:

| `name` | Resource field |
| --- | --- |
| `read`, `write`, `ffi` | `path` |
| `net` | `host` and `port`, both required |
| `env` | `variable` |
| `sys` | `kind` |
| `stdio` | `stream` |
| `storage` | `namespace` |
| `engine` | `operation` |
| `rawSocket` | None |

Only `net` supports omitting both `host` and `port` to query the aggregate
status of the two-layer network policy; in that case `"partial"` may be
returned. Other permissions should provide the resource field shown in the
table. Queries do not request permission and do not change the result of later
operations.

## Global APIs that do not go through capability rules

The fixed Web profile includes the following categories that need neither a
module allowlist nor operation rules:

- Request, Response, Headers, URL, Streams, Encoding, Events;
- timers, queueMicrotask, structuredClone, Compression;
- Web Crypto and random numbers;
- WebAssembly within fixed limits;
- `console.*`.

Among these, `console.*` produces bounded `CAPSID_EVENT_LOG` events, but it is
not equivalent to `capsid:stdio.write()`: console is part of the fixed Web
profile, while `capsid:stdio` is a stdout/stderr channel that requires an
explicit module + stream rule. The host still needs to keep draining or dropping
log events and handle redaction and rate limiting itself.

Global `fetch()` is the only standard Web API that directly connects to the
capability/egress policy.

## Errors and audit

Module rejections fall into three categories:

- `module is forbidden`: the specifier is permanently forbidden; no
  configuration can open it;
- `module is unavailable`: the current build does not have the module or the
  specifier is unknown;
- `module is not authorized`: the module is built but is not in this worker's
  allowlist.

Module, operation, and query decisions all produce `CAPSID_EVENT_AUDIT`. Hosts
can use `capsid_audit_record_decode()` to read the stage, decision, rule ID,
application identity, resource, and manifest hash.
