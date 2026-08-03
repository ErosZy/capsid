#ifndef CAPSID_HOST_ARTIFACT_SAFE_READ_H
#define CAPSID_HOST_ARTIFACT_SAFE_READ_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {

// Deployment-input safe-read boundary (M1D). Deployment inputs are never
// read with std::ifstream or joined absolute paths; every artifact is read
// beneath a pre-opened root directory descriptor:
//
//   Linux:   openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS |
//                    RESOLVE_NO_MAGICLINKS); an ENOSYS kernel fails closed.
//   macOS:   component-by-component dirfd walk with openat + O_NOFOLLOW.
//
// Every read rejects absolute paths, empty components, ".", "..", NUL,
// symlinks, FIFOs, devices, sockets and directories; only regular files are
// accepted. The file identity (dev/inode/size/mtime/ctime) is re-checked
// after the read — an in-place replacement, truncation or any identity
// change fails the read. Failures return an empty snapshot and error
// messages that never contain file content.

inline constexpr std::size_t kMaxArtifactFileBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxVersionArtifactTotalBytes =
    64U * 1024U * 1024U;

struct FileIdentity {
    std::uint64_t dev = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_sec = 0;
    std::int64_t mtime_nsec = 0;
    std::int64_t ctime_sec = 0;
    std::int64_t ctime_nsec = 0;

    bool operator==(const FileIdentity& other) const {
        return dev == other.dev && inode == other.inode &&
               size == other.size && mtime_sec == other.mtime_sec &&
               mtime_nsec == other.mtime_nsec &&
               ctime_sec == other.ctime_sec && ctime_nsec == other.ctime_nsec;
    }
    bool operator!=(const FileIdentity& other) const {
        return !(*this == other);
    }
};

struct SafeFile {
    std::vector<std::uint8_t> bytes;
    FileIdentity identity;
};

// The frozen version layout snapshot. capsid.json and bundle are always
// present; the bytecode triple is either fully present (has_bytecode) or
// fully absent.
struct VersionArtifacts {
    SafeFile capsid_json;
    SafeFile bundle;
    bool has_bytecode = false;
    SafeFile bytecode;
    SafeFile attestation;
    SafeFile signature;
};

enum class SafeReadErrorCode {
    kNone,
    kInvalidPath,     // absolute, empty component, ".", "..", NUL
    kNotRegularFile,  // symlink, FIFO, device, socket, directory
    kOverLimit,       // file or version total exceeds the hard limit
    kIdentityChanged, // dev/inode/size/mtime/ctime changed during the read
    kMissingFile,     // required artifact absent (or bytecode triple partial)
    kIoError,
};

struct SafeReadResult {
    SafeReadErrorCode code = SafeReadErrorCode::kNone;
    // Static text only; never contains file content or paths from the
    // deployment tree.
    std::string message;
    SafeFile file;  // empty on failure
    // Populated by safe_read_version_artifacts; empty otherwise.
    VersionArtifacts artifacts;
};

// Reads one regular file beneath root_fd. relative_path is the deployment
// relative path (e.g. "orders/2026-07-31-002/bundle.mjs"); the caller's
// root_fd must be a pre-opened, trusted directory descriptor.
SafeReadResult safe_read_regular_file(int root_fd,
                                      std::string_view relative_path,
                                      std::size_t max_bytes);

// The frozen version layout (M1D):
//
//   applicationsRoot/<app>/<version>/
//     capsid.json          required
//     bundle.mjs           required
//     bundle.qjsb          optional group (all three or none)
//     bytecode.json        optional group
//     bytecode.sig         optional group
//
// All-or-none: a partial bytecode group is a missing-artifact failure.
SafeReadResult safe_read_version_artifacts(
    int root_fd,
    std::string_view app,
    std::string_view version,
    std::size_t max_total_bytes);

}  // namespace capsid::host

#endif
