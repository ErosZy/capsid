// bindingsRoot security scan (docs/binding-technical-design.md §2.1).
//
// The Host scans bindingsRoot once at startup and forms an immutable
// snapshot. The scan rejects symbolic links, hard links, FIFOs, sockets,
// device files, extra files, disallowed owners, group/world-writable modes,
// oversized artifacts and invalid package directory names. The allowed-uid
// set is injectable so the owner rule is testable without root.

#include "host/binding_registry.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using capsid::host::BindingRegistrySnapshot;
using capsid::host::BindingRegistryScanPhase;
using capsid::host::scan_bindings_root;
using capsid::host::scan_bindings_root_with_test_hook;

constexpr char kValidManifest[] =
    R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client"]},"permissions":{"modules":["capsid:internal/core","capsid:utils"],"net":{"allow":["*:27017"]}}})json";

[[noreturn]] void fail(const std::string &message) {
    std::cerr << "test-host-binding-registry: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

// Recursive fixture cleanup. The fixture only ever contains regular files,
// directories, symlinks and FIFOs created by the tests themselves.
void remove_tree(const std::string &path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        remove(path.c_str());
        return;
    }
    DIR *dir = opendir(path.c_str());
    require(dir != nullptr, "fixture cleanup: opendir failed");
    std::vector<std::string> names;
    for (struct dirent *entry = readdir(dir); entry != nullptr;
         entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        names.push_back(path + "/" + entry->d_name);
    }
    closedir(dir);
    for (const std::string &name : names) {
        remove_tree(name);
    }
    rmdir(path.c_str());
}

class Fixture {
public:
    explicit Fixture() {
        const char *tmpdir = getenv("TMPDIR");
        const std::string base =
            tmpdir && *tmpdir ? std::string(tmpdir) : std::string("/tmp");
        std::string pattern = base + "/capsid-binding-registry-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char *created = mkdtemp(buffer.data());
        require(created != nullptr, "mkdtemp failed");
        root_ = created;
    }

    ~Fixture() { remove_tree(root_); }

    const std::string &root() const { return root_; }

    std::string package(const std::string &id) const {
        return root_ + "/" + id;
    }

    void write(const std::string &path,
               const std::string &content,
               mode_t mode = 0644) const {
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
        require(fd >= 0, "fixture write: open failed for " + path);
        const ssize_t written =
            ::write(fd, content.data(), content.size());
        require(written == static_cast<ssize_t>(content.size()),
                "fixture write: short write for " + path);
        close(fd);
        if (mode != 0644) {
            require(chmod(path.c_str(), mode) == 0,
                    "fixture write: chmod failed for " + path);
        }
    }

    void write_sized(const std::string &path, off_t size) const {
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        require(fd >= 0, "fixture sized write: open failed for " + path);
        require(ftruncate(fd, size) == 0,
                "fixture sized write: ftruncate failed for " + path);
        close(fd);
    }

    void add_valid_package(const std::string &id) const {
        require(mkdir(package(id).c_str(), 0755) == 0,
                "fixture: mkdir failed for " + package(id));
        write(package(id) + "/manifest.json", kValidManifest);
        write(package(id) + "/index.js", "export default () => ({});");
    }

    void require_scan_ok(const std::vector<uid_t> &allowed_uids,
                         const char *label) const {
        BindingRegistrySnapshot snapshot;
        std::string error;
        const bool ok = scan_bindings_root(root_, allowed_uids, &snapshot,
                                           &error);
        require(ok, std::string(label) + " failed: " + error);
        require(error.empty(), std::string(label) + " set an error string");
    }

    void require_scan_fail(const std::vector<uid_t> &allowed_uids,
                           const char *needle,
                           const char *label) const {
        BindingRegistrySnapshot snapshot;
        std::string error;
        const bool ok = scan_bindings_root(root_, allowed_uids, &snapshot,
                                           &error);
        require(!ok, std::string(label) + " succeeded");
        require(error.find(needle) != std::string::npos,
                std::string(label) + " error '" + error +
                    "' does not mention '" + needle + "'");
    }

private:
    std::string root_;
};

std::vector<uid_t> current_uid_only() {
    return {geteuid()};
}

void test_empty_root_scans_clean() {
    Fixture fixture;
    fixture.require_scan_ok(current_uid_only(), "empty bindingsRoot");
    BindingRegistrySnapshot snapshot;
    std::string error;
    require(scan_bindings_root(fixture.root(), current_uid_only(), &snapshot,
                               &error),
            "empty bindingsRoot second scan failed");
    require(snapshot.packages.empty(), "empty bindingsRoot produced packages");
}

void test_valid_packages_snapshot_sorted_with_digests() {
    Fixture fixture;
    fixture.add_valid_package("redis");
    fixture.add_valid_package("mongo");
    BindingRegistrySnapshot snapshot;
    std::string error;
    require(scan_bindings_root(fixture.root(), current_uid_only(), &snapshot,
                               &error),
            "valid bindingsRoot failed: " + error);
    require(snapshot.packages.size() == 2,
            "expected two binding packages");
    require(snapshot.packages[0].id == "mongo" &&
                snapshot.packages[1].id == "redis",
            "package snapshot is not sorted by id");
    const auto &mongo = snapshot.packages[0];
    require(mongo.manifest_json == kValidManifest,
            "manifest snapshot does not match the file bytes");
    require(mongo.source == "export default () => ({});",
            "source snapshot does not match the file bytes");
    require(mongo.manifest_digest.rfind("sha256:", 0) == 0,
            "manifest digest is missing the sha256 prefix");
    require(mongo.source_digest.rfind("sha256:", 0) == 0,
            "source digest is missing the sha256 prefix");
}

void test_symlinks_are_rejected() {
    Fixture fixture;
    fixture.add_valid_package("mongo");
    require(symlink(fixture.root().c_str(),
                    (fixture.root() + "-link").c_str()) == 0,
            "fixture: root symlink failed");
    {
        // The symlinked root must fail even though its target is valid.
        BindingRegistrySnapshot snapshot;
        std::string error;
        const bool ok = scan_bindings_root(fixture.root() + "-link",
                                           current_uid_only(), &snapshot,
                                           &error);
        require(!ok, "symlinked bindingsRoot succeeded");
        require(error.find("symbolic link") != std::string::npos,
                "symlinked bindingsRoot error does not name symbolic links");
    }

    Fixture fixture2;
    fixture2.add_valid_package("target");
    require(symlink(fixture2.package("target").c_str(),
                    fixture2.package("alias").c_str()) == 0,
            "fixture: package symlink failed");
    fixture2.require_scan_fail(current_uid_only(), "symbolic link",
                               "symlinked package directory");

    Fixture fixture3;
    require(mkdir(fixture3.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture3.write(fixture3.package("mongo") + "/index.js",
                   "export default () => ({});");
    fixture3.write(fixture3.root() + "/outside.json", kValidManifest);
    require(symlink((fixture3.root() + "/outside.json").c_str(),
                    (fixture3.package("mongo") + "/manifest.json").c_str()) ==
                0,
            "fixture: manifest symlink failed");
    fixture3.require_scan_fail(current_uid_only(), "symbolic link",
                               "symlinked manifest file");
}

void test_hard_links_are_rejected() {
    Fixture fixture;
    require(mkdir(fixture.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture.write(fixture.package("mongo") + "/manifest.json", kValidManifest);
    fixture.write(fixture.root() + "/outside.js",
                  "export default () => ({});");
    require(link((fixture.root() + "/outside.js").c_str(),
                 (fixture.package("mongo") + "/index.js").c_str()) == 0,
            "fixture: hard link failed");
    fixture.require_scan_fail(current_uid_only(), "hard link",
                              "hard-linked source file");
}

void test_special_files_are_rejected() {
    Fixture fixture;
    require(mkdir(fixture.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture.write(fixture.package("mongo") + "/manifest.json", kValidManifest);
    require(mkfifo((fixture.package("mongo") + "/index.js").c_str(), 0644) == 0,
            "fixture: mkfifo failed");
    fixture.require_scan_fail(current_uid_only(), "regular file",
                              "FIFO in place of index.js");
}

void test_extra_and_missing_files_are_rejected() {
    Fixture fixture;
    fixture.add_valid_package("mongo");
    fixture.write(fixture.package("mongo") + "/extra.txt", "x");
    fixture.require_scan_fail(current_uid_only(), "extra.txt",
                              "extra file in a package");

    Fixture fixture2;
    require(mkdir(fixture2.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture2.write(fixture2.package("mongo") + "/manifest.json", kValidManifest);
    fixture2.require_scan_fail(current_uid_only(), "index.js",
                               "package without index.js");

    Fixture fixture3;
    require(mkdir(fixture3.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture3.write(fixture3.package("mongo") + "/index.js",
                   "export default () => ({});");
    fixture3.require_scan_fail(current_uid_only(), "manifest.json",
                               "package without manifest.json");

    Fixture fixture4;
    require(mkdir(fixture4.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture4.write(fixture4.package("mongo") + "/manifest.json", kValidManifest);
    fixture4.write(fixture4.package("mongo") + "/index.js",
                   "export default () => ({});");
    require(mkdir((fixture4.package("mongo") + "/nested").c_str(), 0755) == 0,
            "fixture: nested mkdir failed");
    fixture4.require_scan_fail(current_uid_only(), "nested",
                               "nested directory in a package");
}

void test_ownership_and_mode_are_enforced() {
    Fixture fixture;
    fixture.add_valid_package("mongo");
    require(chmod(fixture.root().c_str(), 0777) == 0,
            "fixture: root chmod failed");
    fixture.require_scan_fail(current_uid_only(), "writable",
                              "world-writable bindingsRoot");

    Fixture fixture2;
    fixture2.add_valid_package("mongo");
    require(chmod(fixture2.package("mongo").c_str(), 0777) == 0,
            "fixture: package chmod failed");
    fixture2.require_scan_fail(current_uid_only(), "writable",
                               "world-writable package directory");

    Fixture fixture3;
    fixture3.add_valid_package("mongo");
    require(chmod((fixture3.package("mongo") + "/manifest.json").c_str(),
                  0666) == 0,
            "fixture: manifest chmod failed");
    fixture3.require_scan_fail(current_uid_only(), "writable",
                               "world-writable manifest");

    Fixture fixture4;
    fixture4.add_valid_package("mongo");
    require(chmod((fixture4.package("mongo") + "/index.js").c_str(), 0660) ==
                0,
            "fixture: source chmod failed");
    fixture4.require_scan_fail(current_uid_only(), "writable",
                               "group-writable index.js");

    // The allowed-uid set is injectable: an owner outside the set must be
    // rejected even though the file is otherwise well-formed.
    Fixture fixture5;
    fixture5.add_valid_package("mongo");
    fixture5.require_scan_fail({geteuid() + 1}, "owner",
                               "package owned by a disallowed uid");
}

void test_size_limits_are_enforced() {
    Fixture fixture;
    require(mkdir(fixture.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture.write(fixture.package("mongo") + "/manifest.json",
                  std::string(1024U * 1024U + 1, 'x'));
    fixture.write(fixture.package("mongo") + "/index.js",
                  "export default () => ({});");
    fixture.require_scan_fail(current_uid_only(), "size",
                              "oversized manifest");

    Fixture fixture2;
    require(mkdir(fixture2.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture2.write(fixture2.package("mongo") + "/manifest.json", kValidManifest);
    fixture2.write(fixture2.package("mongo") + "/index.js",
                   std::string(16U * 1024U * 1024U + 1, 'x'));
    fixture2.require_scan_fail(current_uid_only(), "size",
                               "oversized index.js");
}

void test_generation_source_limit_is_enforced() {
    Fixture fixture;
    static const char *const ids[] = {"alpha", "bravo", "charlie", "delta"};
    for (const char *id : ids) {
        require(mkdir(fixture.package(id).c_str(), 0755) == 0,
                "fixture: mkdir failed");
        fixture.write(fixture.package(id) + "/manifest.json", kValidManifest);
        fixture.write_sized(fixture.package(id) + "/index.js",
                            16U * 1024U * 1024U);
    }
    require(mkdir(fixture.package("echo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture.write(fixture.package("echo") + "/manifest.json", kValidManifest);
    fixture.write(fixture.package("echo") + "/index.js", "x");
    fixture.require_scan_fail(current_uid_only(), "aggregate source",
                              "generation source bytes above 64 MiB");
}

void test_root_replacement_during_scan_is_rejected() {
    Fixture fixture;
    fixture.add_valid_package("mongo");
    const std::string displaced = fixture.root() + "-displaced";
    bool replaced = false;
    BindingRegistrySnapshot snapshot;
    std::string error;
    const bool ok = scan_bindings_root_with_test_hook(
        fixture.root(), current_uid_only(),
        [&](BindingRegistryScanPhase phase, std::string_view) {
            if (phase != BindingRegistryScanPhase::kRootEnumerated || replaced) {
                return;
            }
            replaced = true;
            require(rename(fixture.root().c_str(), displaced.c_str()) == 0,
                    "fixture: displace root failed");
            require(mkdir(fixture.root().c_str(), 0755) == 0,
                    "fixture: replacement root mkdir failed");
            require(mkdir(fixture.package("mongo").c_str(), 0755) == 0,
                    "fixture: replacement package mkdir failed");
            fixture.write(fixture.package("mongo") + "/manifest.json",
                          kValidManifest);
            fixture.write(fixture.package("mongo") + "/index.js",
                          "export default () => ({});");
        },
        &snapshot, &error);
    remove_tree(fixture.root());
    require(rename(displaced.c_str(), fixture.root().c_str()) == 0,
            "fixture: restore root failed");
    require(replaced, "root replacement hook did not run");
    require(!ok && error.find("changed") != std::string::npos,
            "replacement bindingsRoot was accepted: " + error);
}

void test_package_replacement_during_scan_is_rejected() {
    Fixture fixture;
    fixture.add_valid_package("mongo");
    const std::string displaced = fixture.package("mongo") + "-displaced";
    bool replaced = false;
    BindingRegistrySnapshot snapshot;
    std::string error;
    const bool ok = scan_bindings_root_with_test_hook(
        fixture.root(), current_uid_only(),
        [&](BindingRegistryScanPhase phase, std::string_view package_id) {
            if (phase != BindingRegistryScanPhase::kPackageEnumerated ||
                package_id != "mongo" || replaced) {
                return;
            }
            replaced = true;
            require(rename(fixture.package("mongo").c_str(),
                           displaced.c_str()) == 0,
                    "fixture: displace package failed");
            require(mkdir(fixture.package("mongo").c_str(), 0755) == 0,
                    "fixture: replacement package mkdir failed");
            fixture.write(fixture.package("mongo") + "/manifest.json",
                          kValidManifest);
            fixture.write(fixture.package("mongo") + "/index.js",
                          "export default () => ({});");
        },
        &snapshot, &error);
    remove_tree(fixture.package("mongo"));
    require(rename(displaced.c_str(), fixture.package("mongo").c_str()) == 0,
            "fixture: restore package failed");
    require(replaced, "package replacement hook did not run");
    require(!ok && error.find("changed") != std::string::npos,
            "replacement binding package was accepted: " + error);
}

void test_invalid_package_names_and_stray_files_are_rejected() {
    Fixture fixture;
    fixture.add_valid_package("Mongo");
    fixture.require_scan_fail(current_uid_only(), "Mongo",
                              "uppercase package directory name");

    Fixture fixture2;
    fixture2.add_valid_package("mongo");
    fixture2.write(fixture2.root() + "/stray.txt", "x");
    fixture2.require_scan_fail(current_uid_only(), "stray.txt",
                               "stray file in bindingsRoot");
}

void test_manifest_content_is_validated() {
    Fixture fixture;
    require(mkdir(fixture.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture.write(fixture.package("mongo") + "/manifest.json", "not json");
    fixture.write(fixture.package("mongo") + "/index.js",
                  "export default () => ({});");
    fixture.require_scan_fail(current_uid_only(), "manifest",
                              "unparseable manifest content");

    Fixture fixture2;
    require(mkdir(fixture2.package("mongo").c_str(), 0755) == 0,
            "fixture: mkdir failed");
    fixture2.write(
        fixture2.package("mongo") + "/manifest.json",
        R"json({"apiVersion":"capsid/binding-v9","permissions":{"modules":["capsid:utils"]}})json");
    fixture2.write(fixture2.package("mongo") + "/index.js",
                   "export default () => ({});");
    fixture2.require_scan_fail(current_uid_only(), "manifest",
                               "manifest with an unsupported apiVersion");
}

void test_snapshot_is_immutable() {
    Fixture fixture;
    fixture.add_valid_package("mongo");
    BindingRegistrySnapshot snapshot;
    std::string error;
    require(scan_bindings_root(fixture.root(), current_uid_only(), &snapshot,
                               &error),
            "initial scan failed: " + error);
    const std::string digest_before = snapshot.packages[0].manifest_digest;
    const std::string manifest_before = snapshot.packages[0].manifest_json;
    fixture.write(fixture.package("mongo") + "/manifest.json",
                  R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"]}})json");
    require(snapshot.packages[0].manifest_json == manifest_before,
            "snapshot bytes changed after the file was rewritten");
    require(snapshot.packages[0].manifest_digest == digest_before,
            "snapshot digest changed after the file was rewritten");
}

}  // namespace

int main() {
    test_empty_root_scans_clean();
    test_valid_packages_snapshot_sorted_with_digests();
    test_symlinks_are_rejected();
    test_hard_links_are_rejected();
    test_special_files_are_rejected();
    test_extra_and_missing_files_are_rejected();
    test_ownership_and_mode_are_enforced();
    test_size_limits_are_enforced();
    test_generation_source_limit_is_enforced();
    test_root_replacement_during_scan_is_rejected();
    test_package_replacement_during_scan_is_rejected();
    test_invalid_package_names_and_stray_files_are_rejected();
    test_manifest_content_is_validated();
    test_snapshot_is_immutable();
    return 0;
}
