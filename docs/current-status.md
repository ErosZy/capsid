# Current Status and Data Scope

Reviewed 2026-11-20. This is the current reader-facing status, not another
execution diary. The [machine-readable status](current-status.json) is generated
from committed source locks, the captured GitHub release metadata and validated
measurement archives. Historical reviews retain their original dates and values.

## Development Checkout

The development line builds on the published `0.3.0-beta` (`v0.3.0-beta.3`)
source (`af8acc1a7710b6344bdf85d1f6b0b067e0fabe89`) with pure-JS hot-path
runtime optimizations in `js/bootstrap.js`, `js/fetch.js`, and `js/url.js`
(`runtime-optimization-successor`) plus `capsid-perf-lane-v3` benchmark and
`perf-site` updates. Public C ABI v7, FetchRPC wire protocol, C/C++ runtime,
build inputs, and `vendor/txiki.js` (QuickJS-NG 0.15.1) remain unchanged.
The machine status keeps qualified-release digests (`headerSHA256`,
`protocolSHA256`, and `qualifiedReleaseFiles`) aligned with the published
`v0.3.0-beta.3` release while setting `releaseQualificationInherited:false`:
later JS, benchmark, or documentation edits do not inherit binary qualification
for `v0.3.0-beta.3`. See [source formatting](formatting.md) for exact-byte
exclusions and [package architecture](releases.md#development-package-matrix)
for the five-platform archive matrix.

## Release and Development Channels

| Item | Current recorded value | Boundary |
| --- | --- | --- |
| Development branch | `dev/v0.3.0` | Documentation/data changes do not requalify a binary |
| Experimental prerelease (GitHub Latest) | `0.3.0-beta` ([v0.3.0-beta.3](https://github.com/ErosZy/capsid/releases/tag/v0.3.0-beta.3)) | Published as GitHub `Latest`; not a stable-production recommendation |
| Stable `0.2.x` release | `v0.2.2` | Remains the stable `0.2.x` channel; install explicitly with `v0.2.2` |
| Beta runtime version | `0.3.0` | An archive/runtime string, not a stable-channel claim |
| Qualified beta source | `af8acc1a7710b6344bdf85d1f6b0b067e0fabe89` | Tree `483146d28dc9f16025da332e86a9118cb8813f2e`; 5460 frozen git entries |
| Public interface | C ABI v7; capability policy v2 | Existing layouts remain frozen; new interfaces are additive |
| Protocol | FetchRPC v3 by default; explicit negotiated v4 | Never mix versions on one connection |
| Pinned vendor | txiki.js `v26.6.0`, 78 SDK patches | No vendor upgrade is implied by this documentation refresh |

The [read-only release capture](data/release-status.json) records its actual UTC
observation, 11 asset IDs/sizes/digests and stable/beta flags. The
[release guide](releases.md) explains installation and package identity.

## Delivered Scope and Security Boundary

`0.3.0-beta` includes the bounded FetchRPC v4/WebSocket foundation: inbound
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
| Full Release/LTO, sanitizer/provider, native platform, fuzz/index qualification | [44246646875](https://github.com/ErosZy/capsid/actions/runs/44246646875) |
| Pinned formatter and protected-input controls | [44246646886](https://github.com/ErosZy/capsid/actions/runs/44246646886) |
| Five-platform Release validation and native packaging (all 13 jobs passed, attempt 1) | [44272321448](https://github.com/ErosZy/capsid/actions/runs/44272321448) |

All 11 published `0.3.0-beta` (`v0.3.0-beta.3`) assets match the verified
archives and sidecars from run `44272321448` plus `install.sh` at `af8acc1`,
recorded in [beta3-publication-final-r1.json](evidence/v0.3.0/2026-11-18-beta3-publication/beta3-publication-final-r1.json).

Earlier beta tags remain preserved for traceability: `v0.3.0-beta.2`
(`87ed4630fa57e360ac0a3a8158218398250c8035`, marked Pre-release, with
[beta2-publication-final-r1.json](evidence/v0.3.0/2026-11-13-ci-resumption/beta2-publication-final-r1.json))
and the failed `v0.3.0-beta.1` tag (`16f4354d353e6ab044394efe85438f040741c033`,
whose checkout failed before product builds/tests).

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

The latest complete accepted local session was measured on **2026-11-20** at
`0.3.0-beta.4` development snapshot `82eaab999cfd628dbe1529eb553f63c0ee52a9ea`,
tree `5e61f517d5325704c8dc38e35b0c63193fb91ec1`, under `capsid-perf-lane-v3`
(pure-JS seven-stack matrix across three reference tiers plus `dynamic-api`).
It is **not a measurement of published `v0.3.0-beta.3`**; its `release` label
`v0.3.0-beta.4` has an empty `sourceTag` until a release tag is qualified.

| Metric | Recorded Capsid value |
| --- | ---: |
| JSON 1 KiB QPS | 21,770.8 |
| JSON 16 KiB QPS | 13,952.2 |
| Stream 1 KiB QPS | 10,756.9 |
| Dynamic API QPS | 9,974.9 |
| Low-load service-tree PSS | 14.9 MiB |
| Loaded service-tree PSS | 18.6 MiB |
| Warm workers at 10 KiB / 100 KiB / 1 MiB, 2048 MiB budget | 731 / 503 / 128 |

Session: `local-wsl-v030-beta4-v3-20261120T052109Z-82eaab99`.
Machine: i5-12400F, 12 logical CPUs, measured CPU set 0–3 under WSL2. All three
lanes (`cold`, `qps`, `density`), quiet windows and independent source/binary/
raw-record review passed (`4533` sealed evidence files). Physical Windows-host
exclusivity is **not** claimed.

The archived `v1` cross-language baseline (`485b29be6cc8d0dcdb653ee6226956ddece02948`,
2026-11-16, session `local-wsl-v030-headers-ready-20261115T235007Z-7368b4ec`,
`5123` sealed evidence files; Capsid `9341.6 / 8090.2 / 6491.7 QPS`, `14.0 / 18.0 MiB`,
`738 / 509 / 128` warm workers) remains preserved unchanged in `perf-site/data.js`
and `perf-site/archives/` alongside the new `v3` bundles (`9656` total verified
evidence files across 6 compressed transports). Historical `v0.2.2` values remain
unchanged and unverified under the current archive contract; no formal paired
trend is inferred across different matrix contracts (`v1` vs `v3`) or unverified
historical points.

Read the [performance reference](performance-benchmarks.md),
[latest accepted review](data/performance-review.json),
[measured `v3` source manifest](data/performance-source-manifest-v030-beta4.json),
[archived `v1` source manifest](data/performance-source-manifest.json) and
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
