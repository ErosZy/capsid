# Standards and Conformance

Target profile: `CAPSID-MIN-2025-subset-v0`. This document combines the standard source lock, conformance deviations, and capability tracking matrix; the machine-readable WPT selection is [`tests/wpt/manifest.json`](../tests/wpt/manifest.json). Test execution is described in [Testing and Continuous Gates](testing.md).

## 1. Standard Source Lock

### Specification Baseline

- Standard: ECMA-429 *Minimum common web API*, first edition, December 2025;
- published document:
  `https://ecma-international.org/wp-content/uploads/ECMA-429_1st_edition_december_2025.pdf`;
- PDF SHA-256:
  `9f8abe3fa86517675cb8388b8b2b3a4024bb6d5d9e3467b89ae4013d20ae30b5`;
- editorial source referenced when locking: `WinterTC55/proposal-minimum-common-api` commit `fe94bc2b0e349d7aae635c27c653b5165039ab66`.

The online editor draft is for reference only and cannot silently replace the published version. Living standards referenced by ECMA-429 are made reproducible through a pinned WPT revision; moving the WPT commit is itself a conformance update requiring review.

### WPT Lock

- Repo: `https://github.com/web-platform-tests/wpt.git`;
- commit: `1985b47aa8972a970f005957f2bfa036da1787c6`;
- exact path: `tests/wpt/manifest.json`;
- branch names cannot be build or CI inputs;
- test files and transitive resources must come from the same commit;
- WPT checkout is only a test input and is not linked into the runtime.

The current profile executes the 84 paths in `executedProfile` in the manifest. Each file runs in an independent worker/realm after being combined with the project adapter. CMake rejects checkouts whose `HEAD` does not equal the pinned commit.

HTML inputs extract only the original inline script; assertions are not modified. Each bundle uses the fixed WPT URL as the logical source name to make error location easier.

The document-scoped harness in `promise-rejection-events.html` is not suitable for the current one-file-per-worker-realm model, so the manifest lists it as `notExecuted`; the corresponding worker support source executes directly, and ordering semantics are supplied by the project contract.

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

Expected failures must be exact to path/subtest and reference a registered deviation; unexpected failures and unexpected passes both fail the result.

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
| CAPSID-D004 | `Performance` interface | Closed implementation gap | `Performance` inherits `EventTarget`; branding, `timeOrigin`, `now()`, `toJSON()`, and the two HR-Time files pass. | No known gap in the selected batch. | Closed 2026-07-25; reopen if extended tests expose a semantic gap. |
| CAPSID-D005 | Error and rejection reporting | Closed implementation gap | `reportError`, `PromiseRejectionEvent`, `unhandledrejection`/`rejectionhandled` identity and task ordering pass. Upstream `promise-rejection-events.html` is not executed because it depends on a document-scoped harness; the manifest lists it as `notExecuted`, and ordering is proven by the project contract. | No known gap in the executed batch; the upstream document harness remains an explicit evidence gap. | Closed 2026-07-25; reopen if supporting that execution model exposes semantic differences. |
| CAPSID-D006 | TextDecoder legacy multibyte encoding | Closed implementation gap | GBK, GB18030, Big5, EUC-JP, EUC-KR, ISO-2022-JP, and Shift_JIS are implemented by project standard state machines and compact indexes; the selected decode/stream/EOF/fatal corpus passes. | No known gap in the selected Encoding batch. | Closed 2026-07-25; reopen if an extended corpus exposes a gap. |
| CAPSID-D007 | Compression Streams brotli | Accepted profile exclusion | Only gzip, deflate, and deflate-raw are supported; `brotli` is rejected. | Applications depending on Compression Streams brotli are unsupported. | Implement brotli without adding ambient capability, pass pinned tests, and revise the profile. |
| CAPSID-D008 | WebAssembly shared memory/threads | Accepted profile exclusion | `WAMR_BUILD_SHARED_MEMORY=0`; shared Memory lacks compliant grow semantics, and the corresponding precise WPT is an expected failure. | Wasm thread/shared linear memory applications are unsupported; non-shared behavior passes the selected corpus. | Enable WAMR shared memory and host primitives, remove the expected failure, and pass pinned tests. |
| CAPSID-D009 | QuickJS Proxy constructor probe | Provisional engine deviation | QuickJS rejects the standard Proxy-based `IsConstructor` probe even when the target is constructible; the Encoding IDL harness uses an equivalent `Reflect.construct` and continues checking all interface constructors. | User code relying on that Proxy constructibility pattern observes non-standard behavior. | Fix/upgrade QuickJS, restore the original probe, and pass the pinned IDL harness. |
| CAPSID-D010 | `MessagePort_initial_disabled` WPT stale | Accepted WPT upstream divergence | The file asserts a newly created port is initially stopped, contrary to the current WHATWG spec; the file itself also marks the case as possibly an unmaintained duplicate. | The single subtest `Untitled test` is an expected failure and does not affect profile capability. | Remove the expected-failure item after WPT upstream fixes and republishes. |
| CAPSID-D011 | GB18030-2022 new code points | Provisional engine deviation | QuickJS/txiki's `TextDecoder` encoding tables are based on GB18030-2005; the 18 code points added by GB18030-2022 (U+9FB4–U+9FBB CJK characters and U+FE10–U+FE19 vertical punctuation mappings) decode differently from WPT expectations. The 18 subtests of `gb18030-decoder.any.js` (`GB18030-2022 19`–`GB18030-2022 36`) are expected failures. | Text depending on GB18030-2022 new code points decodes to replacement mapping characters; all existing GB18030-2005 code points are correct. | Upgrade/fix the QuickJS encoding tables to GB18030-2022, remove expected-failure items, and pass pinned WPT. |

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
| Event and rejection reporting | 3 EventTarget files, `reportError`, PromiseRejectionEvent, and rejection lifecycle | `worker_p1_platform_contract` | Selected batch passes; CAPSID-D005 closed |
| Timer and microtask | 2 timer files, 1 `queueMicrotask` file | `worker_p1_platform_contract` | Pass |
| Encoding | 39 pinned files, including Web IDL, stream, legacy multibyte corpus | `worker_p1_platform_contract` | Selected corpus passes; CAPSID-D006 closed |
| URL / URLPattern | URL, URLSearchParams, URLPattern constructors | `worker_p1_platform_contract` | 3/3 files pass, including 893 URL constructor cases |
| Streams / MessageChannel | 4 Streams, 4 MessageChannel/Port files | `worker_p1_platform_contract` | 8/8 files pass |
| Blob / File / FormData | Blob, File constructors | `worker_p1_platform_contract` | 2/2 files pass; FormData has process coverage |
| Compression | compression-stream and pinned resources | `worker_p1_platform_contract` | gzip/deflate/deflate-raw pass; brotli is CAPSID-D007 |
| Console | `console-is-a-namespace.any.js` | `worker_p1_platform_contract` | Pass; CAPSID-D003 closed |
| Web Crypto | getRandomValues, randomUUID | `worker_p1_platform_contract` | 2/2 files and digest/random process behavior pass |
| Fetch | 5 Headers/Request/Response files | direct fetch, cancel, HTTP/HTTPS, egress, netns tests | Selected constructor/WebIDL and real egress matrix pass |
| Performance | HR-Time `basic`, `monotonic-clock` | `worker_p1_platform_contract` | Pass; CAPSID-D004 closed |
| WebAssembly subset | 12 compile/instantiate/validate, Memory/Table/Global and streaming files | `worker_wasm_minimal` and shared/exported resource regressions | 12/12 files pass; CAPSID-D001/002/008 are accepted exclusions |
| Host capability policy (non-conformance extension) | capability manifest, module contract, policy/audit/parser and fuzz | permissions, utility, env, system, storage, stdio, fs real worker contracts, per-module denial matrix and final binary audit | ABI v7 / policy v2 three-layer gates pass; the twelve `capsid:` modules can be authorized item by item, with known deferred modules and operations staying `unavailable` |

Expected failures are accepted only with an exact test name and deviation ID. Standard tests take precedence over existing txiki.js behavior; deviation changes must be reviewed as profile changes.

The current adapted batch executes 84 upstream files in separate realms. It only proves those pinned files, not all of WPT or full ECMA-429 conformance.