# Releases and Installation

Choose a release channel deliberately. Installing a beta is not an upgrade of
the stable channel's support or isolation guarantees.

## Release Channels

As recorded by the 2026-11-17 publication verification:

| Channel | Version | Meaning |
| --- | --- | --- |
| Stable | [v0.2.2](https://github.com/ErosZy/capsid/releases/tag/v0.2.2) | Selected by the default installer and the `latest` release URL |
| Experimental prerelease | [v0.3.0-beta.2](https://github.com/ErosZy/capsid/releases/tag/v0.3.0-beta.2) | Explicit opt-in; public C ABI v7; runtime version string `0.3.0` |
| Historical failed tag | `v0.3.0-beta.1` | Preserved for traceability; its release workflow failed before builds/tests and no Release assets were published |

The beta was published with `prerelease:true` and `make_latest:false`. Its
qualified product commit is `87ed4630fa57e360ac0a3a8158218398250c8035`.
The [qualification record](v0.3.0-current-qualification.md) and
[final verification](evidence/v0.3.0/2026-11-13-ci-resumption/beta2-publication-final-r1.json)
retain the exact gates, original failures and one explicitly authorized
macOS package capacity retry. A later documentation edit is not a new
qualification of the release binary.

## Published Packages

Beta.2 carries these archives, each with its original `.sha256` sidecar:

| Platform | Archive | Scope |
| --- | --- | --- |
| Linux x86_64 | `capsid-0.3.0-linux-musl.tar.gz` | Fully static musl package; Linux isolation still requires the documented kernel/delegation prerequisites |
| macOS arm64 | `capsid-0.3.0-darwin-arm64.tar.gz` | Native development/integration, not Linux strict sandboxing |
| Windows x86_64 | `capsid-0.3.0-windows-x86_64.zip` | Native MSVC/static-CRT development/integration |

The archive filename contains runtime version `0.3.0`; the Release tag selects
`v0.3.0-beta.2`. Do not infer a stable channel from that filename. A source build
on another architecture does not mean a matching release asset exists; in
particular, these assets do not include Linux arm64 or macOS x86_64.

Packages include:

- `capsid-host`, `capsid-worker` and `capsid-bytecode-compile` under `bin`;
- Runtime library, public C/C++ headers and the CMake package;
- Build/compatibility metadata, a package file manifest, licenses and SPDX SBOM
  metadata under the package's shared-data directories.

SBOM verification follows the repository's metadata contract. It is not a claim
of exhaustive transitive license inventory or general SPDX certification.

## Install on Linux or macOS

The installer selects the matching published OS/architecture asset, verifies its
SHA-256 sidecar and installs under `$HOME/.local` by default.

Latest stable:

```sh
curl -fsSL https://github.com/ErosZy/capsid/releases/latest/download/install.sh | bash
```

Explicit beta:

```sh
curl -fsSL https://github.com/ErosZy/capsid/releases/download/v0.3.0-beta.2/install.sh \
  | bash -s -- v0.3.0-beta.2
```

Custom prefix:

```sh
curl -fsSL https://github.com/ErosZy/capsid/releases/download/v0.3.0-beta.2/install.sh \
  | bash -s -- --prefix "$HOME/opt/capsid-beta2" v0.3.0-beta.2
```

You can download and inspect `install.sh` before running it instead of piping it
to Bash. The checksum sidecar detects archive mismatch; it is not a separate
publisher signature. Do not bypass TLS or checksum verification.

Use the installed prefix's `bin` directory in the [quick start](getting-started.md),
or add it to PATH yourself. Keep different release prefixes separate if you
need stable and beta side by side.

## Install on Windows

Download the ZIP and its `.sha256` file from the chosen Release. Verify them in
PowerShell before extracting; the following assumes both files are in the
current directory:

```powershell
$archive = '.\capsid-0.3.0-windows-x86_64.zip'
$sidecar = "$archive.sha256"
$expected = ((Get-Content -LiteralPath $sidecar -Encoding UTF8 -Raw).Trim() -split '\s+')[0]
$actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $expected.ToLowerInvariant()) { throw 'SHA-256 mismatch' }
Expand-Archive -LiteralPath $archive -DestinationPath '.\capsid-beta2'
```

Choose a new destination instead of overwriting another installation. Use the
extracted package's actual `bin` path; the archive has a versioned top-level
directory. Follow the [PowerShell quick start](getting-started.md#windows-powershell).
For source builds or CMake SDK consumption, see [Windows](windows.md).

## Keep Artifact Identities Together

Do not mix a Host, worker, runtime library or compiler from different builds.
Trusted bytecode must be compiled for the matching compatibility identity and
validated by the Host. The compiler provides:

```sh
capsid-bytecode-compile --print-compatibility-id
```

This is a compiler command, not a universal `--build-info` interface for every
program. The public runtime build-info API and package metadata describe the
build provenance; see [Host integration](host-integration.md).

## What Release Acceptance Means

The beta passed its declared source-specific CI, actual tag validation and
three-platform package review. Its published seven assets—the three archives,
three checksum sidecars and installer—were verified against their remote digests.

That does **not** claim complete Web Platform Tests conformance, removal of all
genuine prerequisite skips, closure of every historical v0.3.0 goal or a new
performance measurement. Known raw WPT differences remain under the pinned
compatibility policy. See [testing](testing-guide.md),
[conformance](conformance.md) and [performance methodology](performance-benchmarks.md)
for those separate boundaries.
