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

// Serves one already-accepted Unix-stream connection. The fd stays
// CALLER-OWNED throughout: this function never closes it, so a long-lived
// service can keep the descriptor under its own lifecycle lock and the
// accept wrapper (serve_one) can close it exactly once. The connection is
// bound to its kernel peer credentials BEFORE any HTTP byte is read, the
// global authorization is enforced (an unauthorized peer receives an
// immediate JSON 403 even if it sent nothing), the request is framed by
// the Boost.Beast HTTP/1 authority under the configured ceilings and
// deadlines, dispatch goes through handle_admin_request, and one bounded
// JSON response with Content-Type, Content-Length and Connection: close
// is written. A shutdown(fd, SHUT_RDWR) from another thread (service
// stop) interrupts the read loops promptly instead of waiting out the
// HTTP deadlines. Transport failures return false with a static redacted
// error text; backend failures never leak internals.
bool serve_accepted_admin_http_connection(int fd,
                                          const AdminHttpOptions& options,
                                          AdminBackend* backend,
                                          std::string* error);

// Accepts exactly one Unix-stream connection from listener_fd (which stays
// owned by the caller) and serves it through
// serve_accepted_admin_http_connection.
bool serve_one_admin_http_connection(int listener_fd,
                                     const AdminHttpOptions& options,
                                     AdminBackend* backend,
                                     std::string* error);

}  // namespace capsid::host

#endif
