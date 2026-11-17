# Current Status and Data Scope

Reviewed 2026-11-17. This is the current reader-facing status, not another
execution diary. The [machine-readable status](current-status.json) is generated
from committed source locks, the captured GitHub release metadata and validated
measurement archives. Historical reviews retain their original dates and values.

## Release and Development Channels

| Item | Current recorded value | Boundary |
| --- | --- | --- |
| Development branch | `dev/v0.3.0` | Documentation/data changes do not requalify a binary |
| Latest stable | `v0.2.2` | Still selected by the default installer/latest Release URL |
| Experimental prerelease | `v0.3.0-beta.2` | Published; not a stable-production recommendation |
| Beta runtime version | `0.3.0` | An archive/runtime string, not a stable-channel claim |
| Qualified beta source | `87ed4630fa57e360ac0a3a8158218398250c8035` | Tree `96bef3a7b0a1acec0cd37be9d470a312046386cb`; 5347 frozen source inputs |
| Public interface | C ABI v7; capability policy v2 | Existing layouts remain frozen; new interfaces are additive |
| Protocol | FetchRPC v3 by default; explicit negotiated v4 | Never mix versions on one connection |
| Pinned vendor | txiki.js `v26.6.0`, 78 SDK patches | No vendor upgrade is implied by this documentation refresh |

The [read-only release capture](data/release-status.json) records its actual UTC
observation, seven asset IDs/sizes/digests and stable/beta flags. The
[release guide](releases.md) explains installation and package identity.

## Delivered Scope and Security Boundary

Beta.2 includes the bounded FetchRPC v4/WebSocket foundation: inbound
WebSocketPair/101, outbound ws/wss, channel/result/terminal confirmation,
Upgrade-fetch metadata/body fallback and retained incoming connection ownership.
Bindings remain v1 with independent User/Binding runtimes and operation gates.
The ordinary handler is still `fetch(request)`, not a complete Wrangler,
Cloudflare platform or `fetch(request, env, ctx)` compatibility layer.

Local single-worker/static-pool modes support trusted development and
integration on Linux, macOS and native Windows. Linux managed mode is the
first-party production path, but its deployment interface remains experimental.
Untrusted workloads require a verified Linux strict sandbox and surrounding
policy/resource/network boundaries. Native macOS/Windows do not provide that
production isolation guarantee. Successful startup or CI is not a substitute.

Use [architecture](architecture.md), [embedding](host-integration.md),
[Host configuration](host-config.md), [Linux sandboxing](linux-sandbox.md) and
[security policy](../SECURITY.md) for the exact boundaries.

## Latest Published Binary Qualification

All of these bind to the beta source above, not to a later docs/data commit:

| Required scope | Actual GitHub Actions run |
| --- | --- |
| Full Release/LTO, sanitizer/provider, native platform, fuzz/index qualification | [43881121477](https://github.com/ErosZy/capsid/actions/runs/43881121477) |
| Debug, original regressions/timings and uninterrupted 1800-second soak | [43881141077](https://github.com/ErosZy/capsid/actions/runs/43881141077) |
| Three-platform package preflight | [43881160382](https://github.com/ErosZy/capsid/actions/runs/43881160382) |
| Actual beta.2 tag validation and packages | [43912740060](https://github.com/ErosZy/capsid/actions/runs/43912740060) |

Required source qualification passed on its first attempts. In the actual tag
run, the original macOS package job never acquired a runner and executed no
steps. Exactly that job received one explicitly authorized capacity retry and
passed on actual attempt 2; the other ten executions remained their original
successful attempt 1. The cancellation, aliases, request and execution ledger
remain visible in the [publication verification](evidence/v0.3.0/2026-11-13-ci-resumption/beta2-publication-final-r1.json).

The failed beta.1 tag remains at `16f4354d353e6ab044394efe85438f040741c033`.
Its checkout-ref failure happened before product builds/tests. It was not moved,
rerun into success or used to publish beta.2.

## Standards and Compatibility Data

- Profile: `CAPSID-MIN-2025-subset-v0`.
- WPT commit: `1985b47aa8972a970f005957f2bfa036da1787c6`; 84 selected files.
- Current compatibility classification:
  `CAPSID-WORKERD-v1.20261010.1-default-v2`.
- Five raw file failures remain: rejection support, PromiseRejectionEvent
  constructor, MessagePort initial-disabled, Headers basic and Headers errors.
- `ordinarySuitePassed:false`; `standardsConformanceClaimed:false`.

The v2 mapping preserves the four declared nested-task timeouts and other
exact reference conflicts. It does not accept an arbitrary timeout, cleanup
failure, changed source or extra failure. See [conformance](conformance.md)
and [testing](testing-guide.md). Required compatibility acceptance must not be
reported as complete WPT, browser or workerd conformance.

## Latest Accepted Local Performance Record

The latest complete accepted local session was measured on **2026-11-16** at
private development snapshot `485b29be6cc8d0dcdb653ee6226956ddece02948`, tree
`25bbf46247b68478a005cf779d884970fb98dc13`. It is **not a measurement of beta.2
or the later documentation commit**, and its archival `release: v0.3.0` label
does not mean a stable v0.3.0 release existed.

| Metric | Recorded Capsid value |
| --- | ---: |
| JSON 1 KiB QPS | 9341.6 |
| JSON 16 KiB QPS | 8090.2 |
| Stream 1 KiB QPS | 6491.7 |
| Low-load service-tree PSS | 14.0 MiB |
| Loaded service-tree PSS | 18.0 MiB |
| Warm workers at 10 KiB / 100 KiB / 1 MiB, 2048 MiB budget | 738 / 509 / 128 |

Session: `local-wsl-v030-headers-ready-20261115T235007Z-7368b4ec`.
Machine: i5-12400F, 12 logical CPUs, measured CPU set 0–3 under WSL2. All three
original lanes, four quiet windows and independent source/binary/raw-record
review passed. Physical Windows-host exclusivity is **not** claimed.

The site now includes the complete 5123 sealed evidence files in hashed,
compressed transports. Staging restores and revalidates the original bytes and
point; compression is not a substitute for acceptance. Historical v0.2.2 values
remain unchanged and unverified under the current archive contract; no formal
paired gain or universal slowdown is inferred across those different records.

Read the [performance reference](performance-benchmarks.md),
[original accepted review](data/performance-review.json),
[measured source manifest](data/performance-source-manifest.json) and
[site/transport instructions](../perf-site/README.md).

## Documentation Coverage and Historical Records

The [coverage inventory](documentation-coverage.json) classifies every page as a
current guide/reference, dated plan/journal or immutable evidence snapshot. A
link audit is not a claim that every page was rewritten or every example run.
Current references are checked against the source/profile above; dated evidence
is checked for preservation, not updated to different numbers.

Large execution journals formerly prepended to current reference pages are
preserved as exact text snapshots with a hash inventory and legacy-anchor
links. The [history index](history/README.md) explains their scope. No old
handoff is an instruction to restart a completed release or benchmark campaign.
