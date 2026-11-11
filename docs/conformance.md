# Standards and Conformance

<!-- capsid-current-qualification-20261110 -->
本页后续历轮内容保留原记录身份。当前兼容边界、冻结输入、原始失败和封板开放项见[当前验收与边界](v0.3.0-current-qualification.md)。当前源码版本为 0.3.0 候选，v0.3.0 尚未封板或发布。

部分历史产物已按用户授权清理；[证据索引](evidence/v0.3.0/2026-11-10/index.json)及[可用性审计](evidence/v0.3.0/2026-11-10/historical-evidence-availability.json)说明时点与范围。旧章节的“当前”“仍保留”不表示本轮重新验证了已删除文件。
<!-- /capsid-current-qualification-20261110 -->


Target profile: `CAPSID-MIN-2025-subset-v0`. This document combines the standard source lock, conformance deviations, and capability tracking matrix; the machine-readable WPT selection is [`tests/wpt/manifest.json`](../tests/wpt/manifest.json). Test execution is described in [Testing and Continuous Gates](testing.md).

## 1. Standard Source Lock

### Specification Baseline

- Standards body: ECMA TC55 (WinterTC), Web-interoperable Runtimes Community Group;
- Standard: ECMA-429 *Minimum common web API*, first edition, December 2025;
- Capsid profile: `CAPSID-MIN-2025-subset-v0` (a versioned, testable subset of
  ECMA-429; full conformance is not claimed outside the profile);
- published document:
  `https://ecma-international.org/wp-content/uploads/ECMA-429_1st_edition_december_2025.pdf`;
- PDF SHA-256:
  `9f8abe3fa86517675cb8388b8b2b3a4024bb6d5d9e3467b89ae4013d20ae30b5`;
- editorial source referenced when locking: `WinterTC55/proposal-minimum-common-api` commit `fe94bc2b0e349d7aae635c27c653b5165039ab66`.

The online editor draft is for reference only and cannot silently replace the published version. Living standards referenced by ECMA-429 are made reproducible through a pinned WPT revision; moving the WPT commit is itself a conformance update requiring review.

### WinterTC Alignment

Capsid follows the WinterTC runtime model: applications export a Web-standard
`fetch(request)` handler and receive `Request`/`Response`, Web Streams, `URL` and
`URLPattern`, Text Encoding, Web Crypto, timers and microtasks, `AbortController`,
`MessageChannel`/`MessagePort`, and WebAssembly through the profile listed above.
The profile is enforced by the restricted build and is evidenced by the pinned
WPT batch plus process-level contract tests. Anything outside the profile (for
example a Node-style server API) is an intentional product boundary, not a hidden
conformance gap; see the capability tracking matrix below.

### WPT Lock

- Repo: `https://github.com/web-platform-tests/wpt.git`;
- commit: `1985b47aa8972a970f005957f2bfa036da1787c6`;
- exact path: `tests/wpt/manifest.json`;
- branch names cannot be build or CI inputs;
- test files and transitive resources must come from the same commit;
- WPT checkout is only a test input and is not linked into the runtime.

The current profile executes the 84 paths in `executedProfile` in the manifest. Each file runs in an independent worker/realm after being combined with the project adapter. CMake rejects checkouts whose `HEAD` does not equal the pinned commit. Build and CTest source-integrity gates also compare the actual selected files and their declared transitive inputs byte-for-byte against that commit, including the official test harness used by adapter controls. A matching `HEAD` alone is insufficient.

HTML inputs extract only the original inline script; assertions are not modified. Each bundle uses the fixed WPT URL as the logical source name to make error location easier.

The document-scoped harness in `promise-rejection-events.html` is not suitable for the current one-file-per-worker-realm model, so the manifest lists it as `notExecuted`. The worker support source runs with two declared supplemental callback-count assertions: each affected delayed-handling case must observe exactly one matching `rejectionhandled` callback. All upstream statements and reason assertions remain unchanged. The manifest records this augmentation, and byte-restoration controls verify its scope. It is an augmented upstream run, not an unmodified standards-conformance result; its default-profile reason disagreements still fail.

### Evidence Layers

1. `worker_p1_platform_contract`: the project's own process-level regression, not marked as WPT;
2. adapted WPT: preserves upstream assertions and metadata; the adapter only provides harness, pinned resources, and result transport;
3. host integration: covers IPC, lifecycle, network, resource limits, and the C ABI.

Passing the first layer cannot imply passing the second. The capability matrix records them separately.

The adapter supports the sync/async/promise test primitives required by the current selection. Any unsupported harness capability must fail the batch, not silently skip. Test-only `location.href`, the pinned resource map, and rejection triggers exist only in the test realm and do not enter the product surface.

Reviewed mechanical adaptations include:

- classic-script resources are concatenated in fixed order before ESM bundling;
- helper tails that would create duplicate declarations after concatenation are removed;
- the rest parameter `arguments`, illegal in strict ESM, is renamed to `importArguments`;
- under CAPSID-D009, a problematic QuickJS Proxy constructor probe is replaced with an equivalent `Reflect.construct` probe.

Expected failures must specify path, subtest, registered deviation, error class,
and the exact first diagnostic line. Only an assertion during the test phase can
use a waiver; timeout, cleanup failure, an unrelated exception, an unexpected
pass, or a changed diagnostic fails the result. Reports retain the complete
diagnostic and status of accepted failures. Native report validation checks the
top-level success fields and empty failure arrays, rejects duplicate keys and
invalid JSON scalars, and requires a positive subtest count.

The adapter's numeric, array and exception assertions have independent controls
against the locked upstream `resources/testharness.js`. These controls detect
NaN comparisons, sparse-array differences, error subclass mismatches and nested
assertion failures that previously could report success. This comparison covers
the exercised helper contracts, not every upstream harness capability.

### Selection and Exclusion

- Only APIs that belong to ECMA-429 and are included in this profile are in scope;
- if an in-profile test does not run, it must reference a deviation or an open gap ID;
- harness incompatibility is work to fix, not a semantic expected failure;
- out-of-profile APIs such as Window, Document, ServiceWorker, and WASI do not need a deviation ID;
- tentative tests are not selected by default unless there is a clear, stable profile need;
- tests must not be edited to accommodate current txiki.js behavior;
- network tests must use deterministic local fixtures; the public internet is not a conformance dependency.

### Update Process

Updates must record old/new commits, review all selected file changes, run the full contract/WPT/integration matrix, confirm vendor clean, and sync the capability matrix and deviation table. CI must reject WPT checkouts inconsistent with the manifest commit.

## 2. Conformance Deviations

This table distinguishes actively accepted profile exclusions from implementation gaps that have already been closed. Open implementation bugs cannot be treated as supported deviations and also block the conformance claim for the corresponding capability.

| ID | Capability | Classification | Current behavior | Impact | Exit criteria |
| --- | --- | --- | --- | --- | --- |
| CAPSID-D001 | `WebAssembly.Tag`, `WebAssembly.Exception`, `WebAssembly.JSTag` | Accepted profile exclusion | The pinned WAMR/txiki combination does not expose exception-handling JS interfaces. | Wasm depending on the exception-handling proposal is unsupported; full ECMA-429 Wasm conformance must not be claimed. | Adopt an engine/configuration with the required semantics and pass pinned Wasm JS API tests. |
| CAPSID-D002 | WebAssembly fixed-width SIMD | Accepted profile exclusion | `WAMR_BUILD_SIMD=0`; the process contract confirms SIMD modules are rejected by `WebAssembly.validate()`. | SIMD modules fail validation or compilation. | Enable SIMD without uncontrolled dependency downloads, pass pinned tests, and publish a new profile version. |
| CAPSID-D003 | Console Standard | Closed implementation gap | Method names, representative operations, and `console-is-a-namespace.any.js` pass. | No known gap in the selected batch. | Closed 2026-07-25; reopen if extended tests expose a semantic gap. |
| CAPSID-D004 | `Performance` interface | Open verification gap | A bounded diagnostic captured a failing HR-Time run with Date minus performance elapsed time of 1972.974 ms and an independently sampled realtime/monotonic offset jump of 1973.158 ms inside that same process window (difference about 0.18 ms). Six earlier windows without such jumps passed. | This establishes an environment clock cause for that captured failure, not every historical failure or stable behavior on all targets. The original upstream assertion and failures remain intact. | Run in a clock-stable environment and retain timestamped evidence; do not increase tolerance or retry a failed CTest into a passing result. |
| CAPSID-D005 | Error and rejection reporting | Open upstream lifecycle gap | The selected support source retains all upstream statements. Two delayed-handling cases additionally require exactly one matching rejectionhandled callback before cleanup; native missing-listener controls now fail. Their original reason assertions still disagree with the selected default workerd profile (D012), so the support-source CTest remains failing. The document-scoped lifecycle HTML remains unexecuted. | Supplemental observation checks and project differential tests do not establish upstream standards conformance or replace the document-scoped harness. | Resolve the profile boundary, execute the upstream lifecycle harness, and retain independent event-presence and differential controls. |
| CAPSID-D006 | TextDecoder legacy multibyte encoding | Closed implementation gap | GBK, GB18030, Big5, EUC-JP, EUC-KR, ISO-2022-JP, and Shift_JIS are implemented by project standard state machines and compact indexes; the selected decode/stream/EOF/fatal corpus passes. | No known gap in the selected Encoding batch. | Closed 2026-07-25; reopen if an extended corpus exposes a gap. |
| CAPSID-D007 | Compression Streams brotli | Accepted profile exclusion | Only gzip, deflate, and deflate-raw are supported; `brotli` is rejected. | Applications depending on Compression Streams brotli are unsupported. | Implement brotli without adding ambient capability, pass pinned tests, and revise the profile. |
| CAPSID-D008 | WebAssembly shared memory/threads | Accepted profile exclusion | `WAMR_BUILD_SHARED_MEMORY=0`; shared Memory lacks compliant grow semantics, and the corresponding precise WPT is an expected failure. | Wasm thread/shared linear memory applications are unsupported; non-shared behavior passes the selected corpus. | Enable WAMR shared memory and host primitives, remove the expected failure, and pass pinned tests. |
| CAPSID-D009 | QuickJS Proxy constructor probe | Provisional engine deviation | QuickJS rejects the standard Proxy-based `IsConstructor` probe even when the target is constructible; the Encoding IDL harness uses an equivalent `Reflect.construct` and continues checking all interface constructors. | User code relying on that Proxy constructibility pattern observes non-standard behavior. | Fix/upgrade QuickJS, restore the original probe, and pass the pinned IDL harness. |
| CAPSID-D010 | MessagePort queue starts before `start()` | Accepted workerd profile deviation | The pinned workerd default and Capsid both deliver queued messages before `start()`, as the mandatory API fixture verifies. The WHATWG standard requires an initially disabled queue. After listener alignment, the upstream initial-disabled test encounters the D012 rejection of capture:true before reaching its queue assertion. | The old exact `Untitled test` assertion waiver is not matched: the TypeError remains a failing CTest. This run is not evidence that the upstream queue assertion executed. | Resolve the standards/default-workerd profile boundary while preserving the original source, exact failure checks and independent queue observations. |
| CAPSID-D011 | GB18030 four-byte range mapping | Closed implementation gap | The former four-byte fallback depended on platform conversion tables: Linux mapped 18 WHATWG pointers to private-use characters, while native Windows and the locked workerd returned the standard code points. Decoding now uses all 207 manifest-pinned WHATWG ranges, the U+E7C7 exception, and a content-locked 23,940-entry WHATWG two-byte index. A fresh Managed Worker also uses this index: the former platform converter reported host encoding is unavailable there. All 18 expected-failure entries are removed without changing upstream assertions. | The selected 275-case upstream GB18030 batch and exact workerd observations cover these mappings; full standards conformance is not implied. | Keep the pinned range-generation audit, streaming/error/reset cases, and exact four-byte/two-byte pointer comparisons across Linux and Windows. |
| CAPSID-D012 | Default workerd event behavior | Accepted workerd profile deviation | The v0.3.0 development profile follows the locked workerd default: currentTarget persists after dispatch, event timeStamp is zero, isTrusted is a configurable prototype getter, default ErrorEvent.error and CustomEvent.detail are undefined, MessagePort message source is the receiving port, and rejectionhandled omits reason. The 105 public event cases, 66 listener cases and 259 event-handler attribute inputs also freeze synchronous exception propagation, rejection of true capture/passive options, registration-time handleEvent capture, retained stop flags and cancellation by an exactly true return value. | The exact currentTarget assertion in EventTarget-constructible.any.js is retained as an expected failure. These are declared default-profile behaviors, not full DOM/HTML standards conformance. | Revisit when the locked default changes or a separately versioned standards profile is implemented; retain the upstream assertions and four-backend observations. |

For D010, the [WHATWG MessagePort definition](https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports)
requires an initially disabled port message queue. The previous description that
called this WPT stale was incorrect. The workerd compatibility profile retains
the observed default behavior; it does not claim conformance to that requirement.
The latest run also retains the earlier capture:true registration failure in
that upstream file. Its TypeError is not substituted for the D010 assertion
waiver; the same generated source reproduces the registration failure on the
locked reference and all three Capsid modes. See the
[listener audit](v0.3.0-glm-takeover-review.md#22-2026-10-15-监听器回归补齐与-tdd-修正).

### Deployment Resource Policy

The following limits reject some spec-valid workloads, so deployers must publish them, but they are not new JavaScript APIs:

- Wasm linear memory is capped at 256 pages (16 MiB) and table at 1024 elements;
- `max_fetch_request_body_bytes` / `max_fetch_response_body_bytes` can limit aggregate egress Fetch bodies; default `0` means no additional total limit is imposed;
- strict sandbox, namespace, cgroup, CPU/memory/swap/PID/fd can cause startup or workload failure.

WASI, Hono/Workers `env`/`ExecutionContext`, txiki `tjs:*`, process, raw socket, HTTP server, FFI, SQLite, and Capsid's own read-only file module are product capability boundaries, not ECMA-429 conformance deviations.

## 3. Capability Tracking Matrix

A capability can only be marked complete when both its conformance test and process integration test pass.

| Capability group | Spec/conformance evidence | Process evidence | Status |
| --- | --- | --- | --- |
| Global surface and txiki isolation | versioned profile manifest | `worker_global_surface`, module denial, `worker_p1_platform_contract` | Selected surface and isolation pass |
| Event and rejection reporting | 3 EventTarget files, `reportError`, PromiseRejectionEvent, and the unexecuted document-scoped rejection lifecycle | `worker_p1_platform_contract` and mandatory workerd API differential | Selected WPT batch includes the D012 currentTarget failure; 66 listener cases match the reference, but upstream lifecycle evidence remains open under D005 |
| Timer and microtask | 2 timer files, 1 `queueMicrotask` file | `worker_p1_platform_contract` | Pass |
| Encoding | 39 pinned files, including Web IDL, stream, legacy multibyte corpus | `worker_p1_platform_contract` | Selected corpus passes; CAPSID-D006 closed |
| URL / URLPattern | URL, URLSearchParams, URLPattern constructors | `worker_p1_platform_contract` | 3/3 files pass, including 893 URL constructor cases |
| Streams / MessageChannel | 4 Streams, 4 MessageChannel/Port files | `worker_p1_platform_contract` and mandatory workerd API differential | 7/8 files pass; initial-disabled fails at capture:true registration before its queue assertion (D010/D012) |
| Blob / File / FormData | Blob, File constructors | `worker_p1_platform_contract` | 2/2 files pass; FormData has process coverage |
| Compression | compression-stream and pinned resources | `worker_p1_platform_contract` | gzip/deflate/deflate-raw pass; brotli is CAPSID-D007 |
| Console | `console-is-a-namespace.any.js` | `worker_p1_platform_contract` | Pass; CAPSID-D003 closed |
| Web Crypto | getRandomValues, randomUUID | `worker_p1_platform_contract` | 2/2 files and digest/random process behavior pass |
| Fetch | 5 Headers/Request/Response files | direct fetch, cancel, HTTP/HTTPS, egress, netns tests | Selected constructor/WebIDL and real egress matrix pass |
| Performance | HR-Time `basic`, `monotonic-clock` | `worker_p1_platform_contract` | D004 reopened: relative elapsed comparison has an unexplained failure; diagnostic reruns are not a fix |
| WebAssembly subset | 12 compile/instantiate/validate, Memory/Table/Global and streaming files | `worker_wasm_minimal` and shared/exported resource regressions | 12/12 files pass; CAPSID-D001/002/008 are accepted exclusions |
| Host capability policy (non-conformance extension) | capability manifest, module contract, policy/audit/parser and fuzz | permissions, utility, env, system, storage, stdio, fs real worker contracts, per-module denial matrix and final binary audit | ABI v7 / policy v2 three-layer gates pass; the twelve `capsid:` modules can be authorized item by item, with known deferred modules and operations staying `unavailable` |

Expected failures require an exact test name, AssertionError diagnostic, test phase and deviation ID. Upstream assertions remain unchanged. The v0.3.0 development workerd profile explicitly accepts D010/D012 differences; this does not turn the corresponding standards failures into conformance passes.

### MessagePort scheduling observations

The locked workerd default delivers MessagePort callbacks through Promise
reactions. Capsid captures the Promise intrinsic for this delivery, including
when an application replaces public Promise or timer methods. The
`native_message_port_order_<mode>` tests compare the same complete program on
the native reference and every supported Host mode. They cover ordering against
Promise reactions, nested port messages, intrinsic overrides, and a rejection
handled through nested port callbacks. These are functional observations with
an unchanged five-second request deadline, not performance measurements or a
claim about all MessagePort features.

The pinned [workerd implementation](https://github.com/cloudflare/workerd/blob/8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f/src/workerd/api/messagechannel.c%2B%2B)
defines the delivery behavior. This differs from the task timing assumed by
four nested-task cases in the selected rejection WPT support source. Their
original ten-second timeouts and standard failures remain visible. The older
two-file default-profile classification below does not accept these additional
timeouts and cannot qualify this scheduling change; the complete standard and
platform qualification remains required.

### Versioned default-workerd evidence

[`workerd-default-profile.json`](../tests/wpt/workerd-default-profile.json) defines
`CAPSID-WORKERD-v1.20261010.1-default-v1` independently of the standard WPT result.
It binds the reference release, source commit, binary digest, compatibility date
and flags, the pinned WPT commit and both original file digests. Its scope is the
36-subtest rejection support source and the one-subtest initial-disabled source.
The original CTests remain failing; no expected-failure entry or upstream reason
assertion is changed by this mapping.

`node tools/workerd-wpt-profile.mjs metadata` checks the declaration. The `audit`
command consumes complete archived LastTest/JUnit/registration evidence and a
validated required workerd gate using the same Host/Worker binaries. It requires
the three exact failure names, test phases, error classes and diagnostic first
lines, as well as the unreached D010 expectation. Cleanup failures, missing events,
timeouts, partial files and additional failures cannot use this classification.
Four-backend API observations independently require the handled event and promise
identity, the original unhandled reason, capture-option rejection and queue
delivery before and after start. The earlier event-presence guards remain required.

A successful audit reports `profile-matched-with-standard-failures`,
`standardCTestFilesPassed:false`, `ordinarySuitePassed:false` and
`standardsConformanceClaimed:false`. It does not rewrite standard reports or claim
complete workerd or standards conformance. CI archives this audit after an ordinary
suite failure while retaining that step's failure and the original full evidence.
`workerd_wpt_profile_metadata` and `workerd_wpt_profile_controls` run on platforms
without a local reference or WPT checkout; those declaration/control tests do not
constitute a live reference comparison.

The current adapted batch executes 84 upstream files in separate realms. It only proves those pinned files, not all of WPT or full ECMA-429 conformance.
## Development WebSocket scope (v0.3.0)

The development profile adds WebSocketPair, WebSocket and CloseEvent. Inbound
101 and User-origin outbound ws/wss have working first-party host paths;
the release remains 0.2.2/ABI v7 until its separate acceptance gates complete.
The [semantics dossier](workerd-websocket-semantics.md) records the locked
reference, tested cases and differences, including strict UTF-8/wire Close
validation, bounded queues, unrequested protocols, and pending/abrupt lifetime
observations. This addition does not constitute a WebSocket WPT certification.
Binding-origin WS, full parameter/event/cleanup matrices and production strict
platform evidence remain unaccepted in the [execution plan](superpowers/plans/2026-10-12-v0.3.0-remediation-and-delivery.md).


### CAPSID-D013: Headers in the locked workerd default profile

For v0.3.0 the default public `Headers` preserves the locked workerd behavior,
as explicitly accepted for this release. The current 20-case diagnostic agrees
with the locked workerd binary in all cases. Five standard expectations differ:
rejection of a non-ByteString value, and live iterator/forEach observation of
append, deletion and insertion before the iterator position. The locked default
uses entry snapshots for these iteration operations.

Retain the original standard assertions and their recorded failures. This is
an explicit compatibility profile difference and does not establish full Fetch
Headers standards conformance. The same 20 cases have also been compared through
the actual public global `Headers` in native QuickJS, with two challenged requests
in each of single-worker, static-pool and managed mode. All native values matched
the locked reference, including the five declared standard differences. This
targeted comparison does not replace the broader WPT and final combined gates.
