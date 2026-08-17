# Linux strict sandbox

> For a cross-platform capability overview and selection guidance, see [Platform Support Overview](platform-support.md).

The Linux sandbox is configured by the host only through the C ABI; it is not exposed as a JavaScript global, module, or permission prompt.

## Strict baseline

All workers close unrelated inherited fds before initializing txiki.js and start with an explicitly empty environment. Strict mode also closes inherited stdin/stdout/stderr; application logs continue to go through FetchRPC. The bundle is parsed only after the HELLO checksum and sandbox installation complete.

`strict_sandbox = 1` requires every item in `CAPSID_SANDBOX_FEATURE_STRICT_BASE` to succeed:

- rlimit;
- `no_new_privs`;
- default-deny Landlock filesystem rules;
- seccomp BPF syscall allowlist.

Landlock opens only resolver/hosts configuration, system CAs, timezone data, the kernel random device, and the explicit `tls_ca_bundle_path` read-only. The application bundle always stays in memory.

seccomp allows the process memory, event loop, DNS, TLS, and IPv4/IPv6 stream/datagram operations required by txiki.js standard `fetch()`; it denies listeners, Unix/raw sockets, process/thread creation, exec, ptrace, namespace/mount changes after sandbox installation, filesystem writes, executable mappings, and kernel interfaces such as key/BPF/perf. Because thread creation is denied after installation, the libuv work pool that `fetch()` hostname pre-resolution runs on is warmed — and the system resolver primed — before the sandbox installs.

Strict mode currently requires Linux x86-64/AArch64 with usable Landlock and seccomp. If any mandatory feature is missing, startup fails; READY cannot be reported in a partially strict mode. Requesting strict mode on other platforms also fails closed.

## Optional isolation

`sandbox_required_features` can require:

- user namespace;
- private mount namespace;
- IPC namespace;
- UTS namespace;
- cgroup v2 membership;
- entering a host-preconfigured network namespace.

When namespaces are required, the runtime first establishes a user namespace; all namespace setup happens before Landlock/seccomp.

### cgroup v2

`sandbox_cgroup_path` must be an existing, absolute, delegated cgroup v2 directory. The host is responsible for creating the directory and enabling controllers at the parent level; the runtime does not modify `cgroup.subtree_control`.

ABI v7 `capsid_resource_limits.enabled_fields` distinguishes "unset" from "explicitly zero". Supported fields:

- `file_descriptors` → `RLIMIT_NOFILE`;
- CPU quota/period → `cpu.max`;
- CPU weight → `cpu.weight`;
- memory high/max/swap max;
- PID max.

`CAPSID_RESOURCE_UNLIMITED` and `CAPSID_RESOURCE_PIDS_UNLIMITED` write the kernel `max`. The runtime saves old values, writes each item, and reads back; any failed step triggers a best-effort rollback in reverse order, and it verifies before HELLO that the child PID has entered the target cgroup. Directory cleanup is the host's responsibility.

### Network namespace

`sandbox_network_namespace_fd` accepts a host-configured Linux network namespace fd. A non-negative fd requires strict mode and implicitly requires `CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE`.

The runtime validates the fd type, compares the inode after entering the namespace, and leaves caller fd ownership unchanged. The host is responsible for veth, route, DNS, firewall/NAT, and namespace lifecycle; the runtime does not set up networking for the host.

## Egress network policy

`egress_policy == NULL` means deny-all. Rule targets support:

- exact ASCII hostname;
- single-label wildcards of the form `*.example.com`;
- numeric IP;
- canonical IPv4/IPv6 CIDR.

Deny always takes precedence. Even when `default_action` is allow, protected addresses such as loopback, link-local, private/unique-local, metadata-adjacent, multicast, unspecified, and documentation still require an explicit CIDR allow.

The policy checks the original hostname, the actual connect address chosen after DNS, and every redirect. A hostname allow cannot bypass DNS rebinding protection. If the capability policy also provides `net_policy`, both must allow.

CA bundle and request/response body limits are host configuration and are not exposed to JavaScript. Certificate chain and hostname verification are not disabled by a custom CA.

## Explicit limitations

- Allowing standard Fetch means seccomp cannot forbid all socket syscalls;
- Landlock is not a network boundary; a network namespace/firewall provides stronger network isolation;
- cgroup/namespace prerequisites are provided by the deployment environment; the runtime does not acquire extra privileges;
- strict sandbox is not a multi-tenant scheduler and does not replace the host's worker pool and auditing;
- capability policy and the OS sandbox complement each other and cannot replace each other.

## Testing and CI

Process tests cover strict enforcement, fd hygiene, namespace, cgroup controller write/read-back/rollback, network namespace inode, direct HTTP/HTTPS Fetch, and custom CA.

When ordinary hosts lack delegation or namespace permissions, the corresponding tests return CTest skip 77. This is not positive evidence. The hosted validity workflow runs `scripts/run-delegated-sandbox-tests.sh` in a `--privileged --cgroupns=private` container and treats any 77 as a failure.

For the full test layering and commands, see [Testing and Continuous Gate](testing.md).
