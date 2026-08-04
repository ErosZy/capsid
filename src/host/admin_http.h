#ifndef CAPSID_HOST_ADMIN_HTTP_H
#define CAPSID_HOST_ADMIN_HTTP_H

#include "host/admin_api.h"

#include <cstdint>
#include <string>

namespace capsid::host {

// Wire-level Admin transport options.
struct AdminHttpOptions {
    AdminApiOptions api;
    // Deadlines in milliseconds for the three connection phases. A
    // deadline is a bounded wait, not a total-operation bound.
    std::uint32_t header_timeout_ms = 5000;
    std::uint32_t body_timeout_ms = 5000;
    std::uint32_t write_timeout_ms = 5000;
};

// Accepts exactly one Unix-stream connection from listener_fd (which stays
// owned by the caller), binds the accepted connection to its kernel peer
// credentials BEFORE any HTTP byte is read, enforces the global
// authorization (an unauthorized peer receives an immediate JSON 403 even
// if it sent nothing), frames the request with the Boost.Beast HTTP/1
// authority under the configured header/body ceilings and deadlines,
// dispatches through handle_admin_request, writes one bounded JSON
// response with Content-Type, Content-Length and Connection: close, and
// closes the accepted connection. Transport failures return false with a
// static redacted error text; backend failures never leak internals.
bool serve_one_admin_http_connection(int listener_fd,
                                     const AdminHttpOptions& options,
                                     AdminBackend* backend,
                                     std::string* error);

}  // namespace capsid::host

#endif
