// TrustedKeyStore implementation — see trusted_key_store.h. The §9.5 file
// contract is enforced here and nowhere else: every key file is opened with
// the frozen flags, verified to be a root/euid-owned regular 32-byte file
// with no group/other write bits, and its identity (dev/ino/size/mtime/ctime)
// is compared before and after the read so a mid-read swap fails closed.

#include "host/trusted_key_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace capsid::host {

namespace {

bool stat_identical(const struct stat& before, const struct stat& after) {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_size == after.st_size &&
           before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

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
        if (descriptor.key_path.empty() || descriptor.key_path[0] != '/') {
            set_key_error(error, "trusted key path must be absolute");
            return TrustedKeyStore();
        }
        // O_NOFOLLOW: a symlink anywhere in the final component is
        // rejected outright (the file is the key, not a pointer to it).
        // O_NONBLOCK: a FIFO or device cannot be the key.
        const int fd = open(descriptor.key_path.c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            set_key_error(error, "trusted key file cannot be opened");
            return TrustedKeyStore();
        }
        struct stat before = {};
        if (fstat(fd, &before) != 0) {
            close(fd);
            set_key_error(error, "trusted key file cannot be verified");
            return TrustedKeyStore();
        }
        if (!S_ISREG(before.st_mode)) {
            close(fd);
            set_key_error(error, "trusted key file is not a regular file");
            return TrustedKeyStore();
        }
        // Owner is root or the Host euid; group/other have no write bits.
        const uid_t euid = geteuid();
        if (!(before.st_uid == 0 || before.st_uid == euid) ||
            (before.st_mode & 0022) != 0) {
            close(fd);
            set_key_error(error,
                          "trusted key file owner or permissions rejected");
            return TrustedKeyStore();
        }
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
        struct stat after = {};
        if (fstat(fd, &after) != 0 || !stat_identical(before, after)) {
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
