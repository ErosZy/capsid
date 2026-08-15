// bindingsRoot security scan (docs/binding-technical-design.md §2.1).
//
// Every lstat is a no-follow check on the entry itself; nothing in the
// scanned tree is ever opened through a symbolic link. Each package must be
// a plain directory named by the Binding ID grammar and containing exactly
// manifest.json and index.js, both single-link regular files. Owners must
// be in the allowed-uid set and nothing may be group- or world-writable.
// File sizes are bounded before any byte is read, and every manifest is
// validated against the binding manifest schema. The returned snapshot
// copies all bytes, so later edits to bindingsRoot cannot change it (v1
// does not watch or reload).

#include "host/binding_registry.h"

#include "host/config.h"
#include "host/generation_identity.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace capsid::host {
namespace {

bool uid_allowed(uid_t uid, const std::vector<uid_t> &allowed_uids) {
    return std::find(allowed_uids.begin(), allowed_uids.end(), uid) !=
           allowed_uids.end();
}

bool is_group_or_world_writable(mode_t mode) {
    return (mode & (S_IWGRP | S_IWOTH)) != 0;
}

// lstat-level check shared by the root, package directories and files.
bool check_entry(const std::string &path,
                 bool require_directory,
                 const std::vector<uid_t> &allowed_uids,
                 std::string *error) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        *error = path + " cannot be stat'ed";
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        *error = path + " is a symbolic link";
        return false;
    }
    if (require_directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) {
        *error = require_directory ? path + " is not a directory"
                                   : path + " is not a regular file";
        return false;
    }
    if (!require_directory && st.st_nlink != 1) {
        *error = path + " is a hard link";
        return false;
    }
    if (!uid_allowed(st.st_uid, allowed_uids)) {
        *error = path + " has a disallowed owner";
        return false;
    }
    if (is_group_or_world_writable(st.st_mode)) {
        *error = path + " is group- or world-writable";
        return false;
    }
    return true;
}

// Bounded whole-file read; st_size was already checked against `limit`, and
// a shrink or growth during the read fails closed so the snapshot bytes are
// always exactly one stable file version.
bool read_whole_file(const std::string &path,
                     std::size_t limit,
                     std::string *out,
                     std::string *error) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        *error = path + " cannot be opened";
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *error = path + " is not a regular file";
        return false;
    }
    if (static_cast<std::uint64_t>(st.st_size) > limit) {
        close(fd);
        *error = path + " exceeds the size limit";
        return false;
    }
    std::string content(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t n = read(fd, content.data() + offset,
                               content.size() - offset);
        if (n < 0) {
            close(fd);
            *error = path + " cannot be read";
            return false;
        }
        if (n == 0) {
            close(fd);
            *error = path + " shrank while being read";
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    char extra = '\0';
    if (read(fd, &extra, 1) != 0) {
        close(fd);
        *error = path + " grew while being read";
        return false;
    }
    close(fd);
    *out = std::move(content);
    return true;
}

}  // namespace

bool scan_bindings_root(const std::string &root,
                        const std::vector<uid_t> &allowed_uids,
                        BindingRegistrySnapshot *out,
                        std::string *error) {
    if (out == nullptr || error == nullptr) {
        return false;
    }
    out->packages.clear();

    const auto fail_with = [error](std::string message) {
        *error = std::move(message);
        return false;
    };

    if (!check_entry(root, /*require_directory=*/true, allowed_uids, error)) {
        return false;
    }

    DIR *root_dir = opendir(root.c_str());
    if (root_dir == nullptr) {
        return fail_with(root + " cannot be opened");
    }
    std::vector<std::string> entries;
    for (struct dirent *entry = readdir(root_dir); entry != nullptr;
         entry = readdir(root_dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        entries.push_back(entry->d_name);
    }
    closedir(root_dir);
    // Deterministic iteration so the first reported error never depends on
    // readdir order.
    std::sort(entries.begin(), entries.end());

    std::vector<BindingPackageSnapshot> packages;
    for (const std::string &name : entries) {
        const std::string package_path = root + "/" + name;
        if (!valid_binding_id(name)) {
            return fail_with(package_path +
                             " is not a valid binding package directory name");
        }
        if (!check_entry(package_path, /*require_directory=*/true,
                         allowed_uids, error)) {
            return false;
        }

        DIR *package_dir = opendir(package_path.c_str());
        if (package_dir == nullptr) {
            return fail_with(package_path + " cannot be opened");
        }
        std::vector<std::string> files;
        for (struct dirent *entry = readdir(package_dir); entry != nullptr;
             entry = readdir(package_dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            files.push_back(entry->d_name);
        }
        closedir(package_dir);
        std::sort(files.begin(), files.end());

        bool has_manifest = false;
        bool has_index = false;
        for (const std::string &file : files) {
            if (file == "manifest.json") {
                has_manifest = true;
            } else if (file == "index.js") {
                has_index = true;
            } else {
                return fail_with(package_path + "/" + file +
                                 " is an unexpected entry");
            }
            if (!check_entry(package_path + "/" + file,
                             /*require_directory=*/false, allowed_uids,
                             error)) {
                return false;
            }
        }
        if (!has_manifest) {
            return fail_with(package_path + "/manifest.json is missing");
        }
        if (!has_index) {
            return fail_with(package_path + "/index.js is missing");
        }

        BindingPackageSnapshot snapshot;
        snapshot.id = name;
        if (!read_whole_file(package_path + "/manifest.json",
                             kMaxBindingManifestBytes, &snapshot.manifest_json,
                             error)) {
            return false;
        }
        if (!read_whole_file(package_path + "/index.js",
                             kMaxBindingSourceBytes, &snapshot.source,
                             error)) {
            return false;
        }
        const ConfigValidationResult validation =
            validate_binding_manifest(snapshot.manifest_json);
        if (!validation.ok) {
            return fail_with(package_path +
                             "/manifest.json is not a valid binding manifest: " +
                             validation.error.message);
        }
        snapshot.manifest_digest =
            compute_binding_manifest_digest(snapshot.manifest_json);
        snapshot.source_digest = sha256_hex(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(snapshot.source.data()),
                snapshot.source.size()));
        packages.push_back(std::move(snapshot));
    }

    out->packages = std::move(packages);
    return true;
}

}  // namespace capsid::host
