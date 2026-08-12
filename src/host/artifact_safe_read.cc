// Deployment-input safe-read boundary (M1D). See artifact_safe_read.h.

#include "host/artifact_safe_read.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace capsid::host {
namespace {

SafeReadResult fail_result(SafeReadErrorCode code, const char* message) {
    SafeReadResult result;
    result.code = code;
    result.message = message;
    return result;
}

// Path syntax: relative, non-empty components, no ".", "..", NUL or
// leading slash. The kernel flags additionally forbid traversal, but the
// syntax check keeps error semantics uniform and rejects hostile paths
// before any descriptor work.
bool valid_relative_path(std::string_view path) {
    if (path.empty() || path.front() == '/' ||
        path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::size_t component_end =
            end == std::string_view::npos ? path.size() : end;
        const std::size_t length = component_end - begin;
        if (length == 0) {
            return false;  // empty component (double slash or trailing)
        }
        if (length == 1 && path[begin] == '.') {
            return false;
        }
        if (length == 2 && path[begin] == '.' && path[begin + 1] == '.') {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

#if defined(__APPLE__)
#define CAPSID_STAT_MTIME_SEC(st) ((st).st_mtimespec.tv_sec)
#define CAPSID_STAT_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define CAPSID_STAT_CTIME_SEC(st) ((st).st_ctimespec.tv_sec)
#define CAPSID_STAT_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#else
#define CAPSID_STAT_MTIME_SEC(st) ((st).st_mtim.tv_sec)
#define CAPSID_STAT_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define CAPSID_STAT_CTIME_SEC(st) ((st).st_ctim.tv_sec)
#define CAPSID_STAT_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

void snapshot_identity(const struct stat& st, FileIdentity* identity) {
    identity->dev = static_cast<std::uint64_t>(st.st_dev);
    identity->inode = static_cast<std::uint64_t>(st.st_ino);
    identity->size = static_cast<std::uint64_t>(st.st_size);
    identity->mtime_sec = static_cast<std::int64_t>(CAPSID_STAT_MTIME_SEC(st));
    identity->mtime_nsec = static_cast<std::int64_t>(CAPSID_STAT_MTIME_NSEC(st));
    identity->ctime_sec = static_cast<std::int64_t>(CAPSID_STAT_CTIME_SEC(st));
    identity->ctime_nsec = static_cast<std::int64_t>(CAPSID_STAT_CTIME_NSEC(st));
}

#if defined(__linux__)

int open_beneath(int root_fd, const char* path) {
    struct open_how how = {};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    how.resolve =
        RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    const int fd = static_cast<int>(
        syscall(SYS_openat2, root_fd, path, &how, sizeof(how)));
    if (fd < 0 && errno == ENOSYS) {
        // Linux requires openat2 for deployment reads; an old kernel fails
        // closed rather than falling back to a less strict open.
        errno = ENOSYS;
    }
    return fd;
}

#elif defined(__APPLE__)

// Component-by-component dirfd walk with O_NOFOLLOW at every step.
int open_beneath(int root_fd, const char* path) {
    const std::string relative(path);
    int dir_fd = root_fd;
    std::size_t begin = 0;
    while (begin < relative.size()) {
        const std::size_t end = relative.find('/', begin);
        const std::string component =
            relative.substr(begin, end == std::string::npos
                                       ? std::string::npos
                                       : end - begin);
        const bool last = end == std::string::npos;
        const int next = openat(
            dir_fd, component.c_str(),
            (last ? O_RDONLY : O_RDONLY | O_DIRECTORY) | O_CLOEXEC |
                O_NOFOLLOW | O_NONBLOCK);
        if (next < 0) {
            if (dir_fd != root_fd) {
                close(dir_fd);
            }
            return -1;
        }
        if (dir_fd != root_fd) {
            close(dir_fd);
        }
        if (last) {
            return next;
        }
        dir_fd = next;
        begin = end + 1;
    }
    // Unreachable for validated non-empty relative paths; fail closed.
    return -1;
}

#else
#error "capsid host safe-read requires Linux or macOS"
#endif

SafeReadResult read_regular_file_at(int fd, std::size_t max_bytes) {
    struct stat before = {};
    if (fstat(fd, &before) != 0) {
        return fail_result(SafeReadErrorCode::kIoError,
                           "cannot stat deployment file");
    }
    if (!S_ISREG(before.st_mode)) {
        return fail_result(SafeReadErrorCode::kNotRegularFile,
                           "deployment path is not a regular file");
    }
    const std::uint64_t size = static_cast<std::uint64_t>(before.st_size);
    if (size > max_bytes) {
        return fail_result(SafeReadErrorCode::kOverLimit,
                           "deployment file exceeds the size limit");
    }

    SafeFile file;
    file.bytes.resize(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < file.bytes.size()) {
        const ssize_t count = pread(
            fd, file.bytes.data() + offset, file.bytes.size() - offset,
            static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail_result(SafeReadErrorCode::kIoError,
                               "deployment file read failed");
        }
        if (count == 0) {
            break;  // truncated mid-read; identity check below catches it
        }
        offset += static_cast<std::size_t>(count);
    }
    if (offset != file.bytes.size()) {
        return fail_result(SafeReadErrorCode::kIdentityChanged,
                           "deployment file changed while reading");
    }

    struct stat after = {};
    if (fstat(fd, &after) != 0) {
        return fail_result(SafeReadErrorCode::kIoError,
                           "cannot re-stat deployment file");
    }
    FileIdentity identity;
    snapshot_identity(after, &identity);
    FileIdentity before_identity;
    snapshot_identity(before, &before_identity);
    if (before_identity != identity) {
        return fail_result(SafeReadErrorCode::kIdentityChanged,
                           "deployment file changed while reading");
    }
    file.identity = identity;
    SafeReadResult result;
    result.code = SafeReadErrorCode::kNone;
    result.file = std::move(file);
    return result;
}

}  // namespace

bool valid_app_version_id(std::string_view value,
                          std::size_t max_length) {
    if (value.empty() || value.size() > max_length) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char c = value[index];
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9');
        const bool extra = c == '.' || c == '_' || c == '-';
        if (!alnum && !extra) {
            return false;
        }
        // No empty or ".." component: the first char cannot be a
        // separator, and a '.' must not be followed by '.'.
        if (index == 0 && (c == '.' || c == '_' || c == '-')) {
            return false;
        }
        if (c == '.' && index + 1 < value.size() &&
            value[index + 1] == '.') {
            return false;
        }
    }
    return true;
}

SafeReadResult safe_read_regular_file(int root_fd,
                                      std::string_view relative_path,
                                      std::size_t max_bytes) {
    if (root_fd < 0 || !valid_relative_path(relative_path)) {
        return fail_result(SafeReadErrorCode::kInvalidPath,
                           "invalid deployment relative path");
    }
    const std::string path(relative_path);
    const int fd = open_beneath(root_fd, path.c_str());
    if (fd < 0) {
        if (errno == ENOSYS) {
            return fail_result(SafeReadErrorCode::kIoError,
                               "openat2 is required on Linux");
        }
        if (errno == ENOENT || errno == ENOTDIR) {
            return fail_result(SafeReadErrorCode::kMissingFile,
                               "deployment file does not exist");
        }
        if (errno == ELOOP) {
            return fail_result(SafeReadErrorCode::kNotRegularFile,
                               "deployment path contains a symlink");
        }
        // Socket nodes: Linux open(2) refuses them with ENXIO, but macOS
        // (BSD) reports a different errno for the same node type. Classify
        // by probing the node type instead of trusting either errno, so
        // the mapping is platform-independent (§13.5). The probe is
        // diagnostic only: open already failed, so either way the deploy
        // is rejected fail-closed.
        struct stat node = {};
        if (fstatat(root_fd, path.c_str(), &node, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISSOCK(node.st_mode)) {
            return fail_result(SafeReadErrorCode::kNotRegularFile,
                               "deployment path is not a regular file");
        }
        return fail_result(SafeReadErrorCode::kIoError,
                           "cannot open deployment file");
    }
    SafeReadResult result = read_regular_file_at(fd, max_bytes);
    close(fd);
    return result;
}

SafeReadResult safe_read_version_artifacts(int root_fd,
                                           std::string_view app,
                                           std::string_view version,
                                           std::size_t max_total_bytes) {
    if (!valid_app_version_id(app, 64) ||
        !valid_app_version_id(version, 128)) {
        return fail_result(SafeReadErrorCode::kInvalidPath,
                           "invalid app or version identifier");
    }
    const std::string prefix = std::string(app) + "/" + std::string(version) + "/";

    SafeReadResult result;
    std::size_t total = 0;
    SafeFile capsid_json;
    SafeFile bundle;
    SafeFile bytecode;
    SafeFile attestation;
    SafeFile signature;
    const auto collect = [&](const char* name, SafeFile* target,
                             std::size_t limit) -> bool {
        SafeReadResult one =
            safe_read_regular_file(root_fd, prefix + name, limit);
        if (one.code != SafeReadErrorCode::kNone) {
            result = std::move(one);
            return false;
        }
        total += one.file.bytes.size();
        if (total > max_total_bytes) {
            result = fail_result(SafeReadErrorCode::kOverLimit,
                                 "version artifacts exceed the total limit");
            return false;
        }
        *target = std::move(one.file);
        return true;
    };

    // Required files with per-file limits.
    if (!collect("capsid.json", &capsid_json, kMaxConfigFileBytes) ||
        !collect("bundle.mjs", &bundle, kMaxBundleFileBytes)) {
        return result;
    }

    // Bytecode triple: strictly 0 or 3. Each of the three files is probed
    // independently; a partial set (any one or two present) is an
    // all-or-none violation, not a source-only version.
    const char* const bytecode_names[] = {
        "bundle.qjsb", "bytecode.json", "bytecode.sig",
    };
    const std::size_t bytecode_limits[] = {
        kMaxBundleFileBytes, kMaxAttestationFileBytes,
        kBytecodeSignatureBytes,
    };
    SafeReadResult probes[3];
    bool present[3] = {};
    for (std::size_t index = 0; index < 3; ++index) {
        probes[index] =
            safe_read_regular_file(root_fd, prefix + bytecode_names[index],
                                   bytecode_limits[index]);
        present[index] =
            probes[index].code != SafeReadErrorCode::kMissingFile;
        if (present[index] && probes[index].code != SafeReadErrorCode::kNone) {
            return probes[index];
        }
    }
    const int present_count =
        (present[0] ? 1 : 0) + (present[1] ? 1 : 0) + (present[2] ? 1 : 0);
    if (present_count != 0 && present_count != 3) {
        // Partial set: any one or two of the three files is an
        // all-or-none violation, not a source-only version.
        return fail_result(SafeReadErrorCode::kMissingFile,
                           "bytecode artifacts must be all-or-none");
    }
    const bool bytecode_present = present_count == 3;
    if (bytecode_present) {
        bytecode = std::move(probes[0].file);
        attestation = std::move(probes[1].file);
        signature = std::move(probes[2].file);
        total += bytecode.bytes.size() + attestation.bytes.size() +
                 signature.bytes.size();
        if (total > max_total_bytes) {
            return fail_result(SafeReadErrorCode::kOverLimit,
                               "version artifacts exceed the total limit");
        }
    }

    SafeReadResult snapshot;
    snapshot.code = SafeReadErrorCode::kNone;
    snapshot.file = SafeFile{};
    VersionArtifacts artifacts;
    artifacts.capsid_json = std::move(capsid_json);
    artifacts.bundle = std::move(bundle);
    artifacts.has_bytecode = bytecode_present;
    artifacts.bytecode = std::move(bytecode);
    artifacts.attestation = std::move(attestation);
    artifacts.signature = std::move(signature);
    // The snapshot file carries the whole version's byte total so the
    // caller can bound staging without re-reading.
    snapshot.file.bytes.resize(0);
    snapshot.file.identity.size = static_cast<std::uint64_t>(total);
    snapshot.artifacts = std::move(artifacts);
    return snapshot;
}

}  // namespace capsid::host
