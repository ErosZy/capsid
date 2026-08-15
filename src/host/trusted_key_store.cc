// TrustedKeyStore implementation — see trusted_key_store.h. The §9.5 file
// contract is enforced here and nowhere else: every key file is opened with
// the frozen flags, verified to be a root/euid-owned regular 32-byte file
// with no group/other write bits, and its identity (dev/ino/size/mtime/ctime)
// is compared before and after the read so a mid-read swap fails closed.

#include "host/trusted_key_store.h"

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>

namespace capsid::host {

namespace {

// Cross-platform stat timestamp accessors: Apple spells the fields
// st_mtimespec/st_ctimespec; other POSIX systems use st_mtim/st_ctim;
// Windows carries whole seconds only (nsec = 0), and the identity check
// still catches replacement via dev/ino/size/mtime.
#if defined(__APPLE__)
#define CAPSID_KT_MTIME_SEC(st) ((st).st_mtimespec.tv_sec)
#define CAPSID_KT_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define CAPSID_KT_CTIME_SEC(st) ((st).st_ctimespec.tv_sec)
#define CAPSID_KT_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#elif defined(_WIN32)
#define CAPSID_KT_MTIME_SEC(st) ((st).st_mtime)
#define CAPSID_KT_MTIME_NSEC(st) 0
#define CAPSID_KT_CTIME_SEC(st) ((st).st_ctime)
#define CAPSID_KT_CTIME_NSEC(st) 0
#else
#define CAPSID_KT_MTIME_SEC(st) ((st).st_mtim.tv_sec)
#define CAPSID_KT_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define CAPSID_KT_CTIME_SEC(st) ((st).st_ctim.tv_sec)
#define CAPSID_KT_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

template <typename Stat>
bool stat_identical(const Stat& before, const Stat& after) {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_size == after.st_size &&
           CAPSID_KT_MTIME_SEC(before) == CAPSID_KT_MTIME_SEC(after) &&
           CAPSID_KT_MTIME_NSEC(before) == CAPSID_KT_MTIME_NSEC(after) &&
           CAPSID_KT_CTIME_SEC(before) == CAPSID_KT_CTIME_SEC(after) &&
           CAPSID_KT_CTIME_NSEC(before) == CAPSID_KT_CTIME_NSEC(after);
}

#if defined(_WIN32)
// Opens the trusted key with reparse-point rejection: a symlinked key
// file fails the open instead of being followed (the O_NOFOLLOW
// equivalent; intermediate directory traversal follows Windows default
// resolution, see docs/windows.md).
int open_trusted_key(const std::string& path, std::string* error) {
    const int wide_size = MultiByteToWideChar(
        CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide_size <= 0) {
        set_key_error(error, "trusted key path encoding rejected");
        return -1;
    }
    std::wstring wide(static_cast<std::size_t>(wide_size - 1), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, 0, path.c_str(), -1, &wide[0], wide_size) <= 0) {
        set_key_error(error, "trusted key path encoding rejected");
        return -1;
    }
    const HANDLE handle = CreateFileW(
        wide.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes = {};
    if (GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes,
            sizeof(attributes)) != 0 &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        set_key_error(error, "trusted key file cannot be opened");
        return -1;
    }
    const int fd = _open_osfhandle(
        reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
    }
    return fd;
}
#endif

// Stable operator-facing failure: never includes the path, the key id or
// any key bytes.
void set_key_error(std::string* error, const char* stable_message) {
    if (error != nullptr) {
        *error = stable_message;
    }
}

bool read_exactly(int fd, std::array<std::uint8_t, kEd25519PublicKeyBytes>* out,
                  std::string* error) {
    std::size_t filled = 0;
    while (filled < out->size()) {
        const ssize_t count =
            read(fd, out->data() + filled, out->size() - filled);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_key_error(error, "trusted key file unreadable");
            return false;
        }
        if (count == 0) {
            set_key_error(error, "trusted key file is shorter than 32 bytes");
            return false;
        }
        filled += static_cast<std::size_t>(count);
    }
    // The file is exactly 32 bytes: any further byte means it is not the
    // trusted artifact. (The pre/post stat comparison already catches a
    // size change; this guards a file that reports an identical stat while
    // holding more data, which cannot happen for a regular file, but the
    // contract is checked here regardless.)
    std::uint8_t extra = 0;
    const ssize_t probe = read(fd, &extra, 1);
    if (probe > 0) {
        set_key_error(error, "trusted key file is longer than 32 bytes");
        return false;
    }
    return true;
}

}  // namespace

TrustedKeyStore TrustedKeyStore::load(
    std::span<const TrustedKeyDescriptor> descriptors, std::string* error) {
    TrustedKeyStore store;
    if (descriptors.size() > kMaxTrustedKeys) {
        set_key_error(error, "too many trusted bytecode keys");
        return store;
    }
    store.ids_.reserve(descriptors.size());
    store.raw_keys_.reserve(descriptors.size());
    store.keys_.reserve(descriptors.size());

    for (const TrustedKeyDescriptor& descriptor : descriptors) {
        if (descriptor.key_id.empty() ||
            descriptor.key_id.size() > kMaxTrustedKeyIdBytes) {
            set_key_error(error, "trusted key id is empty or too long");
            return TrustedKeyStore();
        }
        // v1: no rotation — a duplicate id cannot mean "the second load".
        for (const std::string& existing : store.ids_) {
            if (existing == descriptor.key_id) {
                set_key_error(error, "duplicate trusted key id");
                return TrustedKeyStore();
            }
        }
        // Absolute path only: a relative path resolves differently under a
        // different cwd, which is an unverifiable configuration.
        bool absolute_path = false;
#if defined(_WIN32)
        // Windows absolute form: <drive>:\ or <drive>:/.
        absolute_path =
            descriptor.key_path.size() >= 3 &&
            ((descriptor.key_path[0] >= 'A' &&
              descriptor.key_path[0] <= 'Z') ||
             (descriptor.key_path[0] >= 'a' &&
              descriptor.key_path[0] <= 'z')) &&
            descriptor.key_path[1] == ':' &&
            (descriptor.key_path[2] == '\\' ||
             descriptor.key_path[2] == '/');
#else
        absolute_path =
            !descriptor.key_path.empty() && descriptor.key_path[0] == '/';
#endif
        if (!absolute_path) {
            set_key_error(error, "trusted key path must be absolute");
            return TrustedKeyStore();
        }
        // O_NOFOLLOW: a symlink anywhere in the final component is
        // rejected outright (the file is the key, not a pointer to it).
        // O_NONBLOCK: a FIFO or device cannot be the key. On Windows the
        // reparse-point rejection below is the O_NOFOLLOW equivalent.
#if defined(_WIN32)
        const int fd = open_trusted_key(descriptor.key_path, error);
#else
        const int fd = open(descriptor.key_path.c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
#endif
        if (fd < 0) {
            set_key_error(error, "trusted key file cannot be opened");
            return TrustedKeyStore();
        }
#if defined(_WIN32)
        struct _stat64 before = {};
        const bool stat_ok = _fstat64(fd, &before) == 0;
        const bool regular = (before.st_mode & _S_IFREG) != 0;
#else
        struct stat before = {};
        const bool stat_ok = fstat(fd, &before) == 0;
        const bool regular = S_ISREG(before.st_mode);
#endif
        if (!stat_ok) {
            close(fd);
            set_key_error(error, "trusted key file cannot be verified");
            return TrustedKeyStore();
        }
        if (!regular) {
            close(fd);
            set_key_error(error, "trusted key file is not a regular file");
            return TrustedKeyStore();
        }
#if !defined(_WIN32)
        // Owner is root or the Host euid; group/other have no write bits.
        // (Windows has no uid/mode on files; NTFS ACLs are the boundary,
        // see docs/windows.md.)
        const uid_t euid = geteuid();
        if (!(before.st_uid == 0 || before.st_uid == euid) ||
            (before.st_mode & 0022) != 0) {
            close(fd);
            set_key_error(error,
                          "trusted key file owner or permissions rejected");
            return TrustedKeyStore();
        }
#endif
        if (before.st_size != static_cast<off_t>(kEd25519PublicKeyBytes)) {
            close(fd);
            set_key_error(error, "trusted key file is not 32 bytes");
            return TrustedKeyStore();
        }
        std::array<std::uint8_t, kEd25519PublicKeyBytes> raw_key{};
        if (!read_exactly(fd, &raw_key, error)) {
            close(fd);
            return TrustedKeyStore();
        }
#if defined(_WIN32)
        struct _stat64 after = {};
        if (_fstat64(fd, &after) != 0 || !stat_identical(before, after)) {
#else
        struct stat after = {};
        if (fstat(fd, &after) != 0 || !stat_identical(before, after)) {
#endif
            close(fd);
            set_key_error(error,
                          "trusted key file changed during verification");
            return TrustedKeyStore();
        }
        close(fd);

        // Own the memory BEFORE creating the views: the views point into
        // ids_ / raw_keys_, and vector reallocation must not invalidate
        // them. Both are reserved to the final size up front, so the
        // push_backs below cannot reallocate.
        store.ids_.push_back(descriptor.key_id);
        store.raw_keys_.push_back(raw_key);
        store.keys_.push_back(TrustedBytecodeKey{
            std::string_view(store.ids_.back()),
            std::span<const std::uint8_t>(store.raw_keys_.back())});
    }
    return store;
}

}  // namespace capsid::host
