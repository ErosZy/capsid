# Building from Source

Use a [prebuilt release](releases.md) if you only need to run an application.
This guide is for runtime development, custom builds and SDK integration.
Building successfully is not the same as passing the release qualification
matrix or proving production isolation.

## Requirements

| Component | Requirement |
| --- | --- |
| Source | Git checkout with the recorded submodule commits; initialize recursively |
| Build tools | CMake 3.18+ and a supported build tool; the examples use Ninja |
| Compiler | Public C11/C++11 interfaces; first-party Host internals require C++20 support |
| JavaScript tooling | Node.js/npm for the build-time tools; use the CI-pinned Node 24.14.0 for reference tests |
| Crypto library | OpenSSL 3.0+ as required by the CMake targets; release/sanitizer CI pins its provider separately |
| Host networking | Boost and c-ares 1.27+ |
| Full interoperability tests | Full CPython 3.12.3 with `venv`/`pip`, Node 24.14.0, an independent OpenSSL CLI and locked workerd/client assets |

The CMake dependency minimum is not a claim that every provider version has the
same qualification. CI uses pinned, audited provider sources and instrumentation;
copying an ordinary system-library build does not reproduce sanitizer coverage.
See [testing](testing-guide.md) and the
[workflow](../.github/workflows/testing-validity.yml) for the exact profile.

## 1. Prepare a Checkout

For the published beta source:

```sh
git clone --branch v0.3.0-beta.2 --recurse-submodules \
  https://github.com/ErosZy/capsid.git
cd capsid
```

For an existing checkout, preserve your work and select the intended revision
before building. Do not reset or clean a working tree merely to satisfy the
Release identity gate.

```sh
git submodule update --init --recursive
```

Install all locked JavaScript dependency trees when preparing a test build:

```sh
for directory in \
  vendor/txiki.js \
  examples/hono-reference \
  examples/itty-router-reference \
  examples/h3-v2-reference \
  examples/elysia-reference
do
  npm ci --ignore-scripts --prefix "$directory"
done
```

The framework dependencies are required when their test targets are configured;
missing inputs are errors, not a reason to omit the corresponding gates.
On Windows PowerShell, use `npm.cmd`:

```powershell
$directories = @(
  'vendor/txiki.js',
  'examples/hono-reference',
  'examples/itty-router-reference',
  'examples/h3-v2-reference',
  'examples/elysia-reference'
)
foreach ($directory in $directories) {
  npm.cmd ci --ignore-scripts --prefix $directory
  if ($LASTEXITCODE -ne 0) { throw "Dependency install failed: $directory" }
}
```

## 2. Install Native Dependencies

### Ubuntu 24.04

With the required Node version prepared separately:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build git \
  libboost-system-dev libssl-dev libc-ares-dev
```

These are ordinary development dependencies, not the pinned/instrumented
providers used by full CI. Linux sandbox tests additionally need the kernel,
filesystem and delegation prerequisites described in [Linux sandboxing](linux-sandbox.md).

### macOS

```sh
brew install cmake ninja boost openssl@3.5 c-ares
```

Use the locked Node/Python runtimes when running the reference suites. Native
builds support trusted development; they do not enable Linux strict sandboxing
or managed mode.

### Windows

Use a Visual Studio developer shell with MSVC, the Windows SDK, Ninja and
vcpkg available. Capsid does not support a MinGW build.

```powershell
vcpkg install openssl c-ares boost-system boost-asio boost-beast --triplet x64-windows-static
if ($LASTEXITCODE -ne 0) { throw 'vcpkg install failed' }
```

The static triplet and CRT must match. Full native-test prerequisites and the
separate OpenSSL certificate-fixture CLI are described in [Windows](windows.md).

## 3. Configure and Build

Use a separate build directory per toolchain, configuration and sanitizer.
Do not reuse objects from a different profile.

### Development Build

Linux or macOS:

```sh
cmake -S . -B build-dev -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCAPSID_ENABLE_LTO=OFF \
  -DCAPSID_BUILD_HOST=ON -DBUILD_TESTING=ON
cmake --build build-dev --parallel 2
```

Windows PowerShell, after preparing dependencies and the developer shell:

```powershell
cmake -S . -B build-dev -G Ninja `
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_BUILD_TYPE=Debug -DCAPSID_BUILD_HOST=ON `
  -DBUILD_TESTING=ON `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build build-dev --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

See the [testing guide](testing-guide.md) before interpreting missing reference
prerequisites, WPT results or platform skips.

### Release Build

Release configuration requires resolvable Git provenance and a **clean source
worktree**. Release packaging rejects development provenance. Use a separate
clean checkout of the selected source; do not disable the identity check.

For Linux/macOS, from that prepared checkout:

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCAPSID_BUILD_HOST=ON \
  -DBUILD_TESTING=OFF
cmake --build build-release --parallel 2
```

On Windows, use the compiler/toolchain/triplet arguments from the development
example with `Release`, a new build directory and the intended `BUILD_TESTING`
setting. CMake disables MSVC LTO for the project's replaceable-allocation
contract; do not describe that build as Linux Release/LTO.

`BUILD_TESTING=OFF` is suitable for producing tools without the test targets.
It is not a release qualification command.

## 4. Install or Package

From a clean, appropriately configured Release build:

```sh
cmake --install build-release --prefix "$HOME/opt/capsid-sdk"
cmake --build build-release --target package
```

Keep the install prefix outside the source worktree so installation does not
make a clean Release checkout dirty before packaging. Use a separate prefix for
each build you want to keep. CPack emits the configured archive
and SHA-256 sidecar; use the [release guide](releases.md) to understand package
identity, SBOM scope and supported published architectures. Current Linux package
names contain both libc and target CPU, for example
`capsid-0.3.0-linux-musl-x86_64.tar.gz` and
`capsid-0.3.0-linux-musl-arm64.tar.gz` (local glibc builds use `linux-gnu-<arch>`).
CMake canonicalizes `AMD64`/`x86_64` and `aarch64`/`arm64` from the target
processor, not the build host. The Release workflow uses native runners for both
Linux CPUs and both macOS CPUs, and retains validation-before-packaging.
macOS Intel emits `capsid-0.3.0-darwin-x86_64.tar.gz`; Apple Silicon keeps
`capsid-0.3.0-darwin-arm64.tar.gz`. These are distinct native archives, not
universal binaries.

The examples use single-configuration Ninja. With a supported multi-config
generator, select the same configuration for build, tests and installation using
`--config Release` / `ctest -C Release` as appropriate.

## Consume the SDK

An installed package exposes `<capsid/runtime.h>`, `<capsid/runtime.hpp>` and
its CMake package:

```cmake
cmake_minimum_required(VERSION 3.18)
project(my_host LANGUAGES C CXX)
find_package(Capsid CONFIG REQUIRED)
add_executable(my_host main.cc)
target_link_libraries(my_host PRIVATE Capsid::capsid_runtime)
```

Configure the consuming project with
`-DCMAKE_PREFIX_PATH=/absolute/path/to/capsid-sdk`, pointing at the prefix you
installed.
The runtime is implemented in C++, so a C application still needs the C++ linker.
On Windows, match the package's static CRT; see the complete consumer example in
[Windows](windows.md).

For an in-tree integration, the project also provides `capsid::runtime`:

```cmake
add_subdirectory(path/to/capsid EXCLUDE_FROM_ALL)
target_link_libraries(my_host PRIVATE capsid::runtime)
```

The [Host integration contract](host-integration.md) defines request/credit,
cancellation, READY and process ownership. Linking the target alone does not
implement these responsibilities.
