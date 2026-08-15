// M2 E-3 slow-client write deadline RED/GREEN (design §9.2):
//   - a socket write that does not complete within write_timeout_ms (the
//     client stopped reading) cancels the request and closes the
//     connection; the server keeps serving afterwards;
//   - the write deadline is a Host-side socket view, independent from the
//     worker-side request timeout (§8.3 separate timers): a hung write is
//     torn down by the write deadline even when the worker deadline is
//     much longer;
//   - the worker deadline still fires independently when the client reads
//     normally but the worker is slow;
//   - a fast response is never touched by the write deadline.
//
// Frozen assertions: hung write → connection closed within a bounded time
// (well below the worker deadline); server healthy afterwards; worker
// deadline still maps to 504; fast responses unaffected.

#if __has_include("host/single_worker_server.h")
#include "host/single_worker_server.h"
#define CAPSID_HAS_SINGLE_WORKER_SERVER 1
#else
#define CAPSID_HAS_SINGLE_WORKER_SERVER 0
#endif

#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <arpa/inet.h>
#endif
#include "win32_compat.h"
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/socket.h>
#endif

// macOS does not define SOCK_CLOEXEC; these IPC pairs do not cross exec
// on the test paths, so a plain socket type is the portable fallback.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/time.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

#if CAPSID_HAS_SINGLE_WORKER_SERVER

std::string read_one_ready_line(int fd) {
    std::string line;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (line.empty() || line.back() != '\n') {
        require(std::chrono::steady_clock::now() < deadline,
                "server did not publish READY after start returned");
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = capsid::win32::capsid_poll(&descriptor, 1, 50);
        require(polled >= 0, "cannot poll server READY pipe");
        if (polled == 0) {
            continue;
        }
        char byte = 0;
#if defined(_WIN32)
        require(capsid::win32::read_fd(fd, &byte, 1) == 1,
#else
        require(read(fd, &byte, 1) == 1,
#endif
                "server READY pipe closed without a complete record");
        line.push_back(byte);
        require(line.size() <= 1024, "server READY record is unbounded");
    }
    return line;
}

std::uint16_t ready_port(const std::string& line) {
    const std::string marker = "\"port\":";
    const std::string::size_type begin = line.find(marker);
    require(begin != std::string::npos, "server READY record has no port");
    const char* digits = line.c_str() + begin + marker.size();
    char* end = nullptr;
    const unsigned long port = std::strtoul(digits, &end, 10);
    require(end != digits && port > 0 && port <= 65535,
            "server READY record has an invalid port");
    return static_cast<std::uint16_t>(port);
}

// A body much larger than any kernel socket buffer: the Host write blocks
// as soon as the client stops reading.
const std::vector<std::uint8_t>& big_body_bundle() {
    static const std::string source =
        "export default { fetch: () => { "
        "  return new Response(new TextEncoder().encode('x'.repeat(4 * 1024 * 1024)), "
        "    { headers: { 'content-type': 'application/octet-stream' } }); } };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

// The worker stalls for 500ms before answering: a worker-deadline fixture.
const std::vector<std::uint8_t>& slow_bundle() {
    static const std::string source =
        "export default { fetch: () => { "
        "  return new Promise(resolve => setTimeout(() => { "
        "    resolve(new Response('slow-ok')); }, 500)); } };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

// An immediate small response.
const std::vector<std::uint8_t>& fast_bundle() {
    static const std::string source =
        "export default { fetch: () => new Response('fast-ok') };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

int connect_to(std::uint16_t port) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create write-timeout HTTP socket");
    require(capsid::win32::setsockopt_recv_timeout_fd(fd, 3000) == 0,
            "cannot set write-timeout HTTP receive timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode write-timeout loopback address");
    require(capsid::win32::connect_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to write-timeout server");
    return fd;
}

void send_request(int fd, const std::string& target, bool keep_alive) {
    const std::string connection = keep_alive ? "keep-alive" : "close";
    const std::string request =
        "GET " + target + " HTTP/1.1\r\n"
        "Host: public.example\r\n"
        "Connection: " + connection + "\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            capsid::win32::send_fd(fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "cannot write HTTP request");
        sent += static_cast<std::size_t>(count);
    }
}

// The client never reads after sending the request; the server must close
// the connection (the write deadline cancels and tears down) within the
// deadline bound. Returns the elapsed time in ms. Data that reached the
// client before the teardown is drained in bulk (a byte-at-a-time drain
// would not finish within the bound).
long require_connection_closed_after_ignoring(int fd) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(3);
    char scratch[65536];
    for (;;) {
        require(std::chrono::steady_clock::now() < deadline,
                "server did not close the hung-write connection");
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = capsid::win32::capsid_poll(&descriptor, 1, 50);
        require(polled >= 0, "cannot poll hung-write connection");
        if (polled == 0) {
            continue;
        }
        // Drain whatever is pending; EOF or a read error ends the wait.
        // Consuming data here is fine — the teardown already happened or
        // is about to fire.
        for (;;) {
            const ssize_t count = capsid::win32::recv_fd(fd, scratch, sizeof(scratch), 0);
            if (count == 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                return static_cast<long>(elapsed.count());
            }
            if (count < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // drained for now; poll again
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                return static_cast<long>(elapsed.count());
            }
        }
    }
}

struct RawHttpResponse {
    unsigned status = 0;
    std::string body;
};

std::string read_line(int fd, std::string& buffer, char* scratch,
                      std::size_t scratch_size,
                      const std::chrono::steady_clock::time_point& deadline) {
    for (;;) {
        require(std::chrono::steady_clock::now() < deadline,
                "response line timed out");
        const std::string::size_type end = buffer.find("\r\n");
        if (end != std::string::npos) {
            const std::string line = buffer.substr(0, end);
            buffer.erase(0, end + 2);
            return line;
        }
        const ssize_t count = capsid::win32::recv_fd(fd, scratch, scratch_size, 0);
        require(count > 0, "response recv failed");
        buffer.append(scratch, static_cast<std::size_t>(count));
    }
}

void require_bytes(int fd, std::string& buffer, std::size_t count,
                   char* scratch, std::size_t scratch_size,
                   const std::chrono::steady_clock::time_point& deadline) {
    while (buffer.size() < count) {
        require(std::chrono::steady_clock::now() < deadline,
                "response body timed out");
        const ssize_t received = capsid::win32::recv_fd(fd, scratch, scratch_size, 0);
        require(received > 0, "response body recv failed");
        buffer.append(scratch, static_cast<std::size_t>(received));
    }
}

// One complete response (Content-Length or chunked body).
RawHttpResponse read_response(int fd) {
    char scratch[2048];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string buffer;
    const std::string status_line =
        read_line(fd, buffer, scratch, sizeof(scratch), deadline);
    const std::string::size_type sp1 = status_line.find(' ');
    require(sp1 != std::string::npos, "malformed status line");
    RawHttpResponse response;
    response.status = static_cast<unsigned>(
        std::strtoul(status_line.c_str() + sp1 + 1, nullptr, 10));
    require(response.status >= 100, "invalid response status");
    std::size_t body_length = 0;
    bool chunked = false;
    bool saw_content_length = false;
    std::string header_line;
    while (!(header_line = read_line(fd, buffer, scratch, sizeof(scratch),
                                     deadline)).empty()) {
        if (header_line.size() > 15 &&
            header_line.compare(0, 15, "Content-Length:") == 0) {
            body_length = static_cast<std::size_t>(
                std::strtoul(header_line.c_str() + 15, nullptr, 10));
            saw_content_length = true;
        } else if (header_line.size() > 18 &&
                   header_line.compare(0, 18, "Transfer-Encoding:") == 0 &&
                   header_line.find("chunked") != std::string::npos) {
            chunked = true;
        }
    }
    if (chunked) {
        for (;;) {
            const std::string size_line =
                read_line(fd, buffer, scratch, sizeof(scratch), deadline);
            const std::size_t chunk_size = static_cast<std::size_t>(
                std::strtoul(size_line.c_str(), nullptr, 16));
            if (chunk_size == 0) {
                for (;;) {
                    const std::string trailer =
                        read_line(fd, buffer, scratch, sizeof(scratch),
                                  deadline);
                    if (trailer.empty()) {
                        break;
                    }
                }
                break;
            }
            require_bytes(fd, buffer, chunk_size, scratch, sizeof(scratch),
                          deadline);
            response.body.append(buffer.substr(0, chunk_size));
            buffer.erase(0, chunk_size);
            const std::string terminator =
                read_line(fd, buffer, scratch, sizeof(scratch), deadline);
            require(terminator.empty(),
                    "chunked response had a non-empty chunk terminator");
        }
    } else {
        require(saw_content_length,
                "response has neither Content-Length nor chunked");
        require_bytes(fd, buffer, body_length, scratch, sizeof(scratch),
                      deadline);
        response.body = buffer.substr(0, body_length);
    }
    return response;
}

capsid::host::SingleWorkerServerOptions make_options(
    const char* worker_path, int ready_fd) {
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = worker_path;
    options.source_bundle_path = "write-timeout-inline";
    options.source_name = "file://write-timeout/v1/bundle.mjs";
    options.application = "orders";
    options.listen_address = "127.0.0.1";
    options.listen_port = 0;
    options.public_scheme = "http";
    options.public_authority = "public.example";
    options.request_timeout_ms = 5000;
    options.initial_stream_window = 64U * 1024U;
    options.strict_sandbox = false;
    options.ready_fd = ready_fd;
    return options;
}

// A client that stops reading must be torn down by the write deadline
// (bounded, far below the worker deadline), and the server must keep
// serving afterwards. With no write deadline implemented the connection
// would hang until the 5s worker timeout (or forever) and this test would
// fail its 3s bound.
void test_write_timeout_cancels(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create write-timeout READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.write_timeout_ms = 250;  // well below the worker deadline
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(big_body_bundle(), &error),
            "cannot start write-timeout server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int connection = connect_to(port);
    send_request(connection, "/@capsid/orders/big", false);
    const long elapsed = require_connection_closed_after_ignoring(connection);
    close(connection);
    // The write deadline (250ms) must fire, not the worker deadline (5s).
    require(elapsed < 2000,
            "hung write was not torn down by the write deadline: " +
                std::to_string(elapsed) + "ms");

    // The server and its worker are still healthy: a fast request succeeds.
    const int next = connect_to(port);
    send_request(next, "/@capsid/orders/check", true);
    const RawHttpResponse check = read_response(next);
    require(check.status == 200, "server unhealthy after write timeout: " +
                                     std::to_string(check.status));
    close(next);

    server.request_stop();
    require(server.wait(&error), "write-timeout server wait failed: " + error);
}

// The worker-side request timeout still fires independently when the
// client reads normally: 504 well below the write deadline (§8.3 separate
// timers — neither deadline replaces the other).
void test_worker_deadline_independent(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create write-timeout READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.write_timeout_ms = 5000;   // long: must NOT be the trigger
    options.request_timeout_ms = 200;  // the worker deadline is the trigger
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(slow_bundle(), &error),
            "cannot start write-timeout server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int connection = connect_to(port);
    send_request(connection, "/@capsid/orders/slow", false);
    const RawHttpResponse response = read_response(connection);
    require(response.status == 504,
            "worker deadline must map to 504, got " +
                std::to_string(response.status));
    close(connection);

    server.request_stop();
    require(server.wait(&error), "write-timeout server wait failed: " + error);
}

// A fast response is never touched by a short write deadline.
void test_fast_response_untouched(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create write-timeout READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.write_timeout_ms = 250;
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(fast_bundle(), &error),
            "cannot start write-timeout server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int connection = connect_to(port);
    send_request(connection, "/@capsid/orders/fast", false);
    const RawHttpResponse response = read_response(connection);
    require(response.status == 200 && response.body == "fast-ok",
            "fast response was disturbed by the write deadline");
    close(connection);

    server.request_stop();
    require(server.wait(&error), "write-timeout server wait failed: " + error);
}

#endif  // CAPSID_HAS_SINGLE_WORKER_SERVER

}  // namespace

int main(int argc, char** argv) {
    require(argc == 3, "expected mode and capsid-worker path");
#if !CAPSID_HAS_SINGLE_WORKER_SERVER
    (void)argv;
    fail("SingleWorkerServer is not implemented");
#else
    const std::string mode = argv[1];
    if (mode == "write-timeout-cancels") {
        test_write_timeout_cancels(argv[2]);
    } else if (mode == "worker-deadline-independent") {
        test_worker_deadline_independent(argv[2]);
    } else if (mode == "fast-response-untouched") {
        test_fast_response_untouched(argv[2]);
    } else {
        fail("unknown write-timeout mode");
    }
    std::cout << "PASS" << std::endl;
    return 0;
#endif
}
