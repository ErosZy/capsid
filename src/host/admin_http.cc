// M1D Unix Admin HTTP transport. See admin_http.h.
//
// The accepted connection is bound to its kernel peer credentials before a
// single HTTP byte is read; an unauthorized peer gets an immediate JSON
// 403. Authorized requests are framed by the Boost.Beast HTTP/1 authority
// (request_parser) fed from a nonblocking fd under poll deadlines, so
// header limits, body limits, ambiguous framing and slow headers are all
// rejected with the exact HTTP statuses. Responses are bounded JSON
// documents carrying Content-Type, Content-Length and Connection: close.

#include "host/admin_http.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

namespace capsid::host {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;

// Linux suppresses SIGPIPE per send call. macOS exposes the equivalent as a
// socket option which must be installed before any response (including the
// authorization rejection) is written.
bool prepare_no_sigpipe(int fd) {
#if defined(MSG_NOSIGNAL)
    (void) fd;
    return true;
#elif defined(SO_NOSIGPIPE)
    const int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                      sizeof(enabled)) == 0;
#else
    (void) fd;
    return false;
#endif
}

// Fixed-format bounded JSON response. Serialization is a simple, length
// controlled writer (the HTTP/1 framing authority remains Boost.Beast,
// which parsed the request); every response carries the exact headers the
// wire contract requires.
std::string http_response_text(unsigned status, const char* reason,
                               const std::string& body) {
    std::string out = "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason;
    out += "\r\nContent-Type: application/json\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\nConnection: close\r\n\r\n";
    out += body;
    return out;
}

std::string http_error_body(const char* message) {
    std::string body = "{\"error\":\"";
    body += message;
    body += "\"}";
    return body;
}

// Waits up to timeout_ms for the fd to become writable or readable.
// Returns 1 on ready, 0 on timeout, -1 on error (EINTR retried).
int wait_fd(int fd, short events, std::uint32_t timeout_ms) {
    struct pollfd descriptor = {};
    descriptor.fd = fd;
    descriptor.events = events;
    for (;;) {
        const int result = poll(&descriptor, 1, static_cast<int>(timeout_ms));
        if (result >= 0) {
            return result;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

// Bounded write of the whole response with the write deadline. Every byte
// is sent or the connection fails (static redacted error). The deadline is
// computed once for the whole response, so partial writes cannot stretch
// the budget.
bool write_all_bounded(int fd, const std::string& bytes,
                       std::uint32_t timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now);
        if (wait_fd(fd, POLLOUT,
                    static_cast<std::uint32_t>(
                        std::max<std::int64_t>(1, remaining.count()))) <= 0) {
            return false;
        }
#if defined(MSG_NOSIGNAL)
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const ssize_t count = send(fd, bytes.data() + offset,
                                   bytes.size() - offset, send_flags);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

// Reads until readable data arrives or the absolute deadline expires.
// Returns the number of bytes read (>0), 0 on EOF, -1 on timeout, -2 on
// error. The deadline is computed ONCE per phase by the caller: EINTR,
// short reads and a continuous slow drip of bytes all consume the same
// budget, so a Slowloris cannot reset the deadline byte by byte.
ssize_t read_until_deadline(
    int fd, char* buffer, std::size_t capacity,
    const std::chrono::steady_clock::time_point& deadline) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return -1;  // deadline
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now);
        const int ready = wait_fd(
            fd, POLLIN, static_cast<std::uint32_t>(
                            std::max<std::int64_t>(1, remaining.count())));
        if (ready == 0) {
            return -1;  // deadline
        }
        if (ready < 0) {
            return -2;  // poll error
        }
        const ssize_t count = read(fd, buffer, capacity);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return count;
    }
}

// Sends the static 403 for an unauthorized peer that may have sent
// nothing at all. The accepted fd stays caller-owned; the wrapper that
// accepted it is responsible for closing.
void reject_unauthorized(int fd, const AdminHttpOptions& options) {
    const std::string response = http_response_text(
        403, "Forbidden", http_error_body("forbidden"));
    (void) write_all_bounded(fd, response, options.write_timeout_ms);
}

}  // namespace

bool serve_accepted_admin_http_connection(
    int fd, const AdminHttpOptions& options, AdminBackend* backend,
    std::string* error) {
    // The accepted connection is served; the caller owns its lifecycle
    // around this call.
    if (!prepare_no_sigpipe(fd)) {
        if (error != nullptr) {
            *error = "cannot prepare admin connection";
        }
        return false;
    }
    // Bind the connection to its kernel peer credentials and decide the
    // global authorization BEFORE reading any attacker-controlled HTTP
    // byte.
    AdminPeerCredentials peer;
    std::string credential_error;
    if (!query_admin_peer_credentials(fd, &peer, &credential_error)) {
        if (error != nullptr) {
            *error = "cannot query admin peer credentials";
        }
        return false;
    }
    if (!admin_peer_authorized(options.api.authorization, peer)) {
        reject_unauthorized(fd, options);
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    // Nonblocking I/O under poll deadlines for the whole exchange.
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        if (error != nullptr) {
            *error = "cannot prepare admin connection";
        }
        return false;
    }
    // The Boost.Beast HTTP/1 authority frames the request; the parser
    // itself enforces the header and body ceilings.
    http::request_parser<http::string_body> parser;
    parser.header_limit(options.api.max_header_bytes);
    parser.body_limit(options.api.max_body_bytes);

    // One static redacted error response, then close the connection.
    const auto respond_and_close = [&](unsigned status, const char* reason,
                                       const char* message) {
        const std::string response = http_response_text(
            status, reason, http_error_body(message));
        (void) write_all_bounded(fd, response, options.write_timeout_ms);
        if (error != nullptr) {
            error->clear();
        }
        return true;
    };
    // Feeds bytes to the Beast parser and maps its errors: body ceiling is
    // a 413, every other real error (need_more is the normal "wait for
    // more input" signal) is a 400. Returns false when a response was
    // already written and the connection closed.
    const auto feed = [&](const char* data, std::size_t size) -> bool {
        beast::error_code parse_error;
        (void) parser.put(boost::asio::buffer(data, size), parse_error);
        if (parse_error == http::error::body_limit) {
            return respond_and_close(413, "Payload Too Large",
                                     "request body too large");
        }
        if (parse_error != http::error::need_more && parse_error) {
            return respond_and_close(400, "Bad Request", "invalid request");
        }
        return true;
    };

    std::string header_bytes;
    char buffer[4096];
    const auto header_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options.header_timeout_ms);
    for (;;) {
        const ssize_t count = read_until_deadline(
            fd, buffer, sizeof(buffer), header_deadline);
        if (count == -1) {
            // Header deadline: the client sent partial or no headers.
            return respond_and_close(408, "Request Timeout",
                                     "request timeout");
        }
        if (count == -2) {
            if (error != nullptr) {
                *error = "admin connection poll failed";
            }
            return false;
        }
        if (count == 0) {
            // EOF before the header completed.
            if (error != nullptr) {
                *error = "admin request ended before its header";
            }
            return false;
        }
        beast::error_code parse_error;
        const std::size_t used =
            parser.put(boost::asio::buffer(buffer,
                                           static_cast<std::size_t>(count)),
                       parse_error);
        header_bytes.append(buffer, used);
        if (parse_error == http::error::header_limit) {
            return respond_and_close(431, "Request Header Fields Too Large",
                                     "request header too large");
        }
        if (parse_error == http::error::body_limit) {
            return respond_and_close(413, "Payload Too Large",
                                     "request body too large");
        }
        if (parse_error != http::error::need_more && parse_error) {
            return respond_and_close(400, "Bad Request", "invalid request");
        }
        if (parser.is_header_done()) {
            // The parser stops exactly at the header boundary in this
            // call; body bytes that arrived with the header tail are fed
            // in a second pass before the body phase continues.
            if (used < static_cast<std::size_t>(count)) {
                if (!feed(buffer + used, static_cast<std::size_t>(count) - used)) {
                    return true;
                }
            }
            break;
        }
    }
    // Ambiguous framing (both Content-Length and Transfer-Encoding) and a
    // declared Content-Length beyond the body ceiling are rejected at the
    // header boundary, before any further body byte is consumed.
    const auto& header = parser.get().base();
    const bool has_content_length =
        header.find(http::field::content_length) != header.end();
    const bool has_transfer_encoding =
        header.find(http::field::transfer_encoding) != header.end();
    if (has_content_length && has_transfer_encoding) {
        return respond_and_close(400, "Bad Request", "invalid request");
    }
    if (parser.content_length().has_value() &&
        *parser.content_length() > options.api.max_body_bytes) {
        return respond_and_close(413, "Payload Too Large",
                                 "request body too large");
    }
    // Body phase: keep feeding the parser until the request is complete.
    const auto body_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options.body_timeout_ms);
    while (!parser.is_done()) {
        const ssize_t count = read_until_deadline(
            fd, buffer, sizeof(buffer), body_deadline);
        if (count == -1) {
            return respond_and_close(408, "Request Timeout",
                                     "request timeout");
        }
        if (count == -2) {
            if (error != nullptr) {
                *error = "admin connection poll failed";
            }
            return false;
        }
        if (count == 0) {
            // EOF before the declared body completed.
            if (error != nullptr) {
                *error = "admin request ended before its body";
            }
            return false;
        }
        if (!feed(buffer, static_cast<std::size_t>(count))) {
            return true;
        }
    }
    http::request<http::string_body> request = parser.release();
    AdminRequest admin_request;
    admin_request.method = std::string(request.method_string());
    admin_request.target = std::string(request.target());
    std::string content_type =
        std::string(request[http::field::content_type]);
    // Lowercase the content type for the dispatcher comparison.
    for (char& c : content_type) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    admin_request.content_type = std::move(content_type);
    admin_request.body = std::move(request.body());
    admin_request.header_bytes = header_bytes.size();
    const AdminResponse response = handle_admin_request(
        options.api, peer, admin_request, backend);
    const std::string response_text = http_response_text(
        response.status, "Admin", response.body);
    const bool written =
        write_all_bounded(fd, response_text, options.write_timeout_ms);
    if (!written) {
        if (error != nullptr) {
            *error = "cannot write admin response";
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool serve_one_admin_http_connection(int listener_fd,
                                     const AdminHttpOptions& options,
                                     AdminBackend* backend,
                                     std::string* error) {
    // One Unix connection; the listener stays owned by the caller.
    const int fd = accept(listener_fd, nullptr, nullptr);
    if (fd < 0) {
        if (error != nullptr) {
            *error = "cannot accept admin connection";
        }
        return false;
    }
    const bool served = serve_accepted_admin_http_connection(
        fd, options, backend, error);
    // The wrapper owns the accepted fd: close it exactly once, whether
    // the connection was served or rejected early.
    close(fd);
    if (error != nullptr && served) {
        error->clear();
    }
    return served;
}

}  // namespace capsid::host
