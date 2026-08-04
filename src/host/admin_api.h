#ifndef CAPSID_HOST_ADMIN_API_H
#define CAPSID_HOST_ADMIN_API_H

#include "host/managed_host.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>

namespace capsid::host {

// M1D Unix Admin API (internal Host boundary).
//
// The listener is a Host-owned Unix stream socket with an exact mode
// (0600). Authorization is a single global decision taken from kernel
// peer credentials (SO_PEERCRED on Linux, getpeereid on macOS): the one
// configured UID, OR an optional management GID, may call every endpoint.
// Requests are bounded (header/body ceilings, Content-Type check for the
// body-carrying deploy route) and strictly parsed (duplicate fields,
// unknown fields, trailing content and wrong types are 400s). Backend
// failures never leak coordinator internals: responses carry static,
// redacted text only.

// Global Admin authorization: the allowed UID, plus an optional
// management GID that is an OR authority with the UID.
struct AdminAuthorization {
    std::uint64_t allowed_uid = 0;
    std::optional<std::uint64_t> allowed_gid;
};

struct AdminApiOptions {
    AdminAuthorization authorization;
    std::size_t max_header_bytes = 64U * 1024U;
    std::size_t max_body_bytes = 64U * 1024U;
};

// Kernel peer credentials captured from the connected socket.
struct AdminPeerCredentials {
    std::uint64_t uid = 0;
    std::uint64_t gid = 0;
};

// Reads the kernel peer credentials of a connected Unix stream socket:
// SO_PEERCRED on Linux, getpeereid on macOS. Returns false with a static
// error text on failure; never blocks.
bool query_admin_peer_credentials(int fd, AdminPeerCredentials* peer,
                                  std::string* error);

// The one global authorization decision: the peer UID equals the allowed
// UID, or the optional management GID matches the peer GID.
bool admin_peer_authorized(const AdminAuthorization& authorization,
                           const AdminPeerCredentials& peer);

struct AdminSocketOptions {
    std::string path;
    mode_t mode = 0600;
    int backlog = 4;
    // Optional fixed management group applied to the socket inode
    // (fchown after bind). When set, the socket is group-accessible under
    // the configured mode; the owner remains the Host euid.
    std::optional<gid_t> group_gid;
};

// Creates the Admin listener: Unix stream socket, bind, exact mode set on
// the socket, listen. Any failure closes the descriptor, removes a
// half-created socket file and returns a static redacted error (never the
// path or errno text). On success the caller owns the listener fd.
bool open_admin_listener(const AdminSocketOptions& options, int* listener,
                         std::string* error);

struct AdminRequest {
    std::string method;        // uppercase HTTP method
    std::string target;        // path only
    std::string content_type;  // lowercase
    std::string body;
    std::size_t header_bytes = 0;
};

struct AdminResponse {
    unsigned status = 500;
    std::string content_type = "application/json";
    std::string body;
};

// Coordinator adapter contract: the four operations the Admin API can
// trigger. Implementations own all coordinator state transitions.
class AdminBackend {
public:
    virtual ~AdminBackend() = default;
    virtual DeployOutcome deploy(const std::string& application,
                                 const std::string& version,
                                 OperationStatus* status) = 0;
    virtual DeployOutcome retire(const std::string& application,
                                 OperationStatus* status) = 0;
    virtual OperationStatus operation_status(
        const std::string& operation_id) = 0;
    virtual std::string app_status(const std::string& application) = 0;
};

// Pure request dispatch: authorization, bounds, exact route, strict
// parsing, coordinator call, redacted response. Never throws; every path
// returns a complete response.
AdminResponse handle_admin_request(const AdminApiOptions& options,
                                   const AdminPeerCredentials& peer,
                                   const AdminRequest& request,
                                   AdminBackend* backend);

}  // namespace capsid::host

#endif
