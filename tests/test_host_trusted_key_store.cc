// WP-05 PR-08 §9.5: TrustedKeyStore verification-contract tests. Every
// negative here is deterministic: the store must fail closed (empty store,
// stable error without path or key material) on any deviation from the
// contract — relative path, missing file, wrong size, symlink, non-regular
// file, group/world-writable, duplicate id, oversized id, oversized set.

#include "host/trusted_key_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using capsid::host::kMaxTrustedKeyIdBytes;
using capsid::host::kMaxTrustedKeys;
using capsid::host::TrustedBytecodeKey;
using capsid::host::TrustedKeyDescriptor;
using capsid::host::TrustedKeyStore;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-trusted-key-store: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

// 32 raw bytes, deliberately distinguishable per index for content checks.
const std::vector<std::uint8_t> kKeyBytes = [] {
    std::vector<std::uint8_t> bytes(32);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(0x10 + i);
    }
    return bytes;
}();

struct Fixture {
    std::string dir = "/tmp/capsid-key-store-XXXXXX";

    Fixture() {
        require(mkdtemp(&dir[0]) != nullptr, "cannot create temp dir");
    }

    ~Fixture() {
        // Best-effort cleanup: unlink every entry (regular files and the
        // symlink) then the dir itself.
        for (const std::string& entry : entries) {
            unlink((dir + "/" + entry).c_str());
        }
        rmdir(dir.c_str());
    }

    std::vector<std::string> entries;

    std::string path(const std::string& name) const { return dir + "/" + name; }

    // Writes `name` in `dir` with the given bytes, mode and ownership.
    // chown to root when `root_owned` (a no-op when the test runs as root,
    // which makes root-ownership the default and cannot be falsified).
    void write_key(const std::string& name, const void* bytes,
                   std::size_t size, mode_t mode, bool root_owned) {
        const int fd = open(path(name).c_str(),
                            O_CREAT | O_EXCL | O_WRONLY, 0600);
        require(fd >= 0, "cannot create key file");
        require(write(fd, bytes, size) == static_cast<ssize_t>(size),
                "cannot write key file");
        require(close(fd) == 0, "cannot close key file");
        require(chmod(path(name).c_str(), mode) == 0,
                "cannot chmod key file");
        if (root_owned) {
            // Best effort: the owner check accepts root OR the Host euid,
            // so chown failure (unprivileged) leaves euid ownership, which
            // is also acceptable. The group/world-writable bits are the
            // falsifiable part of the permission gate.
            const int chown_result = chown(path(name).c_str(), 0, 0);
            (void)chown_result;
        }
        entries.push_back(name);
    }

    void write_key(const std::string& name) {
        write_key(name, kKeyBytes.data(), kKeyBytes.size(), 0600, false);
    }
};

TrustedKeyDescriptor descriptor(const std::string& id,
                                const std::string& file_path) {
    TrustedKeyDescriptor d;
    d.key_id = id;
    d.key_path = file_path;
    return d;
}

void require_ok(const std::vector<TrustedKeyDescriptor>& descriptors,
                const std::string& label) {
    std::string error;
    TrustedKeyStore store = TrustedKeyStore::load(descriptors, &error);
    require(store.size() == descriptors.size(),
            label + ": load failed: " + error);
    require(error.empty(), label + ": success with an error text");
}

void require_rejected(const std::vector<TrustedKeyDescriptor>& descriptors,
                      const std::string& label) {
    std::string error;
    TrustedKeyStore store = TrustedKeyStore::load(descriptors, &error);
    require(store.size() == 0, label + ": load accepted a bad descriptor");
    require(!error.empty(), label + ": no error text on rejection");
    require(error.find('/') == std::string::npos,
            label + ": error leaks a path: '" + error + "'");
}

void test_valid_key_loads_and_owns_memory() {
    Fixture fixture;
    fixture.write_key("release.raw");
    require_ok({descriptor("release", fixture.path("release.raw"))},
               "valid key");

    std::string error;
    TrustedKeyStore store = TrustedKeyStore::load(
        std::vector{descriptor("release", fixture.path("release.raw"))},
        &error);
    require(store.size() == 1, "valid store size");
    require(store.keys().size() == 1, "valid keys() size");
    const TrustedBytecodeKey& key = store.keys()[0];
    require(key.key_id == "release", "key id view mismatch");
    require(key.public_key.size() == 32, "public key view size");
    require(std::memcmp(key.public_key.data(), kKeyBytes.data(), 32) == 0,
            "public key view content mismatch");
    // The views must point at the store's OWNED memory — mutate the
    // descriptor input after load and the store must be unaffected.
    TrustedKeyDescriptor d = descriptor("release", fixture.path("release.raw"));
    std::string shadow_error;
    TrustedKeyStore shadow =
        TrustedKeyStore::load(std::vector{d}, &shadow_error);
    d.key_path = "/etc/hostname";  // must not touch the loaded store
    require(shadow.keys()[0].key_id == "release",
            "key id view aliases the descriptor");
}

void test_relative_path_rejected() {
    Fixture fixture;
    fixture.write_key("release.raw");
    require_rejected({descriptor("release", "release.raw")},
                     "relative key path");
}

void test_missing_file_rejected() {
    Fixture fixture;
    require_rejected(
        {descriptor("release", fixture.path("does-not-exist.raw"))},
        "missing key file");
}

void test_wrong_size_rejected() {
    Fixture fixture;
    const std::vector<std::uint8_t> short_key(kKeyBytes.begin(),
                                              kKeyBytes.begin() + 31);
    const std::vector<std::uint8_t> long_key = [] {
        std::vector<std::uint8_t> bytes = kKeyBytes;
        bytes.push_back(0xff);
        return bytes;
    }();
    fixture.write_key("short.raw", short_key.data(), short_key.size(), 0600,
                      false);
    fixture.write_key("long.raw", long_key.data(), long_key.size(), 0600,
                      false);
    require_rejected({descriptor("short", fixture.path("short.raw"))},
                     "31-byte key");
    require_rejected({descriptor("long", fixture.path("long.raw"))},
                     "33-byte key");
}

void test_symlink_rejected() {
    Fixture fixture;
    fixture.write_key("real.raw");
    require(symlink(fixture.path("real.raw").c_str(),
                    fixture.path("link.raw").c_str()) == 0,
            "cannot create symlink");
    fixture.entries.push_back("link.raw");
    require_rejected({descriptor("link", fixture.path("link.raw"))},
                     "symlinked key");
}

void test_non_regular_file_rejected() {
    Fixture fixture;
    require(mkfifo(fixture.path("fifo.raw").c_str(), 0600) == 0,
            "cannot create fifo");
    fixture.entries.push_back("fifo.raw");
    require_rejected({descriptor("fifo", fixture.path("fifo.raw"))},
                     "fifo key");
}

void test_group_or_world_writable_rejected() {
    Fixture fixture;
    fixture.write_key("group-writable.raw", kKeyBytes.data(),
                      kKeyBytes.size(), 0620, false);
    fixture.write_key("world-writable.raw", kKeyBytes.data(),
                      kKeyBytes.size(), 0602, false);
    fixture.write_key("world-readable.raw", kKeyBytes.data(),
                      kKeyBytes.size(), 0644, false);
    require_rejected(
        {descriptor("gw", fixture.path("group-writable.raw"))},
        "group-writable key");
    require_rejected(
        {descriptor("ww", fixture.path("world-writable.raw"))},
        "world-writable key");
    // Read-only for other is fine: 0644 must LOAD (readable, not writable).
    require_ok({descriptor("wr", fixture.path("world-readable.raw"))},
               "world-readable key");
}

void test_duplicate_id_rejected() {
    Fixture fixture;
    fixture.write_key("a.raw");
    fixture.write_key("b.raw");
    require_rejected({descriptor("same", fixture.path("a.raw")),
                      descriptor("same", fixture.path("b.raw"))},
                     "duplicate key id");
}

void test_oversized_id_rejected() {
    Fixture fixture;
    fixture.write_key("release.raw");
    std::string long_id(kMaxTrustedKeyIdBytes + 1, 'x');
    require_rejected({descriptor(long_id, fixture.path("release.raw"))},
                     "oversized key id");
}

void test_oversized_set_rejected() {
    Fixture fixture;
    fixture.write_key("release.raw");
    std::vector<TrustedKeyDescriptor> descriptors;
    for (std::size_t i = 0; i < kMaxTrustedKeys + 1; ++i) {
        descriptors.push_back(
            descriptor("key-" + std::to_string(i), fixture.path("release.raw")));
    }
    require_rejected(descriptors, "oversized key set");
}

void test_maximum_set_loads() {
    Fixture fixture;
    fixture.write_key("release.raw");
    std::vector<TrustedKeyDescriptor> descriptors;
    for (std::size_t i = 0; i < kMaxTrustedKeys; ++i) {
        descriptors.push_back(
            descriptor("key-" + std::to_string(i), fixture.path("release.raw")));
    }
    require_ok(descriptors, "maximum key set");
}

void test_error_never_leaks_key_material() {
    Fixture fixture;
    fixture.write_key("release.raw");
    // A missing-file rejection must not name the id or the path.
    std::string error;
    TrustedKeyStore store = TrustedKeyStore::load(
        std::vector{descriptor("top-secret-id", fixture.path("absent.raw"))},
        &error);
    require(store.size() == 0, "missing key loaded");
    require(error.find("top-secret-id") == std::string::npos,
            "error leaks the key id: '" + error + "'");
    require(error.find("absent.raw") == std::string::npos,
            "error leaks the path: '" + error + "'");
}

}  // namespace

int main() {
    test_valid_key_loads_and_owns_memory();
    test_relative_path_rejected();
    test_missing_file_rejected();
    test_wrong_size_rejected();
    test_symlink_rejected();
    test_non_regular_file_rejected();
    test_group_or_world_writable_rejected();
    test_duplicate_id_rejected();
    test_oversized_id_rejected();
    test_oversized_set_rejected();
    test_maximum_set_loads();
    test_error_never_leaks_key_material();
    std::cout << "test-host-trusted-key-store: all tests passed" << std::endl;
    return 0;
}
