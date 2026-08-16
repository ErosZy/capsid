// M1D Unix Admin API. See admin_api.h.

#include "host/admin_api.h"

#include "host/metrics.h"
#include "host/structured_log.h"

#include <jansson.h>

#include "win32_compat.h"
#if defined(_WIN32)
#include <afunix.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <sstream>

namespace capsid::host {
namespace {

// Creates an AF_UNIX stream without allowing the descriptor to leak into a
// spawned worker. Linux can apply CLOEXEC atomically at socket creation;
// other POSIX platforms (including macOS) require a separate fcntl pass.
// Every failure path closes the descriptor before returning.
int create_cloexec_unix_socket() {
#if defined(_WIN32)
    // Winsock AF_UNIX (Windows 10 1803+) replaces the POSIX AF_UNIX
    // listener; the handle becomes a CRT fd. Winsock sockets are
    // non-inheritable by default, so the CLOEXEC pass is unnecessary.
    const SOCKET handle = socket(AF_UNIX, SOCK_STREAM, 0);
    if (handle == INVALID_SOCKET) {
        return -1;
    }
    return _open_osfhandle(
        static_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
#elif defined(__linux__) && defined(SOCK_CLOEXEC)
    return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
#endif
}

#if defined(_WIN32)
// Winsock takes the raw SOCKET handle; these wrappers translate CRT fds
// and winsock errno so the shared bind/connect/listen call sites below
// stay source-identical with POSIX.
int bind_unix_fd(int fd, const struct sockaddr *address,
                 socklen_t address_size) {
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int result = bind(socket_handle, address, address_size);
    if (result != 0) {
        capsid::win32::map_winsock_errno();
    }
    return result;
}

int listen_unix_fd(int fd, int backlog) {
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int result = listen(socket_handle, backlog);
    if (result != 0) {
        capsid::win32::map_winsock_errno();
    }
    return result;
}

int connect_unix_fd(int fd, const struct sockaddr *address,
                    socklen_t address_size) {
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int result = connect(socket_handle, address, address_size);
    if (result != 0) {
        capsid::win32::map_winsock_errno();
    }
    return result;
}
#endif

// Static redacted diagnostics only; never a path, errno text or backend
// material.
AdminResponse json_response(unsigned status, const std::string& body) {
    AdminResponse response;
    response.status = status;
    response.body = body;
    return response;
}

AdminResponse error_response(unsigned status, const char* message) {
    std::string body = "{\"error\":\"";
    body += message;
    body += "\"}";
    return json_response(status, body);
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out << buffer;
            } else {
                out << c;
            }
        }
    }
    return out.str();
}

bool ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// Frozen identifier grammars. App, Version and operation IDs are validated
// separately: an App ID must start lowercase alphanumeric and every
// character stays within [a-z0-9._-] (uppercase ASCII is rejected both at
// the first and every later position); a Version ID starts alphanumeric
// and stays within [A-Za-z0-9._-]; an operation ID starts alphanumeric
// and stays within [A-Za-z0-9_-] (no dot). A malformed ID on a known
// route is a 400 and never reaches the coordinator.
bool valid_app_id(const std::string& value) {
    if (value.empty() || value.size() > 63) {
        return false;
    }
    if (!((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) {
        return false;
    }
    for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_version_id(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    if (!ascii_alnum(value[0])) {
        return false;
    }
    for (const char c : value) {
        if (!ascii_alnum(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

bool valid_operation_id(const std::string& value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    if (!ascii_alnum(value[0])) {
        return false;
    }
    for (const char c : value) {
        if (!ascii_alnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

std::string operation_state_text(OperationState state) {
    switch (state) {
    case OperationState::kValidating: return "validating";
    case OperationState::kStaging: return "staging";
    case OperationState::kWarming: return "warming";
    case OperationState::kActivating: return "activating";
    case OperationState::kActive: return "active";
    case OperationState::kFailed: return "failed";
    }
    return "unknown";
}

// The deploy route is the only body-carrying endpoint. Strict parse:
// exactly {"app": "...", "version": "..."} with no duplicates, unknown
// fields, trailing content or wrong types. String values are constructed
// through json_string_length so an embedded  cannot be silently
// truncated by json_string_value; the NUL then fails the identifier
// grammar.
bool parse_deploy_body(const std::string& body, std::string* application,
                       std::string* version) {
    json_error_t parse_error;
    json_t* root = json_loadb(body.data(), body.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root) ||
        json_object_size(root) != 2) {
        if (root) {
            json_decref(root);
        }
        return false;
    }
    json_t* app_json = json_object_get(root, "app");
    json_t* version_json = json_object_get(root, "version");
    if (!json_is_string(app_json) || !json_is_string(version_json)) {
        json_decref(root);
        return false;
    }
    application->assign(json_string_value(app_json),
                        json_string_length(app_json));
    version->assign(json_string_value(version_json),
                    json_string_length(version_json));
    json_decref(root);
    return valid_app_id(*application) && valid_version_id(*version);
}

// Strict re-derivation of the backend's App-status document. Only the
// safe fields active/app/version/generation are allowed; duplicates,
// unknown fields, wrong types and malformed generation digests are
// rejected so a backend string is never reflected verbatim. The canonical
// document is rebuilt from the validated fields.
bool parse_app_status_document(const std::string& json,
                               std::string* canonical,
                               std::string* app_out) {
    json_error_t parse_error;
    json_t* root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root)) {
        if (root) {
            json_decref(root);
        }
        return false;
    }
    static const std::set<std::string> kAllowedFields = {
        "active", "app", "version", "generation",
    };
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(root, key, value) {
        if (kAllowedFields.find(key) == kAllowedFields.end()) {
            json_decref(root);
            return false;
        }
    }
    json_t* active_json = json_object_get(root, "active");
    json_t* app_json = json_object_get(root, "app");
    json_t* version_json = json_object_get(root, "version");
    json_t* generation_json = json_object_get(root, "generation");
    if (!json_is_boolean(active_json)) {
        json_decref(root);
        return false;
    }
    if (app_json != nullptr && !json_is_string(app_json)) {
        json_decref(root);
        return false;
    }
    if (version_json != nullptr && !json_is_string(version_json)) {
        json_decref(root);
        return false;
    }
    if (version_json != nullptr) {
        const std::string version_text(
            json_string_value(version_json),
            json_string_length(version_json));
        if (!valid_version_id(version_text)) {
            json_decref(root);
            return false;
        }
    }
    if (generation_json != nullptr) {
        if (!json_is_string(generation_json)) {
            json_decref(root);
            return false;
        }
        const char* digest = json_string_value(generation_json);
        const std::size_t length = json_string_length(generation_json);
        if (length != 71 || std::strncmp(digest, "sha256:", 7) != 0) {
            json_decref(root);
            return false;
        }
        for (std::size_t index = 7; index < length; ++index) {
            const char c = digest[index];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                json_decref(root);
                return false;
            }
        }
    }
    if (app_out != nullptr) {
        *app_out = app_json != nullptr
            ? std::string(json_string_value(app_json),
                          json_string_length(app_json))
            : std::string();
    }
    std::ostringstream out;
    out << "{\"active\":"
        << (json_is_true(active_json) ? "true" : "false");
    if (app_json != nullptr) {
        out << ",\"app\":\""
            << json_escape(std::string(json_string_value(app_json),
                                       json_string_length(app_json)))
            << '"';
    }
    if (version_json != nullptr) {
        out << ",\"version\":\""
            << json_escape(std::string(json_string_value(version_json),
                                       json_string_length(version_json)))
            << '"';
    }
    if (generation_json != nullptr) {
        out << ",\"generation\":\""
            << json_escape(std::string(json_string_value(generation_json),
                                       json_string_length(generation_json)))
            << '"';
    }
    out << '}';
    json_decref(root);
    *canonical = out.str();
    return true;
}

// Runs a coordinator call and converts any backend exception (std or
// unknown) into the static redacted failure response. Backend messages
// never cross the boundary.
template <typename F>
AdminResponse guarded_call(F&& call, const char* failure_text) {
    try {
        return call();
    } catch (const std::exception&) {
        return error_response(500, failure_text);
    } catch (...) {
        return error_response(500, failure_text);
    }
}

}  // namespace

bool query_admin_peer_credentials(int fd, AdminPeerCredentials* peer,
                                  std::string* error) {
    if (peer == nullptr) {
        if (error != nullptr) {
            *error = "invalid credential argument";
        }
        return false;
    }
#if defined(_WIN32)
    // Windows AF_UNIX carries no peer uid/gid. Access control is the
    // containing directory's NTFS ACL (see docs/windows.md), and the
    // authorization check below matches the reported identity 0/0.
    (void)fd;
    peer->uid = 0;
    peer->gid = 0;
    return true;
#elif defined(__APPLE__)
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0) {
        if (error != nullptr) {
            *error = "cannot query peer credentials";
        }
        return false;
    }
    peer->uid = static_cast<std::uint64_t>(uid);
    peer->gid = static_cast<std::uint64_t>(gid);
    return true;
#else
    struct ucred credential = {};
    socklen_t length = sizeof(credential);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &length) != 0) {
        if (error != nullptr) {
            *error = "cannot query peer credentials";
        }
        return false;
    }
    peer->uid = static_cast<std::uint64_t>(credential.uid);
    peer->gid = static_cast<std::uint64_t>(credential.gid);
    return true;
#endif
}

bool admin_peer_authorized(const AdminAuthorization& authorization,
                           const AdminPeerCredentials& peer) {
    if (peer.uid == authorization.allowed_uid) {
        return true;
    }
    return authorization.allowed_gid.has_value() &&
           peer.gid == *authorization.allowed_gid;
}

// Verifies the socket's parent directory before anything is created:
// Host-owned, a real directory (lstat, so a symlink is rejected as such),
// and without group/world write bits. A group/world-writable parent would
// let another principal replace the socket pathname after it is created.
bool verify_socket_parent(const std::string& path) {
    const std::string::size_type slash = path.rfind('/');
    const std::string parent =
        slash == std::string::npos
            ? std::string(".")
            : (slash == 0 ? std::string("/") : path.substr(0, slash));
#if defined(_WIN32)
    // Windows has no uid/mode on socket inodes: the parent just has to be
    // a real directory. NTFS ACLs on it are the access-control boundary
    // (see docs/windows.md).
    struct _stat64 st = {};
    if (_stat64(parent.c_str(), &st) != 0) {
        return false;
    }
    return (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st = {};
    if (lstat(parent.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 0022) == 0;
#endif
}

// Removes a crash-residue socket ONLY when all three pieces of evidence
// hold at the same time: the pathname is a Host-owned socket (lstat), a
// connect probe is refused (no live listener), and the device/inode are
// re-checked before the unlink (the pathname was not swapped in between).
// An active socket, a regular file or a symlink is left untouched.
bool remove_stale_socket(const std::string& path) {
#if defined(_WIN32)
    // Windows cannot lstat a socket inode (no S_ISSOCK/st_uid): the
    // connect-refused probe is the only piece of evidence available.
    const int probe = create_cloexec_unix_socket();
    if (probe < 0) {
        return false;
    }
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(),
                 sizeof(address.sun_path) - 1);
    const bool refused =
        connect_unix_fd(probe,
                        reinterpret_cast<const struct sockaddr*>(&address),
                        sizeof(address)) != 0 &&
        errno == ECONNREFUSED;
    close(probe);
    return refused && unlink(path.c_str()) == 0;
#else
    struct stat before = {};
    if (lstat(path.c_str(), &before) != 0 || !S_ISSOCK(before.st_mode) ||
        before.st_uid != geteuid()) {
        return false;
    }
    const int probe = create_cloexec_unix_socket();
    if (probe < 0) {
        return false;
    }
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(),
                 sizeof(address.sun_path) - 1);
    const bool refused =
        connect(probe, reinterpret_cast<const struct sockaddr*>(&address),
                sizeof(address)) != 0 &&
        errno == ECONNREFUSED;
    close(probe);
    if (!refused) {
        return false;
    }
    struct stat after = {};
    if (lstat(path.c_str(), &after) != 0 ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino) {
        return false;
    }
    return unlink(path.c_str()) == 0;
#endif
}

bool open_admin_listener(const AdminSocketOptions& options, int* listener,
                         std::string* error) {
    if (listener == nullptr || options.path.empty() ||
        options.path.size() >= sizeof(sockaddr_un::sun_path)) {
        if (error != nullptr) {
            *error = "invalid admin listener arguments";
        }
        return false;
    }
    // Fail closed on an insecure or foreign parent directory.
    if (!verify_socket_parent(options.path)) {
        if (error != nullptr) {
            *error = "admin listener parent is not secure";
        }
        return false;
    }
    // Linux socket inodes fix their mode at bind time as 0777 & ~umask;
    // pathname chmod/chown (fchownat/fchmodat with AT_SYMLINK_NOFOLLOW)
    // apply after that on Linux and macOS alike, so the dirfd pass below
    // is the authority for the exact mode and the management group, with
    // a final fstatat re-verification on every platform.
    const int fd = create_cloexec_unix_socket();
    if (fd < 0) {
        if (error != nullptr) {
            *error = "cannot create admin listener";
        }
        return false;
    }
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, options.path.c_str(),
                 sizeof(address.sun_path) - 1);
#if defined(_WIN32)
    // Windows socket inodes have no mode/uid/gid to manage: the parent
    // directory's NTFS ACL is the access-control boundary (see
    // docs/windows.md). Bind, with the same one-shot stale-socket retry
    // as POSIX.
    int bind_result = bind_unix_fd(
        fd, reinterpret_cast<const struct sockaddr*>(&address),
        sizeof(address));
    if (bind_result != 0 && errno == EADDRINUSE &&
        remove_stale_socket(options.path)) {
        bind_result = bind_unix_fd(
            fd, reinterpret_cast<const struct sockaddr*>(&address),
            sizeof(address));
    }
    if (bind_result != 0) {
        close(fd);
        if (error != nullptr) {
            *error = "cannot bind admin listener";
        }
        return false;
    }
    if (listen_unix_fd(fd, options.backlog) != 0) {
        (void) unlink(options.path.c_str());
        close(fd);
        if (error != nullptr) {
            *error = "cannot prepare admin listener";
        }
        return false;
    }
#else
    const std::string::size_type final_slash = options.path.rfind('/');
    const std::string parent_path =
        final_slash == std::string::npos
            ? std::string(".")
            : (final_slash == 0 ? std::string("/")
                                : options.path.substr(0, final_slash));
    const std::string socket_name =
        final_slash == std::string::npos
            ? options.path
            : options.path.substr(final_slash + 1);
    // The socket inode's mode is fixed at bind time as 0777 & ~umask, so
    // the creating umask is narrowed to the exact requested mode for the
    // bind window and restored immediately after (a Linux fchmod on a
    // socket fd is a silent no-op; the pathname fchmodat pass below is the
    // exact-mode authority). The group is applied through fchownat, which
    // honors in_group_p for supplementary groups (unlike setegid, which
    // needs CAP_SETGID).
    const mode_t created_mask = static_cast<mode_t>(~options.mode & 0777);
    const mode_t saved_umask = umask(created_mask);
    int bind_result = bind(fd, reinterpret_cast<const struct sockaddr*>(&address),
                           sizeof(address));
    umask(saved_umask);
    if (bind_result != 0 && errno == EADDRINUSE &&
        remove_stale_socket(options.path)) {
        // A crash left its socket behind; the stale pathname was removed
        // under the three-evidence rule, so bind may be retried once.
        const mode_t retry_mask = static_cast<mode_t>(~options.mode & 0777);
        const mode_t retry_umask = umask(retry_mask);
        bind_result = bind(fd,
                           reinterpret_cast<const struct sockaddr*>(&address),
                           sizeof(address));
        umask(retry_umask);
    }
    if (bind_result != 0) {
        close(fd);
        if (error != nullptr) {
            *error = "cannot bind admin listener";
        }
        return false;
    }
    // The exact mode and the optional management group are applied to the
    // PATHNAME inode: fchown/fchmod on a Linux socket fd are silent
    // no-ops, so the modifications go through fchownat/fchmodat from the
    // verified parent dirfd with AT_SYMLINK_NOFOLLOW, then the result is
    // re-verified with fstatat (still no symlink follow): socket type,
    // Host owner, configured management group and exact mode.
    const int parent_fd =
        openat(AT_FDCWD, parent_path.c_str(),
               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent_fd < 0) {
        close(fd);
        (void) unlink(options.path.c_str());
        if (error != nullptr) {
            *error = "cannot prepare admin listener";
        }
        return false;
    }
    const bool group_ok =
        !options.group_gid.has_value() ||
        fchownat(parent_fd, socket_name.c_str(), static_cast<uid_t>(-1),
                 *options.group_gid, AT_SYMLINK_NOFOLLOW) == 0;
    // The exact-mode pass. On virtiofs (Lima/Docker Desktop on macOS)
    // every chmod variant fails a socket inode with EINVAL and even the
    // bind-time umask is ignored (the inode is created 0755 by the
    // host-side daemon). The socket's connect gate is the write bit, so
    // when the mode is unenforceable the recheck below accepts a mode
    // that grants group/other no write access — the verified owner-only
    // parent directory keeps the socket reachable only by the Host owner,
    // and the Admin API additionally authenticates peers by credential.
    const bool mode_exact =
        fchmodat(parent_fd, socket_name.c_str(), options.mode,
                 AT_SYMLINK_NOFOLLOW) == 0;
    const bool mode_unenforceable =
        !mode_exact && errno == EINVAL;
    struct stat recheck = {};
    const bool recheck_ok =
        fstatat(parent_fd, socket_name.c_str(), &recheck,
                AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(recheck.st_mode) && recheck.st_uid == geteuid() &&
        (!options.group_gid.has_value() ||
         recheck.st_gid == *options.group_gid) &&
        (mode_exact
             ? (recheck.st_mode & 0777) == options.mode
             : (mode_unenforceable &&
                (recheck.st_mode & (S_IWGRP | S_IWOTH)) == 0));
    if (mode_unenforceable) {
        std::fprintf(stderr,
                     "admin: filesystem cannot chmod socket inodes; "
                     "relying on the owner-only parent directory "
                     "(effective mode %03o)\n",
                     recheck_ok ? (recheck.st_mode & 0777) : 0);
    }
    close(parent_fd);
    if (!group_ok || (!mode_exact && !mode_unenforceable) ||
        !recheck_ok || listen(fd, options.backlog) != 0) {
        // Remove the half-created socket ONLY when it is still the one
        // this call created (device/inode re-checked before the unlink).
        struct stat before = {};
        struct stat after = {};
        if (lstat(options.path.c_str(), &before) == 0 &&
            lstat(options.path.c_str(), &after) == 0 &&
            after.st_dev == before.st_dev &&
            after.st_ino == before.st_ino) {
            (void) unlink(options.path.c_str());
        }
        close(fd);
        if (error != nullptr) {
            *error = "cannot prepare admin listener";
        }
        return false;
    }
#endif
    *listener = fd;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

AdminResponse handle_admin_request(const AdminApiOptions& options,
                                   const AdminPeerCredentials& peer,
                                   const AdminRequest& request,
                                   AdminBackend* backend) {
    // 1. Global authorization from kernel credentials, before anything
    // else: an unauthorized peer never reaches the coordinator.
    if (!admin_peer_authorized(options.authorization, peer)) {
        // M2 item 7: §12.2 — an admin authorization failure is a
        // control-plane event, never dropped, no request content in the
        // line.
        if (options.log != nullptr) {
            options.log->log(LogLane::kControl,
                             {.event = log_events::kAdminAuth,
                              .result = "forbidden"});
        }
        return error_response(403, "forbidden");
    }
    // 2. Bounded request: header ceiling and body ceiling.
    if (request.header_bytes > options.max_header_bytes) {
        return error_response(431, "request header too large");
    }
    if (request.body.size() > options.max_body_bytes) {
        return error_response(413, "request body too large");
    }

    // 3. Exact routes. Route shape is matched first, then the identifier
    // grammar: a malformed ID on a known route is a 400 that never reaches
    // the coordinator; a known route with a mismatched method is a 405;
    // an unknown target is a 404. Every route except deploy rejects a
    // non-empty body.
    if (request.target == "/v1/deploy") {
        if (request.method != "POST") {
            return error_response(405, "method not allowed");
        }
        if (request.content_type != "application/json") {
            return error_response(415, "unsupported content type");
        }
        std::string application;
        std::string version;
        if (!parse_deploy_body(request.body, &application, &version)) {
            return error_response(400, "invalid request");
        }
        if (backend == nullptr) {
            return error_response(500, "deployment failed");
        }
        return guarded_call(
            [&]() -> AdminResponse {
                OperationStatus status;
                const DeployOutcome outcome =
                    backend->deploy(application, version, &status);
                if (!outcome.ok ||
                    !valid_operation_id(outcome.operation_id)) {
                    // Static redaction: coordinator internals (paths,
                    // secrets, bundle or environment content) and
                    // malformed backend output never enter the response.
                    return error_response(500, "deployment failed");
                }
                std::string body = "{\"operationId\":\"";
                body += json_escape(outcome.operation_id);
                body += "\",\"application\":\"";
                body += json_escape(application);
                body += "\",\"version\":\"";
                body += json_escape(version);
                body += "\",\"status\":\"";
                body += operation_state_text(status.state);
                body += "\"}";
                return json_response(202, body);
            },
            "deployment failed");
    }
    if (request.target.size() > std::string("/v1/apps/").size() &&
        request.target.compare(0, std::string("/v1/apps/").size(),
                               "/v1/apps/") == 0) {
        std::string rest =
            request.target.substr(std::string("/v1/apps/").size());
        bool retire = false;
        if (rest.size() > std::string("/retire").size() &&
            rest.compare(rest.size() - std::string("/retire").size(),
                         std::string("/retire").size(), "/retire") == 0) {
            retire = true;
            rest = rest.substr(0, rest.size() - std::string("/retire").size());
        }
        if (rest.find('/') != std::string::npos) {
            // A deeper path is not one of the known App routes.
            return error_response(404, "not found");
        }
        if (!valid_app_id(rest)) {
            return error_response(400, "invalid request");
        }
        const std::string application = rest;
        if (retire) {
            if (request.method != "POST") {
                return error_response(405, "method not allowed");
            }
            if (!request.body.empty()) {
                return error_response(400, "invalid request");
            }
            if (backend == nullptr) {
                return error_response(500, "operation failed");
            }
            return guarded_call(
                [&]() -> AdminResponse {
                    OperationStatus status;
                    const DeployOutcome outcome =
                        backend->retire(application, &status);
                    if (!outcome.ok ||
                        !valid_operation_id(outcome.operation_id)) {
                        return error_response(500, "operation failed");
                    }
                    std::string body = "{\"operationId\":\"";
                    body += json_escape(outcome.operation_id);
                    body += "\",\"status\":\"";
                    body += operation_state_text(status.state);
                    body += "\"}";
                    return json_response(202, body);
                },
                "operation failed");
        }
        if (request.method != "GET") {
            return error_response(405, "method not allowed");
        }
        if (!request.body.empty()) {
            return error_response(400, "invalid request");
        }
        if (backend == nullptr) {
            return error_response(500, "operation failed");
        }
        return guarded_call(
            [&]() -> AdminResponse {
                // The backend's App-status string is never reflected
                // verbatim: it is strictly parsed and the canonical safe
                // document is rebuilt from the validated fields.
                const std::string document = backend->app_status(application);
                std::string canonical;
                std::string document_app;
                if (!parse_app_status_document(document, &canonical,
                                               &document_app) ||
                    document_app != application) {
                    // An unknown field, malformed digest or a status
                    // document for another App is redacted.
                    return error_response(500, "operation failed");
                }
                return json_response(200, canonical);
            },
            "operation failed");
    }
    if (request.target.size() > std::string("/v1/operations/").size() &&
        request.target.compare(0, std::string("/v1/operations/").size(),
                               "/v1/operations/") == 0) {
        const std::string id =
            request.target.substr(std::string("/v1/operations/").size());
        if (id.find('/') != std::string::npos) {
            return error_response(404, "not found");
        }
        if (!valid_operation_id(id)) {
            return error_response(400, "invalid request");
        }
        if (request.method != "GET") {
            return error_response(405, "method not allowed");
        }
        if (!request.body.empty()) {
            return error_response(400, "invalid request");
        }
        if (backend == nullptr) {
            return error_response(500, "operation failed");
        }
        return guarded_call(
            [&]() -> AdminResponse {
                const OperationStatus status =
                    backend->operation_status(id);
                if (status.operation_id != id ||
                    (!status.version.empty() &&
                     !valid_version_id(status.version))) {
                    // The query result must echo the requested operation
                    // and carry a well-formed version; malformed backend
                    // output is redacted.
                    return error_response(500, "operation failed");
                }
                std::string body = "{\"operationId\":\"";
                body += json_escape(status.operation_id);
                body += "\",\"state\":\"";
                body += operation_state_text(status.state);
                body += "\"";
                if (!status.version.empty()) {
                    body += ",\"version\":\"";
                    body += json_escape(status.version);
                    body += "\"";
                }
                body += "}";
                return json_response(200, body);
            },
            "operation failed");
    }
    // 4. /metrics: the fixed registry (§12.1), rendered on demand. Only a
    // GET with no body; a missing registry is a static 500, never a
    // fabricated empty scrape.
    if (request.target == "/metrics") {
        if (request.method != "GET") {
            return error_response(405, "method not allowed");
        }
        if (!request.body.empty()) {
            return error_response(400, "invalid request");
        }
        if (options.metrics == nullptr) {
            return error_response(500, "metrics unavailable");
        }
        AdminResponse response;
        response.status = 200;
        response.content_type = "text/plain; version=0.0.4; charset=utf-8";
        response.body = options.metrics->render_prometheus_text();
        return response;
    }
    return error_response(404, "not found");
}

}  // namespace capsid::host
