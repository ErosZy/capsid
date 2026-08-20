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
if [[ -z "$output" ]]; then
    printf '%s\n' "${CAPSID_FAKE_RELEASE_JSON:?}"
elif [[ "$url" == *.sha256 ]]; then
    cp "${CAPSID_FAKE_ARCHIVE_SHA:?}" "$output"
else
    cp "${CAPSID_FAKE_ARCHIVE:?}" "$output"
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
    # Put the digest attachment first to prove the installer matches the
    # complete asset name instead of accepting an archive-name prefix.
    CAPSID_FAKE_UNAME_S="$system" \
    CAPSID_FAKE_UNAME_M="$machine" \
    CAPSID_FAKE_ARCHIVE="$archive" \
    CAPSID_FAKE_ARCHIVE_SHA="$archive.sha256" \
    CAPSID_FAKE_RELEASE_JSON="{\"tag_name\":\"$tag\",\"assets\":[{\"name\":\"$asset.sha256\",\"browser_download_url\":\"https://fixture.invalid/$asset.sha256\"},{\"name\":\"$asset\",\"browser_download_url\":\"https://fixture.invalid/$asset\"}]}" \
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

# The release workflow currently publishes only x86_64 Linux. A different
# CPU must fail before downloading an incompatible executable.
if run_fixture Linux aarch64 "$linux_one" "$work/arm-prefix" \
    v0.2.0-rc.07; then
    echo "FAIL: install.sh accepted an unsupported Linux architecture" >&2
    exit 1
fi

echo "PASS: install.sh stages, validates and upgrades every platform fixture"
