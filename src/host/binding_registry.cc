// bindingsRoot security scan (docs/binding-technical-design.md §2.1).
//
// The scan is fd-relative from one no-follow root descriptor. Directory
// entries, opened objects, final names, and directory contents are checked
// against the same dev/ino snapshots, so rename/replace races fail closed.

#include "win32_compat.h"

#include "host/binding_registry.h"

#include "host/config.h"
#include "host/generation_identity.h"

#include <errno.h>
#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace capsid::host {
#if !defined(_WIN32)
namespace {

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

bool uid_allowed(uid_t uid, const std::vector<uid_t> &allowed_uids) {
    return std::find(allowed_uids.begin(), allowed_uids.end(), uid) !=
           allowed_uids.end();
}

bool same_identity(const struct stat &left, const struct stat &right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

long mtime_nsec(const struct stat &st) {
#if defined(__APPLE__)
    return st.st_mtimespec.tv_nsec;
#else
    return st.st_mtim.tv_nsec;
#endif
}

long ctime_nsec(const struct stat &st) {
#if defined(__APPLE__)
    return st.st_ctimespec.tv_nsec;
#else
    return st.st_ctim.tv_nsec;
#endif
}

bool same_file_version(const struct stat &left, const struct stat &right) {
    return same_identity(left, right) && left.st_mode == right.st_mode &&
           left.st_uid == right.st_uid && left.st_gid == right.st_gid &&
           left.st_nlink == right.st_nlink && left.st_size == right.st_size &&
           left.st_mtime == right.st_mtime &&
           mtime_nsec(left) == mtime_nsec(right) &&
           left.st_ctime == right.st_ctime &&
           ctime_nsec(left) == ctime_nsec(right);
}

bool validate_stat(const std::string &path,
                   const struct stat &st,
                   bool require_directory,
                   const std::vector<uid_t> &allowed_uids,
                   std::string *error) {
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
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        *error = path + " is group- or world-writable";
        return false;
    }
    return true;
}

bool lstat_checked(const std::string &path,
                   bool require_directory,
                   const std::vector<uid_t> &allowed_uids,
                   struct stat *out,
                   std::string *error) {
    if (lstat(path.c_str(), out) != 0) {
        *error = path + " cannot be stat'ed";
        return false;
    }
    return validate_stat(path, *out, require_directory, allowed_uids, error);
}

bool fstatat_checked(int parent_fd,
                     const std::string &name,
                     const std::string &path,
                     bool require_directory,
                     const std::vector<uid_t> &allowed_uids,
                     struct stat *out,
                     std::string *error) {
    if (fstatat(parent_fd, name.c_str(), out, AT_SYMLINK_NOFOLLOW) != 0) {
        *error = path + " cannot be stat'ed";
        return false;
    }
    return validate_stat(path, *out, require_directory, allowed_uids, error);
}

bool enumerate_directory(int fd,
                         const std::string &path,
                         std::vector<std::string> *out,
                         std::string *error) {
    // dup() would share the directory offset with `fd`, making a later
    // verification enumeration start at EOF. Opening "." relative to the
    // directory creates an independent open-file description.
    const int enumeration_fd =
        openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (enumeration_fd < 0) {
        *error = path + " cannot be reopened for enumeration";
        return false;
    }
    DIR *dir = fdopendir(enumeration_fd);
    if (dir == nullptr) {
        close(enumeration_fd);
        *error = path + " cannot be opened for enumeration";
        return false;
    }
    out->clear();
    errno = 0;
    for (struct dirent *entry = readdir(dir); entry != nullptr;
         entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        out->push_back(entry->d_name);
    }
    const int read_error = errno;
    closedir(dir);
    if (read_error != 0) {
        *error = path + " changed or failed during enumeration";
        return false;
    }
    std::sort(out->begin(), out->end());
    return true;
}

bool read_whole_file_at(int parent_fd,
                        const std::string &name,
                        const std::string &path,
                        std::size_t limit,
                        const struct stat &expected,
                        std::string *out,
                        std::string *error) {
    ScopedFd fd(openat(parent_fd, name.c_str(),
                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.get() < 0) {
        *error = path + " cannot be opened";
        return false;
    }
    struct stat opened;
    if (fstat(fd.get(), &opened) != 0 || !S_ISREG(opened.st_mode)) {
        *error = path + " is not a regular file";
        return false;
    }
    if (!same_file_version(opened, expected)) {
        *error = path + " changed between scan and read";
        return false;
    }
    if (opened.st_size < 0 ||
        static_cast<std::uint64_t>(opened.st_size) > limit) {
        *error = path + " exceeds the size limit";
        return false;
    }
    std::string content(static_cast<std::size_t>(opened.st_size), '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t count =
            read(fd.get(), content.data() + offset, content.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            *error = path + " cannot be read";
            return false;
        }
        if (count == 0) {
            *error = path + " shrank while being read";
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    char extra = '\0';
    ssize_t extra_count;
    do {
        extra_count = read(fd.get(), &extra, 1);
    } while (extra_count < 0 && errno == EINTR);
    if (extra_count < 0) {
        *error = path + " cannot be read";
        return false;
    }
    if (extra_count != 0) {
        *error = path + " grew while being read";
        return false;
    }
    struct stat after_read;
    struct stat final_name;
    if (fstat(fd.get(), &after_read) != 0 ||
        !same_file_version(opened, after_read) ||
        fstatat(parent_fd, name.c_str(), &final_name,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_file_version(opened, final_name)) {
        *error = path + " changed while being read";
        return false;
    }
    *out = std::move(content);
    return true;
}

bool scan_bindings_root_impl(const std::string &root,
                             const std::vector<BindingOwnerId> &allowed_uids,
                             const BindingRegistryScanHook &hook,
                             BindingRegistrySnapshot *out,
                             std::string *error) {
    if (out == nullptr || error == nullptr) {
        return false;
    }
    out->packages.clear();
    error->clear();

    struct stat root_path_stat;
    if (!lstat_checked(root, true, allowed_uids, &root_path_stat, error)) {
        return false;
    }
    ScopedFd root_fd(open(root.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (root_fd.get() < 0) {
        *error = root + " cannot be opened";
        return false;
    }
    struct stat root_opened_stat;
    if (fstat(root_fd.get(), &root_opened_stat) != 0 ||
        !validate_stat(root, root_opened_stat, true, allowed_uids, error) ||
        !same_file_version(root_path_stat, root_opened_stat)) {
        if (error->empty()) {
            *error = root + " changed between scan and open";
        }
        return false;
    }

    std::vector<std::string> entries;
    if (!enumerate_directory(root_fd.get(), root, &entries, error)) {
        return false;
    }
    if (hook) {
        hook(BindingRegistryScanPhase::kRootEnumerated, {});
    }

    std::uint64_t total_source_bytes = 0;
    std::vector<BindingPackageSnapshot> packages;
    packages.reserve(entries.size());
    for (const std::string &name : entries) {
        const std::string package_path = root + "/" + name;
        if (!valid_binding_id(name)) {
            *error = package_path +
                     " is not a valid binding package directory name";
            return false;
        }
        struct stat package_name_stat;
        if (!fstatat_checked(root_fd.get(), name, package_path, true,
                             allowed_uids, &package_name_stat, error)) {
            return false;
        }
        ScopedFd package_fd(openat(root_fd.get(), name.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                       O_NOFOLLOW));
        if (package_fd.get() < 0) {
            *error = package_path + " cannot be opened";
            return false;
        }
        struct stat package_opened_stat;
        if (fstat(package_fd.get(), &package_opened_stat) != 0 ||
            !same_file_version(package_name_stat, package_opened_stat)) {
            *error = package_path + " changed between scan and open";
            return false;
        }

        std::vector<std::string> files;
        if (!enumerate_directory(package_fd.get(), package_path, &files,
                                 error)) {
            return false;
        }
        if (hook) {
            hook(BindingRegistryScanPhase::kPackageEnumerated, name);
        }
        static const std::vector<std::string> kExpectedFiles = {
            "index.js", "manifest.json"};
        if (files != kExpectedFiles) {
            for (const std::string &file : files) {
                if (file != "index.js" && file != "manifest.json") {
                    *error = package_path + "/" + file +
                             " is an unexpected entry";
                    return false;
                }
            }
            if (!std::binary_search(files.begin(), files.end(),
                                    "manifest.json")) {
                *error = package_path + "/manifest.json is missing";
            } else {
                *error = package_path + "/index.js is missing";
            }
            return false;
        }

        struct stat manifest_stat;
        struct stat source_stat;
        if (!fstatat_checked(package_fd.get(), "manifest.json",
                             package_path + "/manifest.json", false,
                             allowed_uids, &manifest_stat, error) ||
            !fstatat_checked(package_fd.get(), "index.js",
                             package_path + "/index.js", false,
                             allowed_uids, &source_stat, error)) {
            return false;
        }
        if (manifest_stat.st_size < 0 ||
            static_cast<std::uint64_t>(manifest_stat.st_size) >
                kMaxBindingManifestBytes) {
            *error = package_path + "/manifest.json exceeds the size limit";
            return false;
        }
        if (source_stat.st_size < 0 ||
            static_cast<std::uint64_t>(source_stat.st_size) >
                kMaxBindingSourceBytes) {
            *error = package_path + "/index.js exceeds the size limit";
            return false;
        }
        const std::uint64_t source_size =
            static_cast<std::uint64_t>(source_stat.st_size);
        if (source_size > kMaxBindingGenerationSourceBytes -
                              total_source_bytes) {
            *error = root + " exceeds the aggregate source size limit";
            return false;
        }

        BindingPackageSnapshot snapshot;
        snapshot.id = name;
        if (!read_whole_file_at(package_fd.get(), "manifest.json",
                                package_path + "/manifest.json",
                                kMaxBindingManifestBytes, manifest_stat,
                                &snapshot.manifest_json, error) ||
            !read_whole_file_at(package_fd.get(), "index.js",
                                package_path + "/index.js",
                                kMaxBindingSourceBytes, source_stat,
                                &snapshot.source, error)) {
            return false;
        }
        total_source_bytes += source_size;

        std::vector<std::string> files_after;
        struct stat package_name_after;
        if (!enumerate_directory(package_fd.get(), package_path,
                                 &files_after, error) ||
            files_after != files ||
            fstatat(root_fd.get(), name.c_str(), &package_name_after,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !same_file_version(package_opened_stat, package_name_after)) {
            *error = package_path + " changed during scan";
            return false;
        }

        const ConfigValidationResult validation =
            validate_binding_manifest(snapshot.manifest_json);
        if (!validation.ok) {
            *error = package_path +
                     "/manifest.json is not a valid binding manifest: " +
                     validation.error.message;
            return false;
        }
        snapshot.manifest_digest =
            compute_binding_manifest_digest(snapshot.manifest_json);
        snapshot.source_digest = sha256_hex(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(snapshot.source.data()),
                snapshot.source.size()));
        packages.push_back(std::move(snapshot));
    }

    std::vector<std::string> entries_after;
    struct stat root_path_after;
    if (!enumerate_directory(root_fd.get(), root, &entries_after, error) ||
        entries_after != entries ||
        lstat(root.c_str(), &root_path_after) != 0 ||
        !same_file_version(root_opened_stat, root_path_after)) {
        *error = root + " changed during scan";
        return false;
    }

    out->packages = std::move(packages);
    return true;
}

}  // namespace
#endif

bool scan_bindings_root(const std::string &root,
                        const std::vector<BindingOwnerId> &allowed_uids,
                        BindingRegistrySnapshot *out,
                        std::string *error) {
#if defined(_WIN32)
    (void)allowed_uids;
    if (out != nullptr) {
        out->packages.clear();
    }
    if (error != nullptr) {
        *error = root +
                 " cannot be used as bindingsRoot on Windows (secure "
                 "reparse-point/ACL scanning is not implemented)";
    }
    return false;
#else
    return scan_bindings_root_impl(root, allowed_uids, {}, out, error);
#endif
}

bool scan_bindings_root_with_test_hook(
    const std::string &root,
    const std::vector<BindingOwnerId> &allowed_uids,
    const BindingRegistryScanHook &hook,
    BindingRegistrySnapshot *out,
    std::string *error) {
#if defined(_WIN32)
    (void)hook;
    return scan_bindings_root(root, allowed_uids, out, error);
#else
    return scan_bindings_root_impl(root, allowed_uids, hook, out, error);
#endif
}

}  // namespace capsid::host
