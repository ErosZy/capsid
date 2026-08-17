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
#else
#include <aclapi.h>
#include <sddl.h>
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

#if defined(_WIN32)
namespace {

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle) {}
    ~ScopedWinHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    ScopedWinHandle(const ScopedWinHandle &) = delete;
    ScopedWinHandle &operator=(const ScopedWinHandle &) = delete;
    ScopedWinHandle(ScopedWinHandle &&other) noexcept
        : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    ScopedWinHandle &operator=(ScopedWinHandle &&other) noexcept {
        if (this != &other) {
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

bool win_utf8_to_wide(const std::string &text,
                      std::wstring *out,
                      std::string *error) {
    if (text.empty()) {
        out->clear();
        return true;
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
        static_cast<int>(text.size()), NULL, 0);
    if (size <= 0) {
        *error = "cannot encode path as UTF-8";
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
            static_cast<int>(text.size()), out->data(), size) != size) {
        *error = "cannot encode path as UTF-8";
        return false;
    }
    return true;
}

bool win_wide_to_utf8(const std::wstring &text,
                      std::string *out,
                      std::string *error) {
    if (text.empty()) {
        out->clear();
        return true;
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), NULL, 0,
        NULL, NULL);
    if (size <= 0) {
        *error = "cannot decode Windows path";
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    if (WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            out->data(), size, NULL, NULL) != size) {
        *error = "cannot decode Windows path";
        return false;
    }
    return true;
}

std::wstring win_prefixed(const std::wstring &absolute) {
    if (absolute.rfind(L"\\\\?\\", 0) == 0) {
        return absolute;
    }
    if (absolute.size() >= 3 && absolute[1] == L':') {
        return L"\\\\?\\" + absolute;
    }
    if (absolute.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC" + absolute.substr(1);
    }
    return absolute;
}

bool win_absolute_path(const std::string &root,
                       std::wstring *out,
                       std::string *error) {
    std::wstring input;
    if (!win_utf8_to_wide(root, &input, error)) {
        return false;
    }
    const DWORD size =
        GetFullPathNameW(input.c_str(), 0, NULL, NULL);
    if (size == 0) {
        *error = root + " cannot be resolved to an absolute path";
        return false;
    }
    std::wstring absolute(size, L'\0');
    const DWORD written =
        GetFullPathNameW(input.c_str(), size, absolute.data(), NULL);
    if (written == 0 || written >= size) {
        *error = root + " cannot be resolved to an absolute path";
        return false;
    }
    absolute.resize(written);
    *out = win_prefixed(absolute);
    return true;
}

struct WinFileEntry {
    DWORD attributes = 0;
    DWORD links = 0;
    DWORD volume = 0;
    std::uint64_t index = 0;
    std::uint64_t size = 0;
    FILETIME creation_time = {};
    FILETIME write_time = {};

    bool same_identity(const WinFileEntry &other) const {
        return volume == other.volume && index == other.index &&
               links == other.links && size == other.size &&
               attributes == other.attributes &&
               CompareFileTime(&creation_time, &other.creation_time) == 0 &&
               CompareFileTime(&write_time, &other.write_time) == 0;
    }
};

bool win_read_entry(HANDLE handle, WinFileEntry *out) {
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(handle, &info)) {
        return false;
    }
    out->attributes = info.dwFileAttributes;
    out->links = info.nNumberOfLinks;
    out->volume = info.dwVolumeSerialNumber;
    out->index =
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
        info.nFileIndexLow;
    out->size =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
        info.nFileSizeLow;
    out->creation_time = info.ftCreationTime;
    out->write_time = info.ftLastWriteTime;
    return true;
}

bool win_current_user_sid(PSID *out, std::string *error) {
    HANDLE token = INVALID_HANDLE_VALUE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        *error = "cannot open the process token";
        return false;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        *error = "cannot read the process token user";
        return false;
    }
    std::vector<unsigned char> buffer(needed);
    if (!GetTokenInformation(
            token, TokenUser, buffer.data(), needed, &needed)) {
        CloseHandle(token);
        *error = "cannot read the process token user";
        return false;
    }
    CloseHandle(token);
    const TOKEN_USER *user =
        reinterpret_cast<const TOKEN_USER *>(buffer.data());
    const DWORD size = GetLengthSid(user->User.Sid);
    std::vector<unsigned char> sid(size);
    if (size == 0 || !CopySid(size, sid.data(), user->User.Sid)) {
        *error = "cannot copy the process token user";
        return false;
    }
    // The caller owns this copy and must LocalFree it.
    PSID copy = LocalAlloc(LPTR, size);
    if (copy == NULL || !CopySid(size, copy, user->User.Sid)) {
        if (copy != NULL) {
            LocalFree(copy);
        }
        *error = "cannot copy the process token user";
        return false;
    }
    *out = copy;
    return true;
}

// The Windows owner boundary trusts the process identity, and — only for
// privileged principals the process actually carries — Administrators or
// SYSTEM. Runners and services routinely create trees owned by
// Administrators while running under a member account. Arbitrary enabled
// groups are NOT trusted: Everyone, Users, Authenticated Users and similar
// broad principals appear in every ordinary token and must never satisfy
// the ownership check.
bool win_token_contains_sid(PSID candidate) {
    HANDLE token = INVALID_HANDLE_VALUE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenGroups, NULL, 0, &needed);
    bool contains = false;
    if (needed != 0) {
        std::vector<unsigned char> buffer(needed);
        if (GetTokenInformation(
                token, TokenGroups, buffer.data(), needed, &needed)) {
            const TOKEN_GROUPS *groups =
                reinterpret_cast<const TOKEN_GROUPS *>(buffer.data());
            for (DWORD index = 0; index < groups->GroupCount; ++index) {
                if (EqualSid(groups->Groups[index].Sid, candidate)) {
                    contains = true;
                    break;
                }
            }
        }
    }
    CloseHandle(token);
    return contains;
}

bool win_sid_is_privileged_owner(PSID candidate) {
    PSID administrators = NULL;
    PSID system = NULL;
    const BOOL administrators_ok = ConvertStringSidToSidW(
        L"S-1-5-32-544", &administrators);
    const BOOL system_ok = ConvertStringSidToSidW(
        L"S-1-5-18", &system);
    const bool privileged =
        (administrators_ok && EqualSid(candidate, administrators)) ||
        (system_ok && EqualSid(candidate, system));
    if (administrators != NULL) {
        LocalFree(administrators);
    }
    if (system != NULL) {
        LocalFree(system);
    }
    return privileged;
}

constexpr DWORD kWinPublicWritableMask =
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
    FILE_WRITE_ATTRIBUTES | WRITE_DAC | WRITE_OWNER | DELETE;

bool win_acl_grants_mask(PACL acl, PSID sid, DWORD mask) {
    TRUSTEE_W trustee = {};
    trustee.TrusteeForm = TRUSTEE_IS_SID;
    trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    DWORD access = 0;
    return GetEffectiveRightsFromAclW(acl, &trustee, &access) ==
               ERROR_SUCCESS &&
           (access & mask) != 0;
}

bool win_validate_security(HANDLE handle,
                           const std::string &path,
                           const std::vector<BindingOwnerId> &allowed_uids,
                           PSID current_user,
                           std::string *error) {
    if (allowed_uids.empty()) {
        *error = path + " has no allowed owner set";
        return false;
    }
    PSID owner = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    const DWORD result = GetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
        NULL, &dacl, NULL, &descriptor);
    if (result != ERROR_SUCCESS) {
        *error = path + " security descriptor is unreadable (code=" +
                 std::to_string(result) + ")";
        return false;
    }
    bool ok = false;
    if (owner == NULL) {
        *error = path + " has no owner";
    } else if (!EqualSid(owner, current_user) &&
               !(win_sid_is_privileged_owner(owner) &&
                 win_token_contains_sid(owner))) {
        *error = path + " has a disallowed owner";
    } else if (dacl == NULL) {
        *error = path + " has no DACL (everyone has full access)";
    } else {
        PSID everyone = NULL;
        PSID users = NULL;
        const BOOL everyone_ok = ConvertStringSidToSidW(
            L"S-1-1-0", &everyone);
        const BOOL users_ok = ConvertStringSidToSidW(
            L"S-1-5-32-545", &users);
        if (!everyone_ok || !users_ok) {
            *error = path + " cannot resolve public SIDs";
        } else if (win_acl_grants_mask(dacl, everyone,
                                       kWinPublicWritableMask) ||
                   win_acl_grants_mask(dacl, users,
                                       kWinPublicWritableMask)) {
            *error = path + " is writable by Everyone or Users";
        } else {
            ok = true;
        }
        if (everyone != NULL) {
            LocalFree(everyone);
        }
        if (users != NULL) {
            LocalFree(users);
        }
    }
    LocalFree(descriptor);
    return ok;
}

// Open without following reparse points and verify the open object still
// matches `expected`. Files must have exactly one link; directories and
// files must carry the expected type.
bool win_open_checked(const std::wstring &path,
                      bool require_directory,
                      bool check_identity,
                      const WinFileEntry &expected,
                      const std::string &display,
                      std::vector<BindingOwnerId> allowed_uids,
                      PSID current_user,
                      ScopedWinHandle *out,
                      WinFileEntry *entry_out,
                      std::string *error) {
    const DWORD access =
        READ_CONTROL |
        (require_directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES
                           : FILE_READ_DATA | FILE_READ_ATTRIBUTES);
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
    if (require_directory) {
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
    } else {
        flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    }
    ScopedWinHandle handle(CreateFileW(
        path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, flags, NULL));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        *error = display + " cannot be opened";
        return false;
    }
    WinFileEntry opened = {};
    if (!win_read_entry(handle.get(), &opened)) {
        *error = display + " cannot be inspected";
        return false;
    }
    if ((opened.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        *error = display + " is a reparse point (symlink/junction)";
        return false;
    }
    const bool is_directory =
        (opened.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (require_directory ? !is_directory : is_directory) {
        *error = require_directory ? display + " is not a directory"
                                   : display + " is not a regular file";
        return false;
    }
    if (!require_directory && opened.links != 1) {
        *error = display + " is a hard link";
        return false;
    }
    if (check_identity && !expected.same_identity(opened)) {
        *error = display + " changed between scan and open";
        return false;
    }
    if (!win_validate_security(
            handle.get(), display, allowed_uids, current_user, error)) {
        return false;
    }
    *out = std::move(handle);
    *entry_out = opened;
    return true;
}

bool win_enumerate(const std::wstring &directory,
                   std::vector<std::wstring> *names,
                   const std::string &display,
                   std::string *error) {
    const std::wstring search = directory + L"\\*";
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(search.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        *error = display + " cannot be enumerated";
        return false;
    }
    names->clear();
    do {
        if (std::wcscmp(data.cFileName, L".") == 0 ||
            std::wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        names->push_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    const DWORD saved = GetLastError();
    FindClose(find);
    if (saved != ERROR_NO_MORE_FILES) {
        *error = display + " changed or failed during enumeration";
        return false;
    }
    std::sort(names->begin(), names->end());
    return true;
}

bool win_read_file(const std::wstring &path,
                   const std::string &display,
                   std::size_t limit,
                   const WinFileEntry &expected,
                   std::vector<BindingOwnerId> allowed_uids,
                   PSID current_user,
                   std::string *out,
                   std::string *error) {
    ScopedWinHandle handle;
    WinFileEntry opened = {};
    if (!win_open_checked(path, false, true, expected, display, allowed_uids,
                          current_user, &handle, &opened, error)) {
        return false;
    }
    if (opened.size > limit) {
        *error = display + " exceeds the size limit";
        return false;
    }
    std::string content(static_cast<std::size_t>(opened.size), '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
        DWORD count = 0;
        if (!ReadFile(handle.get(), content.data() + offset,
                      static_cast<DWORD>(content.size() - offset), &count,
                      NULL) ||
            count == 0) {
            *error = display + " cannot be read";
            return false;
        }
        offset += count;
    }
    char extra = '\0';
    DWORD extra_count = 0;
    if (ReadFile(handle.get(), &extra, 1, &extra_count, NULL) &&
        extra_count != 0) {
        *error = display + " grew while being read";
        return false;
    }
    WinFileEntry after = {};
    if (!win_read_entry(handle.get(), &after) ||
        !opened.same_identity(after)) {
        *error = display + " changed while being read";
        return false;
    }
    // Reopen the final name: a replacement of the directory entry during the
    // read must not be accepted.
    ScopedWinHandle final_handle;
    WinFileEntry final_entry = {};
    if (!win_open_checked(path, false, true, opened, display, allowed_uids,
                          current_user, &final_handle, &final_entry, error)) {
        return false;
    }
    *out = std::move(content);
    return true;
}

bool scan_bindings_root_impl_win(
    const std::string &root,
    const std::vector<BindingOwnerId> &allowed_uids,
    const BindingRegistryScanHook &hook,
    BindingRegistrySnapshot *out,
    std::string *error) {
    if (out == nullptr || error == nullptr) {
        return false;
    }
    out->packages.clear();
    error->clear();
    if (allowed_uids.empty()) {
        *error = root + " has no allowed owner set";
        return false;
    }

    PSID current_user = NULL;
    if (!win_current_user_sid(&current_user, error)) {
        return false;
    }

    std::wstring root_wide;
    if (!win_absolute_path(root, &root_wide, error)) {
        LocalFree(current_user);
        return false;
    }

    // Root identity and security before enumeration.
    WinFileEntry root_identity = {};
    {
        ScopedWinHandle root_handle(CreateFileW(
            root_wide.c_str(),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            NULL));
        if (root_handle.get() == INVALID_HANDLE_VALUE) {
            *error = root + " cannot be opened";
            LocalFree(current_user);
            return false;
        }
        if (!win_read_entry(root_handle.get(), &root_identity)) {
            *error = root + " cannot be inspected";
            LocalFree(current_user);
            return false;
        }
        if ((root_identity.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            *error = root + " is a reparse point (symlink/junction)";
            LocalFree(current_user);
            return false;
        }
        if ((root_identity.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            *error = root + " is not a directory";
            LocalFree(current_user);
            return false;
        }
        if (!win_validate_security(root_handle.get(), root, allowed_uids,
                                   current_user, error)) {
            LocalFree(current_user);
            return false;
        }
    }

    std::vector<std::wstring> root_entries;
    if (!win_enumerate(root_wide, &root_entries, root, error)) {
        LocalFree(current_user);
        return false;
    }
    if (hook) {
        hook(BindingRegistryScanPhase::kRootEnumerated, {});
    }

    std::uint64_t total_source_bytes = 0;
    std::vector<BindingPackageSnapshot> packages;
    packages.reserve(root_entries.size());
    for (const std::wstring &package_name : root_entries) {
        std::string package_id;
        if (!win_wide_to_utf8(package_name, &package_id, error)) {
            LocalFree(current_user);
            return false;
        }
        const std::string package_path = root + "/" + package_id;
        if (!valid_binding_id(package_id)) {
            *error = package_path +
                     " is not a valid binding package directory name";
            LocalFree(current_user);
            return false;
        }

        const std::wstring package_wide = root_wide + L"\\" + package_name;
        ScopedWinHandle package_handle;
        WinFileEntry package_entry = {};
        if (!win_open_checked(package_wide, true, false, WinFileEntry{},
                              package_path, allowed_uids, current_user,
                              &package_handle, &package_entry, error)) {
            LocalFree(current_user);
            return false;
        }

        std::vector<std::wstring> files;
        if (!win_enumerate(package_wide, &files, package_path, error)) {
            LocalFree(current_user);
            return false;
        }
        if (hook) {
            hook(BindingRegistryScanPhase::kPackageEnumerated, package_id);
        }
        static const std::vector<std::wstring> kExpectedFiles = {
            L"index.js", L"manifest.json"};
        if (files != kExpectedFiles) {
            for (const std::wstring &file : files) {
                if (file != L"index.js" && file != L"manifest.json") {
                    std::string extra;
                    win_wide_to_utf8(file, &extra, error);
                    *error = package_path + "/" + extra +
                             " is an unexpected entry";
                    LocalFree(current_user);
                    return false;
                }
            }
            const bool has_manifest =
                std::find(files.begin(), files.end(), L"manifest.json") !=
                files.end();
            *error = has_manifest ? package_path + "/index.js is missing"
                                  : package_path + "/manifest.json is missing";
            LocalFree(current_user);
            return false;
        }

        // Capture the pre-read identities for both files.
        WinFileEntry manifest_entry = {};
        WinFileEntry source_entry = {};
        {
            ScopedWinHandle probe;
            WinFileEntry opened = {};
            if (!win_open_checked(package_wide + L"\\manifest.json", false,
                                  false, WinFileEntry{},
                                  package_path + "/manifest.json",
                                  allowed_uids, current_user, &probe,
                                  &opened, error) ||
                !win_open_checked(package_wide + L"\\index.js", false,
                                  false, WinFileEntry{},
                                  package_path + "/index.js", allowed_uids,
                                  current_user, &probe, &opened, error)) {
                LocalFree(current_user);
                return false;
            }
            // The second open clobbers probe; re-read identities directly.
            ScopedWinHandle manifest_handle(CreateFileW(
                (package_wide + L"\\manifest.json").c_str(),
                FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, NULL));
            if (manifest_handle.get() == INVALID_HANDLE_VALUE ||
                !win_read_entry(manifest_handle.get(), &manifest_entry)) {
                *error = package_path + "/manifest.json cannot be inspected";
                LocalFree(current_user);
                return false;
            }
            ScopedWinHandle source_handle(CreateFileW(
                (package_wide + L"\\index.js").c_str(),
                FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, NULL));
            if (source_handle.get() == INVALID_HANDLE_VALUE ||
                !win_read_entry(source_handle.get(), &source_entry)) {
                *error = package_path + "/index.js cannot be inspected";
                LocalFree(current_user);
                return false;
            }
        }

        BindingPackageSnapshot snapshot;
        snapshot.id = package_id;
        if (!win_read_file(package_wide + L"\\manifest.json",
                           package_path + "/manifest.json",
                           kMaxBindingManifestBytes, manifest_entry,
                           allowed_uids, current_user,
                           &snapshot.manifest_json, error) ||
            !win_read_file(package_wide + L"\\index.js",
                           package_path + "/index.js", kMaxBindingSourceBytes,
                           source_entry, allowed_uids, current_user,
                           &snapshot.source, error)) {
            LocalFree(current_user);
            return false;
        }
        if (source_entry.size > kMaxBindingGenerationSourceBytes -
                                   total_source_bytes) {
            *error = root + " exceeds the aggregate source size limit";
            LocalFree(current_user);
            return false;
        }
        total_source_bytes += source_entry.size;

        // Final package-level checks: entries, directory identity, and the
        // final names of both files.
        std::vector<std::wstring> files_after;
        if (!win_enumerate(package_wide, &files_after, package_path, error)) {
            LocalFree(current_user);
            return false;
        }
        if (files_after != files) {
            *error = package_path + " changed during scan";
            LocalFree(current_user);
            return false;
        }
        {
            ScopedWinHandle after_handle;
            WinFileEntry after_entry = {};
            if (!win_open_checked(package_wide, true, true, package_entry,
                                  package_path, allowed_uids, current_user,
                                  &after_handle, &after_entry, error)) {
                LocalFree(current_user);
                return false;
            }
        }

        const ConfigValidationResult validation =
            validate_binding_manifest(snapshot.manifest_json);
        if (!validation.ok) {
            *error = package_path +
                     "/manifest.json is not a valid binding manifest: " +
                     validation.error.message;
            LocalFree(current_user);
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

    std::vector<std::wstring> root_entries_after;
    if (!win_enumerate(root_wide, &root_entries_after, root, error)) {
        LocalFree(current_user);
        return false;
    }
    if (root_entries_after != root_entries) {
        *error = root + " changed during scan";
        LocalFree(current_user);
        return false;
    }
    {
        ScopedWinHandle root_after(CreateFileW(
            root_wide.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            NULL));
        if (root_after.get() == INVALID_HANDLE_VALUE) {
            *error = root + " changed during scan";
            LocalFree(current_user);
            return false;
        }
        WinFileEntry root_after_entry = {};
        if (!win_read_entry(root_after.get(), &root_after_entry) ||
            (root_after_entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) !=
                0 ||
            !root_identity.same_identity(root_after_entry)) {
            *error = root + " changed during scan";
            LocalFree(current_user);
            return false;
        }
    }

    std::sort(packages.begin(), packages.end(),
              [](const BindingPackageSnapshot &left,
                 const BindingPackageSnapshot &right) {
                  return left.id < right.id;
              });
    out->packages = std::move(packages);
    LocalFree(current_user);
    return true;
}

}  // namespace
#endif

bool scan_bindings_root(const std::string &root,
                        const std::vector<BindingOwnerId> &allowed_uids,
                        BindingRegistrySnapshot *out,
                        std::string *error) {
#if defined(_WIN32)
    const bool ok = scan_bindings_root_impl_win(
        root, allowed_uids, BindingRegistryScanHook(), out, error);
    // Every explicit path names the offending object. Keep the contract
    // airtight even if a future path forgets: an empty diagnostic must
    // never reach the operator or a regression test.
    if (!ok && error != nullptr && error->empty()) {
        *error = root + " rejected by the Windows registry scanner (code=" +
                 std::to_string(GetLastError()) + ")";
    }
    return ok;
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
    const bool ok = scan_bindings_root_impl_win(
        root, allowed_uids, hook, out, error);
    if (!ok && error != nullptr && error->empty()) {
        *error = root + " rejected by the Windows registry scanner (code=" +
                 std::to_string(GetLastError()) + ")";
    }
    return ok;
#else
    return scan_bindings_root_impl(root, allowed_uids, hook, out, error);
#endif
}

}  // namespace capsid::host
