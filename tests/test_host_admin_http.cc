// Frozen M1D Unix Admin HTTP integration RED suite.
//
// Unlike test_host_admin_api.cc's pure dispatcher tests, this suite sends
// real HTTP/1 bytes over a connected AF_UNIX stream. The server boundary must
// accept one connection, bind authorization to kernel peer credentials before
// reading attacker-controlled bytes, use the Host's Boost.Beast framing
// authority, enforce wire-level limits/timeouts, serialize one bounded JSON
// response, close the accepted connection, and leave the listener owned by
// the caller.

#if __has_include("host/admin_http.h")
#include "host/admin_http.h"
#define CAPSID_HAS_ADMIN_HTTP 1
#else
#define CAPSID_HAS_ADMIN_HTTP 0
#endif

#include "host/admin_api.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
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

volatile sig_atomic_t sigpipe_count = 0;

void record_sigpipe(int) {
    sigpipe_count = 1;
}

#if CAPSID_HAS_ADMIN_HTTP

class FakeAdminBackend final : public capsid::host::AdminBackend {
public:
    capsid::host::DeployOutcome deploy(
        const std::string& application,
        const std::string& version,
        capsid::host::OperationStatus* status) override {
        ++deploy_calls;
        last_application = application;
        last_version = version;
        capsid::host::DeployOutcome outcome;
        outcome.ok = true;
        outcome.operation_id = "op-http-1";
        status->operation_id = outcome.operation_id;
        status->version = version;
        status->state = capsid::host::OperationState::kWarming;
        return outcome;
    }

    capsid::host::DeployOutcome retire(
        const std::string& application,
        capsid::host::OperationStatus* status) override {
        (void) application;
        (void) status;
        ++other_calls;
        return {};
    }

    capsid::host::OperationStatus operation_status(
        const std::string& operation_id) override {
        (void) operation_id;
        ++other_calls;
        return {};
    }

    std::string app_status(const std::string& application) override {
        (void) application;
        ++other_calls;
        return "{\"active\":false}";
    }

    int total_calls() const { return deploy_calls + other_calls; }

    int deploy_calls = 0;
    int other_calls = 0;
    std::string last_application;
    std::string last_version;
};

struct ListenerFixture {
    std::string directory;
    std::string path;
    int fd = -1;

    ListenerFixture() {
        directory = "/tmp/capsid-admin-http-XXXXXX";
        require(mkdtemp(directory.data()) != nullptr,
                "cannot create Admin HTTP fixture directory");
        path = directory + "/admin.sock";
        capsid::host::AdminSocketOptions socket_options;
        socket_options.path = path;
        socket_options.mode = 0600;
        socket_options.backlog = 4;
        std::string error;
        require(capsid::host::open_admin_listener(socket_options, &fd, &error),
                "cannot open Admin HTTP fixture listener: " + error);
    }

    ~ListenerFixture() {
        if (fd >= 0) {
            close(fd);
        }
        if (!path.empty()) {
            (void) unlink(path.c_str());
        }
        if (!directory.empty()) {
            (void) rmdir(directory.c_str());
        }
    }
};

capsid::host::AdminHttpOptions http_options() {
    capsid::host::AdminHttpOptions options;
    options.api.authorization.allowed_uid =
        static_cast<std::uint64_t>(geteuid());
    options.api.max_header_bytes = 1024;
    options.api.max_body_bytes = 1024;
    options.header_timeout_ms = 1000;
    options.body_timeout_ms = 1000;
    options.write_timeout_ms = 1000;
    return options;
}

int connect_client(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    require(fd >= 0, "cannot create Admin HTTP client socket");
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    require(path.size() < sizeof(address.sun_path),
            "Admin HTTP fixture path is too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    require(connect(fd, reinterpret_cast<const struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect Admin HTTP client");
    const struct timeval timeout = {2, 0};
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot bound Admin HTTP client receive");
    return fd;
}

void send_all(int fd, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = send(fd, bytes.data() + offset,
                                   bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0, "cannot send Admin HTTP request");
        offset += static_cast<std::size_t>(count);
    }
}

std::string receive_to_eof(int fd) {
    std::string bytes;
    char buffer[4096];
    for (;;) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            bytes.append(buffer, static_cast<std::size_t>(count));
            require(bytes.size() <= 64U * 1024U,
                    "Admin HTTP response exceeded the test ceiling");
            continue;
        }
        if (count == 0) {
            return bytes;
        }
        if (errno == EINTR) {
            continue;
        }
        fail("Admin HTTP server did not close its response promptly");
    }
}

unsigned response_status(const std::string& response) {
    const std::string prefix = "HTTP/1.1 ";
    require(response.compare(0, prefix.size(), prefix) == 0,
            "Admin HTTP response is not HTTP/1.1");
    require(response.size() >= prefix.size() + 3,
            "Admin HTTP response has no status code");
    return static_cast<unsigned>((response[prefix.size()] - '0') * 100 +
        (response[prefix.size() + 1] - '0') * 10 +
        (response[prefix.size() + 2] - '0'));
}

std::string response_body(const std::string& response) {
    const std::size_t delimiter = response.find("\r\n\r\n");
    require(delimiter != std::string::npos,
            "Admin HTTP response omitted its header terminator");
    return response.substr(delimiter + 4);
}

std::string exchange(const capsid::host::AdminHttpOptions& options,
                     FakeAdminBackend* backend,
                     const std::string& request_bytes,
                     bool shutdown_request = true) {
    ListenerFixture listener;
    bool served = false;
    std::string server_error;
    std::thread server([&]() {
        served = capsid::host::serve_one_admin_http_connection(
            listener.fd, options, backend, &server_error);
    });
    const int client = connect_client(listener.path);
    if (!request_bytes.empty()) {
        send_all(client, request_bytes);
    }
    if (shutdown_request) {
        require(shutdown(client, SHUT_WR) == 0,
                "cannot finish Admin HTTP request stream");
    }
    const std::string response = receive_to_eof(client);
    close(client);
    server.join();
    require(served, "Admin HTTP connection failed: " + server_error);
    return response;
}

std::string drip_exchange(const capsid::host::AdminHttpOptions& options,
                          FakeAdminBackend* backend,
                          const std::string& prefix,
                          const std::string& drip_bytes,
                          std::chrono::steady_clock::duration* elapsed) {
    ListenerFixture listener;
    bool served = false;
    std::string server_error;
    std::thread server([&]() {
        served = capsid::host::serve_one_admin_http_connection(
            listener.fd, options, backend, &server_error);
    });
    const int client = connect_client(listener.path);
    if (!prefix.empty()) {
        send_all(client, prefix);
    }
    const auto started = std::chrono::steady_clock::now();
    for (const char byte : drip_bytes) {
#if defined(MSG_NOSIGNAL)
        const ssize_t count = send(client, &byte, 1, MSG_NOSIGNAL);
#else
        const ssize_t count = send(client, &byte, 1, 0);
#endif
        if (count != 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    const std::string response = receive_to_eof(client);
    *elapsed = std::chrono::steady_clock::now() - started;
    close(client);
    server.join();
    require(served, "Admin HTTP drip connection failed: " + server_error);
    return response;
}

// Wait for each fragment to leave the kernel receive queue before sending
// the next. Unlike sleeps between writes, this forces separate transport
// reads even on a busy runner. The accepted descriptor stays open until the
// client finishes sending, including when the parser rejects a prefix.
std::string fragmented_exchange(
    const capsid::host::AdminHttpOptions& options,
    FakeAdminBackend* backend, const std::vector<std::string>& fragments,
    bool allow_early_response = false) {
    int sockets[2] = {-1, -1};
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "cannot create fragmented Admin HTTP fixture");
    const struct timeval timeout = {2, 0};
    require(setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot bound fragmented Admin HTTP receive");
    std::atomic<bool> finished{false}, sent{false};
    bool served = false;
    std::string server_error;
    std::thread server([&]() {
        served = capsid::host::serve_accepted_admin_http_connection(
            sockets[0], options, backend, &server_error);
        finished.store(true);
        while (!sent.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // A rejection can race the next client write after FIONREAD became
        // zero. The fixture owns this fd: discard those in-flight bytes only
        // after SHUT_WR, without feeding them to the parser or dispatcher.
        // Otherwise closing with unread bytes can reset a valid 400/413/431
        // response. Backend and exact-response assertions remain unchanged.
        char discarded[4096];
        for (;;) {
            const ssize_t count = recv(sockets[0], discarded,
                                       sizeof(discarded), MSG_DONTWAIT);
            if (count > 0 || (count < 0 && errno == EINTR)) {
                continue;
            }
            require(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK,
                    "cannot finish fragmented Admin HTTP fixture");
            break;
        }
        close(sockets[0]);
    });
    std::size_t fragments_sent = 0;
    for (const auto& fragment : fragments) {
        send_all(sockets[1], fragment);
        ++fragments_sent;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        for (;;) {
            int available = -1;
            require(ioctl(sockets[0], FIONREAD, &available) == 0,
                    "cannot observe Admin HTTP fragment consumption");
            if (available == 0 || finished.load()) {
                break;
            }
            require(std::chrono::steady_clock::now() < deadline,
                    "Admin HTTP fragment was not consumed");
            // Keep FIONREAD as the per-fragment read barrier, but do not
            // make every byte pay for a timer. Darwin may coalesce short
            // sleeps enough to exhaust the real HTTP phase deadline before
            // this fixture has delivered the bytes that exercise a limit.
            std::this_thread::yield();
        }
        if (finished.load()) {
            break;
        }
    }
    require(allow_early_response || fragments_sent == fragments.size(),
            "Admin HTTP accepted a request before all fragments were sent");
    require(shutdown(sockets[1], SHUT_WR) == 0,
            "cannot finish fragmented Admin HTTP request");
    sent.store(true);
    const std::string response = receive_to_eof(sockets[1]);
    close(sockets[1]);
    server.join();
    require(served, "fragmented Admin HTTP connection failed: " + server_error);
    return response;
}

std::vector<std::string> byte_fragments(const std::string& bytes) {
    std::vector<std::string> fragments;
    for (const char byte : bytes) {
        fragments.emplace_back(1, byte);
    }
    return fragments;
}

void require_json_http_response(const std::string& response,
                                unsigned status) {
    const unsigned actual_status = response_status(response);
    require(actual_status == status,
            "Admin HTTP response expected status " + std::to_string(status) +
                ", got " + std::to_string(actual_status));
    require(response.find("\r\nContent-Type: application/json\r\n") !=
                std::string::npos,
            "Admin HTTP response omitted its JSON content type");
    require(response.find("\r\nConnection: close\r\n") !=
                std::string::npos,
            "Admin HTTP connection was not explicitly closed");
    const std::string body = response_body(response);
    require(!body.empty() && body.front() == '{' && body.back() == '}',
            "Admin HTTP body is not one JSON object");
    const std::string length_header =
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    require(response.find(length_header) != std::string::npos,
            "Admin HTTP Content-Length does not match its body");
}

#endif

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected one Admin HTTP test mode");
    const std::string mode = argv[1];

#if !CAPSID_HAS_ADMIN_HTTP
    fail("Unix Admin HTTP transport is not implemented: " + mode);
#else
    if (mode == "host_admin_http_authorized_round_trip") {
        FakeAdminBackend backend;
        const std::string body = "{\"app\":\"orders\",\"version\":\"v1\"}";
        const std::string request =
            "POST /v1/deploy HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) +
            "\r\n\r\n" + body;
        const std::string response = exchange(http_options(), &backend,
                                              request);
        require_json_http_response(response, 202);
        require(backend.deploy_calls == 1 &&
                    backend.last_application == "orders" &&
                    backend.last_version == "v1",
                "Admin HTTP changed deploy dispatch");
        require(response_body(response).find("op-http-1") !=
                    std::string::npos,
                "Admin HTTP response omitted operation identity");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_fragmented_headers") {
        FakeAdminBackend backend;
        const std::string body = "{\"app\":\"orders\",\"version\":\"v1\"}";
        const std::string request =
            "POST /v1/deploy HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        require_json_http_response(fragmented_exchange(
            http_options(), &backend, byte_fragments(request)), 202);
        require(backend.deploy_calls == 1 &&
                    backend.last_application == "orders" &&
                    backend.last_version == "v1",
                "fragmented Admin request changed deploy dispatch");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_chunked_requests") {
        const std::string body = "{\"app\":\"orders\",\"version\":\"v1\"}";
        // 0x1f == 31 bytes. The extension and trailer exercise structured
        // elements which Beast may leave unconsumed even without need_more.
        require(body.size() == 31, "chunk fixture length changed");
        const std::string request =
            "POST /v1/deploy HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
            "10;part=first\r\n" + body.substr(0, 16) +
            "\r\nf\r\n" + body.substr(16) + "\r\n0\r\nX-End: yes\r\n\r\n";
        for (const bool fragmented : {false, true}) {
            FakeAdminBackend backend;
            const auto fragments = fragmented ? byte_fragments(request) :
                std::vector<std::string>{request};
            require_json_http_response(fragmented_exchange(
                http_options(), &backend, fragments), 202);
            require(backend.deploy_calls == 1 &&
                        backend.last_application == "orders" &&
                        backend.last_version == "v1",
                    "chunked Admin request changed deploy dispatch");
        }
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_chunked_errors") {
        const std::string header =
            "POST /v1/deploy HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n";
        for (const bool fragmented : {false, true}) {
            for (const auto& body : {std::string("Z\r\n"),
                                    std::string("1\r\nx!\r\n0\r\n\r\n")}) {
                FakeAdminBackend backend;
                const auto fragments = fragmented ? byte_fragments(header + body) :
                    std::vector<std::string>{header + body};
                require_json_http_response(fragmented_exchange(
                    http_options(), &backend, fragments, true), 400);
                require(backend.total_calls() == 0,
                        "invalid chunked Admin request reached the backend");
            }
        }
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_fragmented_limits") {
        auto options = http_options();
        options.api.max_header_bytes = 128;
        options.api.max_body_bytes = 32;
        FakeAdminBackend backend;
        const std::string header =
            "GET /v1/apps/orders HTTP/1.1\r\nX-Pad: " +
            std::string(160, 'x') + "\r\n\r\n";
        require_json_http_response(fragmented_exchange(
            options, &backend, byte_fragments(header), true), 431);
        const std::string chunked =
            "POST /v1/deploy HTTP/1.1\r\n"
            "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
            "10\r\n" + std::string(16, 'x') + "\r\n11\r\n" +
            std::string(17, 'x') + "\r\n0\r\n\r\n";
        require_json_http_response(fragmented_exchange(
            options, &backend, byte_fragments(chunked), true), 413);
        require(backend.total_calls() == 0,
                "fragmented Admin limit reached the backend");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_buffer_boundaries") {
        auto options = http_options();
        options.api.max_header_bytes = 8192;
        options.api.max_body_bytes = 8192;
        const std::string body = "{\"app\":\"orders\",\"version\":\"v1\"}";
        const std::string header =
            "POST /v1/deploy HTTP/1.1\r\nHost: localhost\r\nX-Pad: " +
            std::string(5000, 'x') +
            "\r\nContent-Type: application/json\r\nContent-Length: 8192\r\n\r\n";
        const std::string request = header + body +
            std::string(options.api.max_body_bytes - body.size(), ' ');
        std::vector<std::string> fragments;
        for (std::size_t offset = 0; offset < request.size(); offset += 997) {
            fragments.push_back(request.substr(offset, 997));
        }
        FakeAdminBackend backend;
        require_json_http_response(fragmented_exchange(
            options, &backend, fragments), 202);
        require(backend.deploy_calls == 1 &&
                    backend.last_application == "orders" &&
                    backend.last_version == "v1",
                "Admin buffer boundary changed deploy dispatch");

        // An unfinished chunk extension is not decoded body data; its
        // storage must be bounded even though the body ceiling is not hit.
        const std::string chunk_header =
            "POST /v1/deploy HTTP/1.1\r\n"
            "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n";
        fragments = {chunk_header, "1;pad="};
        for (unsigned index = 0; index < 7; ++index) {
            fragments.push_back(std::string(512, 'x'));
        }
        // Stop exactly at the wire-metadata ceiling, without leaving extra
        // unread bytes which would make a Unix close reset the response.
        fragments.push_back(std::string(4096 - 6 - 7 * 512, 'x'));
        FakeAdminBackend rejected;
        require_json_http_response(fragmented_exchange(
            http_options(), &rejected, fragments, true), 400);
        require(rejected.total_calls() == 0,
                "oversized Admin chunk metadata reached the backend");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_peer_rejected_before_read") {
        capsid::host::AdminHttpOptions options = http_options();
        options.api.authorization.allowed_uid =
            static_cast<std::uint64_t>(geteuid()) + 1U;
        options.api.authorization.allowed_gid = std::nullopt;
        FakeAdminBackend backend;
        // Send no HTTP bytes. A 403 proves the accepted connection was bound
        // to its kernel peer before the parser waited on attacker input.
        const std::string response = exchange(options, &backend, {}, false);
        require_json_http_response(response, 403);
        require(backend.total_calls() == 0,
                "unauthorized Admin HTTP peer reached the backend");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_framing_limits") {
        capsid::host::AdminHttpOptions options = http_options();
        options.api.max_header_bytes = 128;
        options.api.max_body_bytes = 32;
        FakeAdminBackend backend;
        const std::string large_header =
            "GET /v1/apps/orders HTTP/1.1\r\nHost: localhost\r\nX-Pad: " +
            std::string(160, 'x') + "\r\n\r\n";
        require_json_http_response(exchange(options, &backend, large_header),
                                   431);
        const std::string declared_large_body =
            "POST /v1/deploy HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 4096\r\n\r\n";
        require_json_http_response(
            exchange(options, &backend, declared_large_body), 413);
        require(backend.total_calls() == 0,
                "wire-level Admin limit reached the backend");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_smuggling_rejected") {
        FakeAdminBackend backend;
        const std::string request =
            "POST /v1/deploy HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 2\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "0\r\n\r\n";
        require_json_http_response(exchange(http_options(), &backend,
                                             request),
                                   400);
        require(backend.total_calls() == 0,
                "ambiguous Admin framing reached the backend");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_slow_header_timeout") {
        capsid::host::AdminHttpOptions options = http_options();
        options.header_timeout_ms = 100;
        FakeAdminBackend backend;
        const auto started = std::chrono::steady_clock::now();
        const std::string response = exchange(
            options, &backend, "POST /v1/deploy HTTP/1.1\r\nHost: local",
            false);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require_json_http_response(response, 408);
        require(elapsed < std::chrono::seconds(2),
                "slow Admin header was not bounded by its deadline");
        require(backend.total_calls() == 0,
                "partial Admin header reached the backend");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_slow_drip_deadlines") {
        capsid::host::AdminHttpOptions options = http_options();
        options.header_timeout_ms = 100;
        options.body_timeout_ms = 100;
        FakeAdminBackend backend;

        std::chrono::steady_clock::duration header_elapsed = {};
        const std::string header_response = drip_exchange(
            options, &backend,
            "GET /v1/apps/orders HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "X-Drip: ",
            std::string(20, 'x'),
            &header_elapsed);
        require_json_http_response(header_response, 408);
        require(header_elapsed < std::chrono::milliseconds(600),
                "Admin header deadline was reset by slow drip bytes");

        const std::string body_prefix =
            "POST /v1/deploy HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 32\r\n\r\n";
        std::chrono::steady_clock::duration body_elapsed = {};
        const std::string body_response = drip_exchange(
            options, &backend, body_prefix, std::string(20, 'x'),
            &body_elapsed);
        require_json_http_response(body_response, 408);
        require(body_elapsed < std::chrono::milliseconds(600),
                "Admin body deadline was reset by slow drip bytes");
        require(backend.total_calls() == 0,
                "slow-drip Admin request reached the backend");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_accepted_fd_remains_caller_owned") {
        // The long-lived service owns active_fd until it clears the slot
        // under its shutdown mutex. The accepted-connection transport must
        // therefore never close the caller's descriptor behind its back;
        // serve_one is the wrapper that owns and closes its accepted fd.
        int sockets[2] = {-1, -1};
        require(socketpair(AF_UNIX, SOCK_STREAM, 0,
                           sockets) == 0,
                "cannot create accepted-fd ownership fixture");
        FakeAdminBackend backend;
        bool served = false;
        std::string server_error;
        std::thread server([&]() {
            served = capsid::host::serve_accepted_admin_http_connection(
                sockets[0], http_options(), &backend, &server_error);
        });
        const std::string body = "{\"app\":\"orders\",\"version\":\"v1\"}";
        const std::string request =
            "POST /v1/deploy HTTP/1.1\r\nHost: local\r\n"
            "Content-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        send_all(sockets[1], request);
        require(shutdown(sockets[1], SHUT_WR) == 0,
                "cannot finish accepted-fd ownership request");
        server.join();
        require(served, "accepted-fd transport failed: " + server_error);
        require(fcntl(sockets[0], F_GETFD) >= 0,
                "accepted-fd transport closed its caller-owned descriptor");
        close(sockets[0]);
        close(sockets[1]);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_http_closed_peer_does_not_raise_sigpipe") {
        // A management client may disappear after sending a complete request
        // but before reading the response. Exercise the transport under the
        // process-default SIGPIPE disposition: Linux uses MSG_NOSIGNAL while
        // platforms without that flag (notably macOS) must suppress SIGPIPE
        // on the socket itself. A temporary handler lets the test observe
        // any attempted SIGPIPE without allowing it to terminate CTest.
        int sockets[2] = {-1, -1};
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
                "cannot create Admin SIGPIPE fixture");
        const std::string body =
            "{\"app\":\"orders\",\"version\":\"v1\"}";
        const std::string request =
            "POST /v1/deploy HTTP/1.1\r\nHost: local\r\n"
            "Content-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        send_all(sockets[1], request);
        close(sockets[1]);

        struct sigaction action = {};
        action.sa_handler = record_sigpipe;
        sigemptyset(&action.sa_mask);
        struct sigaction previous = {};
        require(sigaction(SIGPIPE, &action, &previous) == 0,
                "cannot install Admin SIGPIPE observer");
        sigpipe_count = 0;
        FakeAdminBackend backend;
        std::string server_error;
        const bool served = capsid::host::serve_accepted_admin_http_connection(
            sockets[0], http_options(), &backend, &server_error);
        const sig_atomic_t observed = sigpipe_count;
        require(sigaction(SIGPIPE, &previous, nullptr) == 0,
                "cannot restore Admin SIGPIPE disposition");
        close(sockets[0]);
        require(!served, "closed Admin peer unexpectedly accepted a response");
        require(observed == 0,
                "closed Admin peer raised SIGPIPE in the Host");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    fail("unknown Admin HTTP test mode: " + mode);
#endif
}
