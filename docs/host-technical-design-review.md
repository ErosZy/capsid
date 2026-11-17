# First-party Host Architecture and Review Boundary

Reviewed 2026-11-17 against the published beta.2 source. This page describes the
implemented Host, not a pending M1/M2 work queue. The original milestone design
and all checkpoints are preserved in the historical snapshot linked below.
Use [current status](current-status.md) for source, release, test and data identities.

## Responsibility Split

| Layer | Owns | Does not imply |
| --- | --- | --- |
| Runtime/C ABI v7 | Worker creation, IPC, bounded queues, request credit, lifecycle and OS sandbox installation | An HTTP listener, deployment policy or tenant scheduler |
| First-party Host | HTTP admission/routing, worker pools, Upgrade framing, outbound channel dialer, generation rollout, Admin API and metrics | A stable deployment interface or a complete external gateway |
| Application | A self-contained ESM Fetch handler and explicitly authorized capability calls | Ambient filesystem/environment, a raw socket, listener or process |
| Deployment | TLS/gateway policy, DNS/network namespace/firewall, delegated resource roots, secrets and host hardening | Isolation merely because a process started |

[Host embedding](host-integration.md) defines the reusable C/C++ interface;
[configuration](host-config.md) defines the executable Host input. Host behavior
must not be inferred from an old design checkbox.

## Delivered Modes

| Mode | Data plane | Configuration/authority | Platform boundary |
| --- | --- | --- | --- |
| `single-worker` | One persistent worker | Explicit CLI and optional local `capsid.json` | Trusted local integration on Linux/macOS/Windows |
| `static-pool` | Fixed worker/shard pool and one public listener | Local application authority; explicit worker count | Native Windows uses a shared acceptor, not SO_REUSEPORT |
| `managed` | Linux coordinator, immutable generation snapshots and supervised pools | `host.json` maximum intersected with per-version application requests | Linux strict sandbox prerequisites; deployment API remains experimental |

Local modes with sandbox off are not production isolation. They remain
loopback-only and retain configuration/registry safety checks. A Binding grant
uses the same validated package registry and policy intersection in local mode,
but managed secrets and rollout services are not thereby available.

## HTTP and Worker Lifecycle

A worker belongs to one serialized owner loop. The Host loads only an audited
self-contained source bundle or matching trusted bytecode, then waits for READY
and verifies required sandbox/compatibility proof before scheduling requests.
It keeps request, response, deadline, cancellation and byte-credit state per ID.

A response head is forwarded without waiting for a complete streaming body.
Credit is returned after actual downstream consumption; a global queue limit
cannot replace per-request backpressure. Slow clients, client disconnect,
worker exit, timeout and protocol errors have explicit bounded cleanup. A
synchronous CPU timeout or poisoned worker requires replacement, not reuse.

Platform-specific process/event-source operations stay behind adapters. POSIX
uses a Unix socket fd; Windows uses the ABI's CRT fd backed by loopback TCP and
native process/reap mechanics. Business routing/pool code must not treat a
Windows fd as a Unix descriptor or claim Linux security feature bits.

## v4 Channels and WebSocket

The legacy ABI entry remains FetchRPC v3. Channel-aware hosts use the separately
sized protocol descriptor and explicitly verify effective v4 capabilities.
Matching first-party peers implement A2 layout, confirmation, final Close,
Upgrade-fetch and connection-lifetime extensions. Old peers are not silently
upgraded or downgraded.

Inbound Upgrade uses an admitted HTTP request and Host-assigned channel ID.
WebSocketPair/101 commits the independent connection; application code never
owns the listener or raw RFC 6455 socket. The Host handles frames, masking,
fragmentation, control traffic, transport validation and network write budgets.

Outbound ws/wss uses a Host-resident resolver/TCP/TLS/handshake dialer with the
application's effective egress authority. ACCEPT remains provisional until the
actual result ACK; a phase notification is not proof that ACCEPT is queued.
CANCEL/transport Close stops the real producer and drains its I/O before ACK.
Queue refusal cannot consume IDs, credit or confirmation state.

Physical channels, pending dials, queued bytes and retained incoming owners are
separate bounded resources. A retained local owner after incoming EOF is not a
live network reservation; managed generation liveness accounts for both
atomically, and its original drain deadline still overrides retention.
See the [wire contract](fetchrpc-v4.md) and
[pinned semantics](workerd-websocket-semantics.md) for precise behavior and
explicit compatibility/resource differences.

## Managed Generations and Readiness

The coordinator validates the complete Host/App intersection, bundle identity,
Binding/secret snapshot and capacity before staging a generation. A failed
warm-up/health/commit operation must not silently replace a serving version.
Committed generation artifacts are immutable inputs for replacement/recovery.

Admin API and disk state have distinct roles. A successful deploy POST returns
an operation identity; 202 is submission, not serving readiness. Query that
exact operation and expected version within the existing deadline. Require its
terminal active state before the expected first data-plane request. A durable
active document may precede routing publication, so disk status or Host READY
alone is insufficient. Do not conceal rollout failure through deploy/business
retries. The Admin API is local same-euid Unix-socket authority, not public HTTP.

For measurements, the Host owns bind(0) continuously and the controller proves
that child's actual listener identity. Selecting and releasing a free port and
then asking the Host to bind is not an ownership guarantee.

## Security and Bindings

Strict sandboxing is Linux-only and must be verified at READY. User authority,
Binding authority and process-wide seccomp/Landlock unions are distinct.
Trusted Binding filesystem/network profiles do not grant those resources to
User JavaScript. Native handles, runtime origins and Binding IDs remain checked
at every relevant operation; no shared JavaScript heap or native handle crosses
the runtime boundary.

The C API defaults to trusted operation: strict sandbox is off and a null direct
Fetch egress policy denies Fetch. Unknown modules, schemas, fields, profiles,
invalid resources and forbidden `tjs:*` names fail closed. The Host retains
ownership, reparse/symlink, file-size and identity checks appropriate to each
platform; local development is not a reason to disable them.

[Capability policy](capability-policy.md), [Binding design](binding-technical-design.md),
[Linux sandboxing](linux-sandbox.md) and [security](../SECURITY.md) are the
normative security references.

## Validation and Performance Boundary

The published beta source passed its declared full/sanitizer/native/fuzz/soak
and actual-package acceptance. Known raw WPT failures and genuine skips remain
visible; neither full standards conformance nor every historical causal issue
is claimed closed. Native Windows/macOS integration is separate from production
isolation. See [qualification](v0.3.0-current-qualification.md).

The benchmark controllers are implemented and their latest accepted local
three-lane session belongs to measured snapshot `485b29b`. It is not beta.2
performance evidence. Diagnostic samples, an implementation milestone, selected
CTest success or a matching source tree cannot stand in for the declared
whole-session protocol and binary/source identity. See
[performance methodology/data](performance-benchmarks.md).

## Historical source and preserved anchors

The original dated checkpoints and their numbers are preserved byte-for-byte in the
[captured document](history/archives/2026-11-17/host-technical-design-review.md.txt). They are not current release instructions.

<details>
<summary>Legacy section links</summary>

<a id="capsid-host-v1-detailed-design"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1) — capsid-host-v1-detailed-design

<a id="1-document-scope-and-frozen-conclusions"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L7) — 1-document-scope-and-frozen-conclusions

<a id="3-key-contract-additions"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L62) — 3-key-contract-additions

<a id="31-product-boundary"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L64) — 31-product-boundary

<a id="32-two-layer-hostjson--capsidjson-model"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L68) — 32-two-layer-hostjson--capsidjson-model

<a id="33-fetch-scheme"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L91) — 33-fetch-scheme

<a id="34-secret-semantics"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L97) — 34-secret-semantics

<a id="35-trusted-bytecode-trust-chain"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L113) — 35-trusted-bytecode-trust-chain

<a id="36-deployment-vertical-loop"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L174) — 36-deployment-vertical-loop

<a id="37-listener-configuration"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L185) — 37-listener-configuration

<a id="38-whole-machine-resource-limits"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L191) — 38-whole-machine-resource-limits

<a id="39-defaults-must-be-calibrated-by-evidence"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L204) — 39-defaults-must-be-calibrated-by-evidence

<a id="310-no-config-startup"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L210) — 310-no-config-startup

<a id="311-capsidstorage-is-not-shared-persistent-storage"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L223) — 311-capsidstorage-is-not-shared-persistent-storage

<a id="312-host-config-reload-deferred"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L227) — 312-host-config-reload-deferred

<a id="4-target-architecture"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L231) — 4-target-architecture

<a id="41-processes-and-threads"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L259) — 41-processes-and-threads

<a id="42-ownership-rules"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L271) — 42-ownership-rules

<a id="43-why-asiobeast-instead-of-raw-epoll"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L285) — 43-why-asiobeast-instead-of-raw-epoll

<a id="5-configuration-scheme"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L295) — 5-configuration-scheme

<a id="51-revised-hostjson-outline"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L297) — 51-revised-hostjson-outline

<a id="52-app-configuration"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L396) — 52-app-configuration

<a id="53-parsing-and-validation"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L433) — 53-parsing-and-validation

<a id="6-version-snapshot-and-persistent-state"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L453) — 6-version-snapshot-and-persistent-state

<a id="61-why-activejson-is-enough"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L455) — 61-why-activejson-is-enough

<a id="62-state-directory"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L506) — 62-state-directory

<a id="63-generation-identity"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L538) — 63-generation-identity

<a id="64-secure-copy"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L554) — 64-secure-copy

<a id="65-atomic-activejson-state-switchover"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L570) — 65-atomic-activejson-state-switchover

<a id="66-secret-snapshot-read"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L592) — 66-secret-snapshot-read

<a id="7-deployment-retire-api-and-state-machine"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L614) — 7-deployment-retire-api-and-state-machine

<a id="71-admin-surface"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L616) — 71-admin-surface

<a id="72-state-machine"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L656) — 72-state-machine

<a id="73-concurrency-and-idempotency"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L701) — 73-concurrency-and-idempotency

<a id="74-health-checks"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L714) — 74-health-checks

<a id="75-drain"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L734) — 75-drain

<a id="76-explicit-retire"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L746) — 76-explicit-retire

<a id="8-routing-and-http-boundary"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L763) — 8-routing-and-http-boundary

<a id="81-one-listener-one-routing-mode"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L765) — 81-one-listener-one-routing-mode

<a id="82-worker-observable-url-and-proxy-headers"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L784) — 82-worker-observable-url-and-proxy-headers

<a id="83-http1-security-rules"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L815) — 83-http1-security-rules

<a id="84-expect-100-continue"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L834) — 84-expect-100-continue

<a id="9-requestresponse-credit-mapping"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L840) — 9-requestresponse-credit-mapping

<a id="91-request-direction"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L842) — 91-request-direction

<a id="92-response-direction"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L857) — 92-response-direction

<a id="93-sse-and-streaming"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L871) — 93-sse-and-streaming

<a id="94-request-id"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L885) — 94-request-id

<a id="10-worker-pool-and-scheduling"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L889) — 10-worker-pool-and-scheduling

<a id="101-shard-local-pool"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L891) — 101-shard-local-pool

<a id="102-choosing-a-worker"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L903) — 102-choosing-a-worker

<a id="103-admission-control"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L915) — 103-admission-control

<a id="104-automatic-retry"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L938) — 104-automatic-retry

<a id="105-worker-crash-replacement-and-generation-quarantine"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L951) — 105-worker-crash-replacement-and-generation-quarantine

<a id="11-permission-compilation-and-isolation"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L980) — 11-permission-compilation-and-isolation

<a id="111-compilation-outputs"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L982) — 111-compilation-outputs

<a id="112-file-paths"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L999) — 112-file-paths

<a id="113-network"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1008) — 113-network

<a id="114-cgroup-hierarchy-and-capacity"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1019) — 114-cgroup-hierarchy-and-capacity

<a id="115-external-isolation-boundary"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1038) — 115-external-isolation-boundary

<a id="12-observability"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1044) — 12-observability

<a id="121-metrics"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1046) — 121-metrics

<a id="122-structured-logs"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1064) — 122-structured-logs

<a id="13-runtime-integration-requirements"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1085) — 13-runtime-integration-requirements

<a id="131-implemented-structured-buildcompatibility-identity"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1087) — 131-implemented-structured-buildcompatibility-identity

<a id="132-p1-structured-errors"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1111) — 132-p1-structured-errors

<a id="133-p1-non-blocking-lifecycle"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1125) — 133-p1-non-blocking-lifecycle

<a id="14-build-and-dependency-governance"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1135) — 14-build-and-dependency-governance

<a id="141-residual-risk-of-the-public-c-host"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1164) — 141-residual-risk-of-the-public-c-host

<a id="15-testing-and-acceptance"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1177) — 15-testing-and-acceptance

<a id="151-tdd-is-the-global-delivery-rule"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1179) — 151-tdd-is-the-global-delivery-rule

<a id="152-unit-and-property-tests"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1192) — 152-unit-and-property-tests

<a id="153-http-and-flow-control-integration"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1206) — 153-http-and-flow-control-integration

<a id="154-deployment-fault-injection"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1223) — 154-deployment-fault-injection

<a id="155-trusted-bytecode-and-secret"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1242) — 155-trusted-bytecode-and-secret

<a id="156-isolation-tests"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1260) — 156-isolation-tests

<a id="157-performance-acceptance"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1270) — 157-performance-acceptance

<a id="current-evidence-boundary"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1272) — current-evidence-boundary

<a id="profile-gate-for-each-performance-slice"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1276) — profile-gate-for-each-performance-slice

<a id="m1-perf-minimal-single-worker-host-ab-checkpoint"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1293) — m1-perf-minimal-single-worker-host-ab-checkpoint

<a id="158-release-gate"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1317) — 158-release-gate

<a id="16-implementation-order"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1330) — 16-implementation-order

<a id="m0-executable-contracts"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1334) — m0-executable-contracts

<a id="m1-artifacts-secrets-and-single-worker-loop"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1348) — m1-artifacts-secrets-and-single-worker-loop

<a id="m2-static-pool-and-reliable-deployment-loop"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1383) — m2-static-pool-and-reliable-deployment-loop

<a id="m3-production-v1-release-gate"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1397) — m3-production-v1-release-gate

<a id="m4-data-driven-follow-up-capabilities"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1409) — m4-data-driven-follow-up-capabilities

<a id="17-confirmed-decisions"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1421) — 17-confirmed-decisions

<a id="18-external-selection-references"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1434) — 18-external-selection-references

<a id="19-final-recommendation"></a>
[Historical section](history/archives/2026-11-17/host-technical-design-review.md.txt#L1447) — 19-final-recommendation

</details>
