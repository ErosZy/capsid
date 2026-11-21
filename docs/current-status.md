# Current Status and Data Scope

Reviewed 2026-11-22. This is the current reader-facing status, not another
execution diary. The [machine-readable status](current-status.json) is generated
from committed source locks, the captured GitHub release metadata and validated
measurement archives. Historical reviews retain their original dates and values.

## Development Checkout

The published prerelease `v0.3.0-beta.5` (`c7a80b9`) includes the pure-JS
hot-path changes in `js/bootstrap.js`, `js/fetch.js` and `js/url.js` on top of
pinned `vendor/txiki.js` (`v26.6.0`, QuickJS-NG 0.15.1, 78 SDK patches).

The current `dev/v0.3.1` checkout is an `engine-integration-successor`
(`releaseQualificationInherited:false`) that integrates `vendor/quickjs-turbo`
(`v0.15.1-turbo.3`, commit `fb7a53b6fb06edd46a431224fdd839b71fe0b629`) through
`patches/txiki/0079-quickjs-turbo-integration.patch` (79 SDK patches total)
while keeping the public C ABI v7 and FetchRPC v3/v4 wire protocol unchanged.
On Linux x86-64 (GCC 13+, non-sanitizer builds) it enables the zero-JIT native
register-frame interpreter (`LINUX_X86_64`) with a portable C fallback on
sanitizers and other platforms, and sets process-startup density defaults
(`QJS_DISABLE_NATIVE_OWN_BYTECODE=1`, `QJS_DISABLE_PROPERTY_IC=1`, overridable
via environment) in `src/worker_main.cc` to preserve high-density worker
packing. The derived release qualification and sealed benchmark records below
describe the tagged `v0.3.0-beta.5` release, not new qualification for
`dev/v0.3.1` commits. See [source formatting](formatting.md) for exact-byte
exclusions and [package architecture](releases.md#development-package-matrix)
for the five-platform archive matrix.

## Release and Development Channels

| Item | Current recorded value | Boundary |
| --- | --- | --- |
| Development branch | `dev/v0.3.1` | Engine-integration successor; does not inherit `v0.3.0-beta.5` binary qualification |
| Experimental prerelease | [v0.3.0-beta.5](https://github.com/rowanellis-113/capsid/releases/tag/v0.3.0-beta.5) | Current beta (QuickJS-NG 0.15.1); install by tag |
| GitHub Latest (recorded default) | [v0.3.0-beta.3](https://github.com/rowanellis-113/capsid/releases/tag/v0.3.0-beta.3) | Earlier release, not the current beta or stable line |
| Stable `0.2.x` release | [v0.2.2](https://github.com/rowanellis-113/capsid/releases/tag/v0.2.2) | Install by tag |
| Beta runtime version | `0.3.0` | An archive/runtime string, not a stable-channel claim |
| Qualified beta source | `c7a80b9bdafe36d415a440be7ed3384f7830d4f8` | Tree `876e29c2792899b654310fea72aac979bd8bb48f`; 5469 frozen git entries |
| Public interface | C ABI v7; capability policy v2 | Existing layouts remain frozen; new interfaces are additive |
| Protocol | FetchRPC v3 by default; explicit negotiated v4 | Never mix versions on one connection |
| Pinned vendor (`dev/v0.3.1`) | txiki.js `v26.6.0` + quickjs-turbo `v0.15.1-turbo.3` (`fb7a53b6`), 79 SDK patches | Published `v0.3.0-beta.5` retains txiki.js `v26.6.0` (QuickJS-NG 0.15.1), 78 SDK patches |

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
| Full Release/LTO, sanitizer/provider, native platform, fuzz/index qualification | [44778427101, attempt 2](https://github.com/rowanellis-113/capsid/actions/runs/44778427101/attempts/2) |
| Pinned formatter and protected-input controls | [44778427087](https://github.com/rowanellis-113/capsid/actions/runs/44778427087) |
| Five-platform tag Release validation and native packaging (all 13 jobs passed, attempt 1) | [44851404278](https://github.com/rowanellis-113/capsid/actions/runs/44851404278) |

All 11 published `v0.3.0-beta.5` assets match the five original archives and
sidecars from run `44851404278` plus `install.sh` at `c7a80b9`, as recorded
in [the final publication review](evidence/v0.3.0/2026-11-21-beta5-publication/beta5-publication-final-r1.json).

`v0.3.0-beta.4` remains tagged at `07db439` with no published Release.
[`v0.3.0-beta.3`'s record](evidence/v0.3.0/2026-11-18-beta3-publication/beta3-publication-final-r1.json) remains separate.
Other earlier beta tags are preserved: `v0.3.0-beta.2`
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

The latest accepted session was measured on **2026-11-21** using a clean local
Release/LTO rebuild of [`v0.3.0-beta.5`](https://github.com/rowanellis-113/capsid/releases/tag/v0.3.0-beta.5)
(`c7a80b9bdafe36d415a440be7ed3384f7830d4f8`). It matches the prerelease **source**; it does not measure
the distributed binaries. It uses `capsid-perf-lane-v3` (pure-JS seven-stack
matrix across three reference tiers plus `dynamic-api`).

| Metric | Recorded Capsid value |
| --- | ---: |
| JSON 1 KiB QPS | 21,746.6 |
| JSON 16 KiB QPS | 14,194.6 |
| Stream 1 KiB QPS | 10,649.7 |
| Dynamic API QPS | 9,808.1 |
| Low-load service-tree PSS | 15.0 MiB |
| Loaded service-tree PSS | 18.5 MiB |
| Warm workers at 10 KiB / 100 KiB / 1 MiB, 2048 MiB budget | 729 / 503 / 128 |

Session: `local-wsl-beta5-v3-20261121T064717Z-c7a80b9`. Machine: i5-12400F, 12 logical CPUs, measured set 0–3
under WSL2. Cold, QPS, density and whole-session replay all passed (`4546`
evidence files). Physical Windows-host exclusivity is **not** claimed.

The untagged beta.4 `v3` snapshot (`82eaab9`) and the archived `v1` baseline
(`485b29b`) are preserved unchanged. The historical `v0.2.2` observation
remains unverified under the current archive contract. Different reference
versions, different matrix contracts and unverified historical points do not
support a formal paired trend.

See the [performance reference](performance-benchmarks.md),
[session review](data/performance-review-v030-beta5.json), [measured source manifest](data/performance-source-manifest-v030-beta5.json)
and [site/transport instructions](../perf-site/README.md).

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
