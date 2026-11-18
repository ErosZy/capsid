#!/usr/bin/env bash
# Offline contract test for install.sh. GitHub, uname and the release assets
# are fixtures, so every supported platform/error path is deterministic.
set -euo pipefail

installer="${1:?expected install.sh path}"
work="$(mktemp -d "${TMPDIR:-/tmp}/capsid-install-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT

real_path="$PATH"
fake_bin="$work/fake-bin"
mkdir -p "$fake_bin"

cat >"$fake_bin/uname" <<'SH'
#!/usr/bin/env bash
case "${1:-}" in
    -s) printf '%s\n' "${CAPSID_FAKE_UNAME_S:?}" ;;
    -m) printf '%s\n' "${CAPSID_FAKE_UNAME_M:?}" ;;
    *) printf '%s\n' "${CAPSID_FAKE_UNAME_S:?}" ;;
esac
SH
cat >"$fake_bin/curl" <<'SH'
#!/usr/bin/env bash
output=""
url=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) output="$2"; shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
    esac
done
if [[ -n "${CAPSID_FAKE_REQUEST_LOG:-}" ]]; then
    printf '%s\n' "$url" >>"$CAPSID_FAKE_REQUEST_LOG"
fi
if [[ -z "$output" ]]; then
    printf '%s\n' "${CAPSID_FAKE_RELEASE_JSON:?}"
else
    # Respect the requested URL: selecting the wrong architecture must not
    # accidentally receive the fixture archive the caller wanted to test.
    cp "${CAPSID_FAKE_ASSET_DIR:?}/${url##*/}" "$output"
fi
SH
chmod +x "$fake_bin/uname" "$fake_bin/curl"

make_package() {
    local label="$1" archive="$2" suffix="$3" missing="${4:-}"
    local tree="$work/tree-$label/capsid-$label"
    mkdir -p "$tree/bin" "$tree/include/capsid"
    for tool in capsid-host capsid-worker capsid-bytecode-compile; do
        [[ "$tool" == "$missing" ]] && continue
        printf '#!/usr/bin/env sh\nprintf "%%s\\n" "%s-%s"\n' \
            "$label" "$tool" >"$tree/bin/$tool$suffix"
        chmod +x "$tree/bin/$tool$suffix"
    done
    printf 'fixture\n' >"$tree/include/capsid/runtime.h"
    if [[ "$archive" == *.zip ]]; then
        (cd "$(dirname "$tree")" &&
            cmake -E tar cf "$archive" --format=zip "$(basename "$tree")")
    else
        tar -czf "$archive" -C "$(dirname "$tree")" "$(basename "$tree")"
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$archive" >"$archive.sha256"
    else
        shasum -a 256 "$archive" >"$archive.sha256"
    fi
}

run_fixture() {
    local system="$1" machine="$2" archive="$3" prefix="$4" tag="$5"
    local asset="$(basename "$archive")"
    local release_json="${6:-}"
    # Put the digest attachment first to prove the installer matches the
    # complete asset name instead of accepting an archive-name prefix.
    if [[ -z "$release_json" ]]; then
        release_json="{\"tag_name\":\"$tag\",\"assets\":[{\"name\":\"$asset.sha256\",\"browser_download_url\":\"https://fixture.invalid/$asset.sha256\"},{\"name\":\"$asset\",\"browser_download_url\":\"https://fixture.invalid/$asset\"}]}"
    fi
    CAPSID_FAKE_UNAME_S="$system" \
    CAPSID_FAKE_UNAME_M="$machine" \
    CAPSID_FAKE_ASSET_DIR="$(dirname "$archive")" \
    CAPSID_FAKE_RELEASE_JSON="$release_json" \
    PATH="$fake_bin:$real_path" \
        bash "$installer" --prefix "$prefix" "$tag"
}

# A relative prefix must produce runnable top-level commands, and a later
# version must atomically replace all three commands rather than preserving
# symlinks into the old archive.
linux_one="$work/capsid-0.2.0-linux-musl.tar.gz"
linux_two="$work/capsid-0.2.1-linux-musl.tar.gz"
make_package one "$linux_one" ""
make_package two "$linux_two" ""
(
    cd "$work"
    run_fixture Linux x86_64 "$linux_one" relative-prefix v0.2.0-rc.06
)
[[ -x "$work/relative-prefix/bin/capsid-host" ]]
[[ "$($work/relative-prefix/bin/capsid-host)" == "one-capsid-host" ]]
(
    cd "$work"
    run_fixture Linux x86_64 "$linux_two" relative-prefix v0.2.0-rc.07
)
for tool in capsid-host capsid-worker capsid-bytecode-compile; do
    [[ -x "$work/relative-prefix/bin/$tool" ]]
    [[ "$($work/relative-prefix/bin/$tool)" == "two-$tool" ]]
done

# Windows packages contain .exe names. The installed PATH surface must keep
# that native suffix and expose all required tools.
windows="$work/capsid-0.2.0-windows-x86_64.zip"
make_package windows "$windows" .exe
run_fixture MINGW64_NT-10.0 x86_64 "$windows" "$work/windows-prefix" \
    v0.2.0-rc.07
for tool in capsid-host capsid-worker capsid-bytecode-compile; do
    [[ -f "$work/windows-prefix/bin/$tool.exe" ]]
done

# A release missing one required binary is never reported as installed.
mkdir -p "$work/incomplete-asset"
incomplete="$work/incomplete-asset/capsid-0.2.0-linux-musl.tar.gz"
make_package incomplete "$incomplete" "" capsid-bytecode-compile
if run_fixture Linux x86_64 "$incomplete" "$work/incomplete-prefix" \
    v0.2.0-rc.07; then
    echo "FAIL: install.sh accepted an archive missing a required tool" >&2
    exit 1
fi

# New releases have explicit Linux architectures. Keep the old x86_64 asset
# first and ARM64 before x86_64 to detect broad/first-match-only selection.
linux_x86="$work/capsid-0.3.0-linux-musl-x86_64.tar.gz"
linux_arm="$work/capsid-0.3.0-linux-musl-arm64.tar.gz"
make_package x86 "$linux_x86" ""
make_package arm "$linux_arm" ""
darwin_x86="$work/capsid-0.3.0-darwin-x86_64.tar.gz"
darwin_arm="$work/capsid-0.3.0-darwin-arm64.tar.gz"
make_package mac-x86 "$darwin_x86" ""
make_package mac-arm "$darwin_arm" ""
mixed_json="{\"tag_name\":\"v0.3.0\",\"assets\":["
for archive in "$linux_one" "$linux_arm" "$darwin_arm" "$linux_x86" "$darwin_x86"; do
    asset="$(basename "$archive")"
    mixed_json="${mixed_json}{\"browser_download_url\":\"https://fixture.invalid/$asset.sha256\"},{\"browser_download_url\":\"https://fixture.invalid/$asset\"},"
done
mixed_json="${mixed_json%,}]}"
for machine in x86_64 amd64; do
    run_fixture Linux "$machine" "$linux_x86" "$work/$machine-prefix" v0.3.0 "$mixed_json"
    [[ "$("$work/$machine-prefix/bin/capsid-host")" == "x86-capsid-host" ]]
done
for machine in aarch64 arm64; do
    run_fixture Linux "$machine" "$linux_arm" "$work/$machine-prefix" v0.3.0 "$mixed_json"
    [[ "$("$work/$machine-prefix/bin/capsid-host")" == "arm-capsid-host" ]]
done

# A mixed release must select the matching macOS CPU as well, irrespective
# of the earlier Linux and other macOS architecture attachments.
for machine in x86_64 amd64; do
    prefix="$work/darwin-$machine-prefix"
    run_fixture Darwin "$machine" "$darwin_x86" "$prefix" v0.3.0 "$mixed_json"
    [[ "$("$prefix/bin/capsid-host")" == "mac-x86-capsid-host" ]]
done
for machine in arm64 aarch64; do
    prefix="$work/darwin-$machine-prefix"
    run_fixture Darwin "$machine" "$darwin_arm" "$prefix" v0.3.0 "$mixed_json"
    [[ "$("$prefix/bin/capsid-host")" == "mac-arm-capsid-host" ]]
done
prefix="$work/darwin-existing-prefix"
mkdir -p "$prefix/bin"
printf 'existing installation\n' >"$prefix/bin/capsid-host"
log="$work/reject-darwin-x86.log"
if CAPSID_FAKE_REQUEST_LOG="$log" run_fixture Darwin x86_64 "$darwin_arm" "$prefix" v0.3.0; then
    echo "FAIL: Intel macOS accepted an ARM64-only release" >&2
    exit 1
fi
[[ "$(<"$prefix/bin/capsid-host")" == 'existing installation' ]]
if grep -q 'https://fixture.invalid/' "$log"; then
    echo "FAIL: macOS downloaded an incompatible architecture" >&2
    exit 1
fi

# ARM64 cannot fall back to a legacy x86_64 archive, and 32-bit/unknown
# architectures cannot be mislabeled as either supported 64-bit target.
for machine in aarch64 arm64 i686 riscv64; do
    log="$work/reject-$machine.log"
    if CAPSID_FAKE_REQUEST_LOG="$log" run_fixture Linux "$machine" "$linux_one" \
        "$work/reject-$machine" v0.2.0-rc.07; then
        echo "FAIL: install.sh accepted an incompatible Linux package: $machine" >&2
        exit 1
    fi
    if grep -q 'https://fixture.invalid/' "$log"; then
        echo "FAIL: install.sh downloaded an incompatible architecture" >&2
        exit 1
    fi
done

echo "PASS: installer legacy compatibility, explicit CPU selection and rejection controls"
