# Releases and Installation

Choose a release channel deliberately. Installing a beta is not an upgrade of
the stable channel's support or isolation guarantees.

## Release Channels

As recorded by the 2026-11-21 release capture:

| Channel | Version | Meaning |
| --- | --- | --- |
| Experimental prerelease | [v0.3.0-beta.5](https://github.com/rowanellis-113/capsid/releases/tag/v0.3.0-beta.5) | Select this tag explicitly. Public C ABI v7; runtime version `0.3.0`. |
| Stable | [v0.2.2](https://github.com/rowanellis-113/capsid/releases/tag/v0.2.2) | Stable `0.2.x` line; select this tag explicitly. |

GitHub's [Latest](https://github.com/rowanellis-113/capsid/releases/latest) is a separate default download, not a
selector for either channel above. See [current status](current-status.md#release-and-development-channels)
for its recorded value. The qualified product commit for `v0.3.0-beta.5` is
`c7a80b9bdafe36d415a440be7ed3384f7830d4f8`.
See the [release notes](../release-notes/v0.3.0-beta.5.md),
[qualification record](v0.3.0-current-qualification.md) and
[final verification](evidence/v0.3.0/2026-11-21-beta5-publication/beta5-publication-final-r1.json) for the exact gates, verified assets and earlier
records. A later documentation edit does not qualify a new binary.

## Published Packages

`0.3.0-beta` (`v0.3.0-beta.5`) carries five native archives, each with its
original `.sha256` sidecar, plus `install.sh` (11 verified assets total):

| Platform | Archive | Scope |
| --- | --- | --- |
| Linux x86_64 | `capsid-0.3.0-linux-musl-x86_64.tar.gz` | Fully static musl package; Linux isolation still requires the documented kernel/delegation prerequisites |
| Linux arm64 | `capsid-0.3.0-linux-musl-arm64.tar.gz` | Fully static musl package on native ARM64; Linux isolation still requires the documented kernel/delegation prerequisites |
| macOS arm64 | `capsid-0.3.0-darwin-arm64.tar.gz` | Native Apple Silicon development/integration, not Linux strict sandboxing |
| macOS x86_64 | `capsid-0.3.0-darwin-x86_64.tar.gz` | Native Intel development/integration, not Linux strict sandboxing |
| Windows x86_64 | `capsid-0.3.0-windows-x86_64.zip` | Native MSVC/static-CRT development/integration |

The archive filename contains runtime version `0.3.0`; the Release tag selects
`v0.3.0-beta.5`. Do not infer a `0.3.0` stable-production channel from that
filename.

<a id="development-package-matrix"></a>
## Package Architecture and Matrix

The release and development packaging workflow builds five separate native package targets:

| Native build target | Archive |
| --- | --- |
| Linux x86_64 / AMD64 | `capsid-<version>-linux-musl-x86_64.tar.gz` |
| Linux arm64 / aarch64 | `capsid-<version>-linux-musl-arm64.tar.gz` |
| macOS arm64 / Apple Silicon | `capsid-<version>-darwin-arm64.tar.gz` |
| macOS x86_64 / Intel | `capsid-<version>-darwin-x86_64.tar.gz` |
| Windows x86_64 | `capsid-<version>-windows-x86_64.zip` |

Linux uses separate native Ubuntu runners and architecture-matched Alpine
containers. macOS uses `macos-15` (Apple Silicon) and `macos-15-intel` (Intel),
with the target architecture selected explicitly; these are separate thin
binaries, not universal archives. The package gate checks the native runner CPU,
all three ELF/Mach-O executable architectures, Linux's absence of dynamic-loader
and shared-library dependencies, package metadata and native package/SDK smoke
tests on both Linux and macOS CPUs. Windows remains a native x86_64 job. Archive
and SBOM names use the same canonical architecture.

A manual **Release** workflow run is a validation/package preflight only: it
preserves verified Actions artifacts and cannot publish or edit a Release.
Stable publication remains restricted to stable tag pushes; prerelease
publication requires operator review of every package.

The current installer prefers the explicit CPU suffix. For older Releases,
x86_64 alone can fall back to `linux-musl.tar.gz`; ARM64 never downloads that
legacy x86_64 archive. macOS also selects the exact CPU suffix and never substitutes
the other CPU's archive. Missing compatible assets fail without changing the
active installation.

Packages include:

- `capsid-host`, `capsid-worker` and `capsid-bytecode-compile` under `bin`;
- Runtime library, public C/C++ headers and the CMake package;
- Build/compatibility metadata, a package file manifest, licenses and SPDX SBOM
  metadata under the package's shared-data directories.

SBOM verification follows the repository's metadata contract. It is not a claim
of exhaustive transitive license inventory or general SPDX certification.

## Install on Linux or macOS

The installer selects the matching published OS/architecture asset, verifies its
SHA-256 sidecar and installs under `$HOME/.local` by default. The examples
specify both the current repository and a release tag so they do not rely on
an older installer's repository default or GitHub's `latest` selection.

Current experimental prerelease (`v0.3.0-beta.5`):

```sh
curl -fsSL https://github.com/rowanellis-113/capsid/releases/download/v0.3.0-beta.5/install.sh \
  | CAPSID_REPO=rowanellis-113/capsid bash -s -- v0.3.0-beta.5
```

Custom prefix:

```sh
curl -fsSL https://github.com/rowanellis-113/capsid/releases/download/v0.3.0-beta.5/install.sh \
  | CAPSID_REPO=rowanellis-113/capsid bash -s -- --prefix "$HOME/opt/capsid-beta" v0.3.0-beta.5
```

Explicit stable `v0.2.2`:

```sh
curl -fsSL https://github.com/rowanellis-113/capsid/releases/download/v0.2.2/install.sh \
  | CAPSID_REPO=rowanellis-113/capsid bash -s -- v0.2.2
```

You can download and inspect `install.sh` before running it instead of piping it
to Bash, or download a platform `.tar.gz` and its `.sha256` sidecar directly
without `curl | bash` (for example with `wget` or a browser):

```sh
# Example: Linux musl x86_64 manual download and verification
wget -q https://github.com/rowanellis-113/capsid/releases/download/v0.3.0-beta.5/capsid-0.3.0-linux-musl-x86_64.tar.gz{,.sha256}
sha256sum -c capsid-0.3.0-linux-musl-x86_64.tar.gz.sha256
tar -xzf capsid-0.3.0-linux-musl-x86_64.tar.gz
```

The checksum sidecar detects archive mismatch; it is not a separate
publisher signature. Do not bypass TLS or checksum verification.

Use the installed prefix's `bin` directory in the [quick start](getting-started.md),
or add it to PATH yourself. Keep different release prefixes separate if you
need stable and beta side by side.

## Install on Windows

Download the ZIP and its `.sha256` sidecar from the chosen Release, verify the
SHA-256 digest in PowerShell and extract into a new destination:

```powershell
$base = 'https://github.com/rowanellis-113/capsid/releases/download/v0.3.0-beta.5'
$archive = '.\capsid-0.3.0-windows-x86_64.zip'
$sidecar = "$archive.sha256"
Invoke-WebRequest "$base/capsid-0.3.0-windows-x86_64.zip" -OutFile $archive
Invoke-WebRequest "$base/capsid-0.3.0-windows-x86_64.zip.sha256" -OutFile $sidecar
$expected = ((Get-Content -LiteralPath $sidecar -Encoding UTF8 -Raw).Trim() -split '\s+')[0]
$actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
if ($actual -ine $expected) { throw 'SHA-256 mismatch' }
Expand-Archive -LiteralPath $archive -DestinationPath "$HOME\capsid-beta" -Force
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

The `0.3.0-beta` release passed its declared source-specific CI and
five-platform package review. Its published 11 assets—the five archives,
five checksum sidecars and installer—were verified against their remote digests.

That does **not** claim complete Web Platform Tests conformance, removal of all
genuine prerequisite skips, closure of every historical v0.3.0 goal or a new
performance measurement. Known raw WPT differences remain under the pinned
compatibility policy. See [testing](testing-guide.md),
[conformance](conformance.md) and [performance methodology](performance-benchmarks.md)
for those separate boundaries.
