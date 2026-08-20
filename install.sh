#!/usr/bin/env bash
# Capsid release installer.
#
# Downloads the prebuilt Capsid package for the current OS/architecture from
# a GitHub Release, verifies the SHA-256, and extracts it into a prefix
# (default: $HOME/.local). After install, capsid-host/capsid-worker/
# capsid-bytecode-compile are available under <prefix>/bin.
#
# Usage:
#   ./install.sh [VERSION]                # default: latest stable release
#   ./install.sh v0.2.0-rc.07             # explicit tag (pre-release too)
#   PREFIX="$HOME/opt" ./install.sh       # install to a custom prefix
#   ./install.sh --prefix "$HOME/opt" v0.2.0
#
# Environment:
#   CAPSID_REPO   GitHub repository (default: ErosZy/capsid)
#   PREFIX        installation prefix (default: $HOME/.local)
set -euo pipefail

CAPSID_REPO="${CAPSID_REPO:-ErosZy/capsid}"
PREFIX="${PREFIX:-$HOME/.local}"
VERSION=""

usage() {
    sed -n '2,16p' "$0"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            [[ $# -ge 2 ]] || { echo "install.sh: --prefix requires a value" >&2; exit 2; }
            PREFIX="$2"
            shift 2
            ;;
        --prefix=*)
            PREFIX="${1#*=}"
            shift
            ;;
        -h|--help)
            usage
            ;;
        -*)
            echo "install.sh: unknown option: $1" >&2
            exit 2
            ;;
        *)
            [[ -z "$VERSION" ]] || { echo "install.sh: too many arguments" >&2; exit 2; }
            VERSION="$1"
            shift
            ;;
    esac
done

VERSION="${VERSION:-latest}"
if [[ "$VERSION" != latest && "$VERSION" != v* ]]; then
    VERSION="v${VERSION}"
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "install.sh: curl is required" >&2
    exit 1
fi

api_url="https://api.github.com/repos/${CAPSID_REPO}/releases/tags/${VERSION}"
if [[ "$VERSION" == latest ]]; then
    api_url="https://api.github.com/repos/${CAPSID_REPO}/releases/latest"
fi

echo "==> resolving release: ${VERSION}"
release_json="$(curl -fsSL "$api_url")"

if [[ "$VERSION" == latest ]]; then
    VERSION="$(printf '%s\n' "$release_json" \
        | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -1)"
    if [[ -z "$VERSION" ]]; then
        echo "install.sh: could not resolve the latest release" >&2
        exit 1
    fi
    echo "==> latest tag: ${VERSION}"
fi

os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
    Linux)
        case "$arch" in
            x86_64|amd64) ;;
            *) echo "install.sh: unsupported Linux architecture: ${arch}" >&2; exit 1 ;;
        esac
        asset_pattern='capsid-.*-linux-musl\.tar\.gz$'
        tool_suffix=''
        ;;
    Darwin)
        case "$arch" in
            arm64|aarch64) darwin_arch="arm64" ;;
            x86_64|amd64) darwin_arch="x86_64" ;;
            *) echo "install.sh: unsupported macOS architecture: ${arch}" >&2; exit 1 ;;
        esac
        asset_pattern="capsid-.*-darwin-${darwin_arch}\\.tar\\.gz$"
        tool_suffix=''
        ;;
    MINGW*|MSYS*|CYGWIN*)
        case "$arch" in
            x86_64|amd64) ;;
            *) echo "install.sh: unsupported Windows architecture: ${arch}" >&2; exit 1 ;;
        esac
        asset_pattern='capsid-.*-windows-x86_64\.zip$'
        tool_suffix='.exe'
        ;;
    *)
        echo "install.sh: unsupported operating system: ${os}" >&2
        exit 1
        ;;
esac

asset_url="$(printf '%s\n' "$release_json" \
    | grep -o '"browser_download_url"[[:space:]]*:[[:space:]]*"[^"]*"' \
    | sed -E 's/.*"([^"]+)"$/\1/' \
    | grep -E "$asset_pattern" \
    | head -1 || true)"

if [[ -z "$asset_url" ]]; then
    echo "install.sh: no matching release asset for ${os}/${arch} (pattern: ${asset_pattern})" >&2
    echo "available assets:" >&2
    printf '%s\n' "$release_json" \
        | grep -o '"name"[[:space:]]*:[[:space:]]*"[^"]*"' \
        | sed -E 's/.*"([^"]+)"$/\1/' \
        | sed 's/^/  /' >&2 || true
    exit 1
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/capsid-install.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
archive="${tmp}/$(basename "$asset_url")"

echo "==> downloading ${asset_url}"
curl -fsSL -o "$archive" "$asset_url"
curl -fsSL -o "${archive}.sha256" "${asset_url}.sha256"

echo "==> verifying SHA-256"
if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$archive" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 256 "$archive" | awk '{print $1}')"
elif command -v python3 >/dev/null 2>&1; then
    actual="$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$archive")"
else
    echo "install.sh: need sha256sum, shasum or python3 to verify the archive" >&2
    exit 1
fi
expected="$(awk '{print $1}' "${archive}.sha256")"
if [[ -z "$expected" || "$actual" != "$expected" ]]; then
    echo "install.sh: SHA-256 mismatch" >&2
    echo "  expected: ${expected:-<missing>}" >&2
    echo "  actual:   ${actual}" >&2
    exit 1
fi

echo "==> staging package for ${PREFIX}"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd -P)"
extract_dir="$tmp/extracted"
mkdir -p "$extract_dir"
case "$archive" in
    *.zip)
        if command -v unzip >/dev/null 2>&1; then
            unzip -q "$archive" -d "$extract_dir"
        else
            tar -xf "$archive" -C "$extract_dir"
        fi
        ;;
    *)
        tar -xzf "$archive" -C "$extract_dir"
        ;;
esac

# Validate one coherent package root before changing the active command
# surface. Searching only the fresh staging tree prevents an older install
# from satisfying a missing-tool check.
source_host="$(find "$extract_dir" -type f -name "capsid-host${tool_suffix}" \
    -path "*/bin/capsid-host${tool_suffix}" -print -quit 2>/dev/null || true)"
if [[ -z "$source_host" ]]; then
    echo "install.sh: package is missing bin/capsid-host${tool_suffix}" >&2
    exit 1
fi
source_bin="$(dirname "$source_host")"
package_root="$(dirname "$source_bin")"
for tool in capsid-host capsid-worker capsid-bytecode-compile; do
    if [[ ! -f "$source_bin/${tool}${tool_suffix}" ]]; then
        echo "install.sh: package is missing bin/${tool}${tool_suffix}" >&2
        exit 1
    fi
done

# Keep the full package under an immutable version+digest directory. A
# reinstall of identical bytes reuses it; another release gets a distinct
# root, so active commands can be switched only after validation succeeds.
releases_dir="$PREFIX/releases"
release_dir="$releases_dir/${VERSION}-${actual}"
mkdir -p "$releases_dir"
if [[ ! -d "$release_dir" ]]; then
    pending_release="$releases_dir/.${VERSION}-${actual}.$$"
    mv "$package_root" "$pending_release"
    mv "$pending_release" "$release_dir"
fi

echo "==> installing to ${PREFIX}"
bin_dir="$PREFIX/bin"
mkdir -p "$bin_dir"
for tool in capsid-host capsid-worker capsid-bytecode-compile; do
    active="$bin_dir/${tool}${tool_suffix}"
    pending="$bin_dir/.${tool}${tool_suffix}.$$"
    cp -f "$release_dir/bin/${tool}${tool_suffix}" "$pending"
    chmod +x "$pending" 2>/dev/null || true
    mv -f "$pending" "$active"
done

echo "==> installed:"
for tool in capsid-host capsid-worker capsid-bytecode-compile; do
    active="$bin_dir/${tool}${tool_suffix}"
    [[ -f "$active" ]] || {
        echo "install.sh: final verification failed for ${active}" >&2
        exit 1
    }
    echo "  ${active}"
done

case ":$PATH:" in
    *":${bin_dir}:"*) ;;
    *) echo "==> add to PATH: export PATH=\"${bin_dir}:\$PATH\"" ;;
esac
