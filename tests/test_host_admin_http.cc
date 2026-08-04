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

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

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

void require_json_http_response(const std::string& response,
                                unsigned status) {
    require(response_status(response) == status,
            "Admin HTTP response returned the wrong status");
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

    fail("unknown Admin HTTP test mode: " + mode);
#endif
}
