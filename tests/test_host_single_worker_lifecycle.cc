// Frozen M2 RED: split the existing single-worker benchmark server into a
// controllable start/request_stop/wait lifecycle without changing its HTTP
// behavior. StaticPoolServer will compose this lifecycle in the next batch.

#include "host/single_worker_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
// macOS does not define SOCK_CLOEXEC; the IPC pair does not cross exec on
// this test path, so a plain socket type is the portable fallback.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

std::string read_ready_line(int fd) {
    std::string line;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (line.find('\n') == std::string::npos) {
        require(std::chrono::steady_clock::now() < deadline,
                "server did not publish READY after start returned");
        struct pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = poll(&descriptor, 1, 50);
        require(polled >= 0, "cannot poll READY pipe");
        if (polled == 0) {
            continue;
        }
        char bytes[512];
        const ssize_t count = read(fd, bytes, sizeof(bytes));
        require(count > 0, "READY pipe closed without a record");
        line.append(bytes, static_cast<std::size_t>(count));
    }
    return line.substr(0, line.find('\n'));
}

std::uint16_t ready_port(const std::string& line) {
    const std::string marker = "\"port\":";
    const std::string::size_type begin = line.find(marker);
    require(begin != std::string::npos, "READY record has no port");
    const char* digits = line.c_str() + begin + marker.size();
    char* end = nullptr;
    const unsigned long port = std::strtoul(digits, &end, 10);
    require(end != digits && port > 0 && port <= 65535,
            "READY record has an invalid port");
    return static_cast<std::uint16_t>(port);
}

void require_http_response(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "cannot create lifecycle HTTP socket");
    struct timeval timeout = {};
    timeout.tv_sec = 3;
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot set lifecycle HTTP timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode loopback address");
    require(connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect after server start");
    const std::string request =
        "GET /@capsid/orders/lifecycle HTTP/1.1\r\n"
        "Host: public.example\r\n"
        "Connection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            send(fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "cannot write lifecycle HTTP request");
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char bytes[2048];
    for (;;) {
        const ssize_t count = recv(fd, bytes, sizeof(bytes), 0);
        if (count == 0) {
            break;
        }
        require(count > 0, "cannot read lifecycle HTTP response");
        response.append(bytes, static_cast<std::size_t>(count));
    }
    close(fd);
    require(response.find(" 200 ") != std::string::npos &&
                response.find("lifecycle-ok") != std::string::npos,
            "started server did not preserve single-worker HTTP behavior");
}

void require_port_closed(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "cannot create closed-port probe socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode closed-port probe address");
    const int connected = connect(
        fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address));
    close(fd);
    require(connected != 0,
            "destroying a running server left its listener active");
}

std::uint16_t reserve_test_port() {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "cannot create port-reservation socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode port-reservation address");
    require(bind(fd, reinterpret_cast<struct sockaddr*>(&address),
                 sizeof(address)) == 0,
            "cannot reserve lifecycle test port");
    socklen_t length = sizeof(address);
    require(getsockname(fd, reinterpret_cast<struct sockaddr*>(&address),
                        &length) == 0,
            "cannot inspect lifecycle test port");
    const std::uint16_t port = ntohs(address.sin_port);
    close(fd);
    require(port != 0, "kernel selected an invalid lifecycle test port");
    return port;
}

void require_port_bindable(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "cannot create listener-cleanup probe socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode listener-cleanup probe address");
    const int bound = bind(fd, reinterpret_cast<struct sockaddr*>(&address),
                           sizeof(address));
    close(fd);
    require(bound == 0,
            "failed start returned while its listener was still bound");
}

capsid::host::SingleWorkerServerOptions make_options(
    const char* worker_path, int ready_fd) {
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = worker_path;
    options.source_bundle_path = "lifecycle-inline";
    options.source_name = "file://orders/v1/bundle.mjs";
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

template <typename Server>
void exercise_lifecycle(Server* server,
                        const std::vector<std::uint8_t>& bundle,
                        int ready_read_fd,
                        int ready_write_fd) {
    if constexpr (requires(Server& value,
                           const std::vector<std::uint8_t>& bytes,
                           std::string* error) {
                      { value.start(bytes, error) } -> std::same_as<bool>;
                      value.request_stop();
                      { value.wait(error) } -> std::same_as<bool>;
                  }) {
        std::string error;
        require(server->start(bundle, &error),
                "controllable server start failed: " + error);
        require(!server->start(bundle, &error),
                "server accepted a duplicate start");
        close(ready_write_fd);
        const std::uint16_t port = ready_port(read_ready_line(ready_read_fd));
        require_http_response(port);

        const auto began = std::chrono::steady_clock::now();
        server->request_stop();
        server->request_stop();
        require(server->wait(&error), "server wait failed: " + error);
        require(std::chrono::steady_clock::now() - began <
                    std::chrono::seconds(2),
                "server stop/wait exceeded its bounded shutdown window");
    } else {
        (void)server;
        (void)bundle;
        (void)ready_read_fd;
        close(ready_write_fd);
        fail("SingleWorkerServer has no start/request_stop/wait lifecycle");
    }
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected capsid-worker path");
    int ready[2];
    require(pipe(ready) == 0, "cannot create READY pipe");

    const std::string source =
        "export default { fetch: () => new Response('lifecycle-ok') };";
    const std::vector<std::uint8_t> bundle(source.begin(), source.end());

    capsid::host::SingleWorkerServer server(make_options(argv[1], ready[1]));
    exercise_lifecycle(&server, bundle, ready[0], ready[1]);
    close(ready[0]);

    // The public facade, not Impl::~Impl(), owns the last reliable chance
    // to stop: long-lived accept/signal handlers retain shared_ptr<Impl> and
    // can otherwise prevent Impl's destructor from ever starting.
    int destructor_ready[2];
    require(pipe(destructor_ready) == 0,
            "cannot create destructor READY pipe");
    std::uint16_t destructor_port = 0;
    const auto destructor_began = std::chrono::steady_clock::now();
    {
        capsid::host::SingleWorkerServer abandoned(
            make_options(argv[1], destructor_ready[1]));
        std::string error;
        require(abandoned.start(bundle, &error),
                "cannot start destructor lifecycle fixture: " + error);
        close(destructor_ready[1]);
        destructor_port = ready_port(read_ready_line(destructor_ready[0]));
        require_http_response(destructor_port);
        // No explicit request_stop/wait: ~SingleWorkerServer must perform
        // the same bounded teardown and drain handlers retaining Impl.
    }
    require(std::chrono::steady_clock::now() - destructor_began <
                std::chrono::seconds(2),
            "running server destructor exceeded its bounded shutdown window");
    require_port_closed(destructor_port);
    close(destructor_ready[0]);

    // A failure after bind (READY publication here) must unwind the listener
    // and worker before start() returns. StaticPoolServer depends on this to
    // make an N-shard startup failure atomic while shard objects remain alive.
    const std::uint16_t failed_start_port = reserve_test_port();
    const int read_only_ready_fd =
        open("/dev/null", O_RDONLY | O_CLOEXEC);
    require(read_only_ready_fd >= 0,
            "cannot create failed-READY lifecycle fixture");
    auto failed_options = make_options(argv[1], read_only_ready_fd);
    failed_options.listen_port = failed_start_port;
    capsid::host::SingleWorkerServer failed_start(
        std::move(failed_options));
    std::string failed_error;
    const auto failed_began = std::chrono::steady_clock::now();
    require(!failed_start.start(bundle, &failed_error),
            "server accepted an unwritable READY descriptor");
    require(std::chrono::steady_clock::now() - failed_began <
                std::chrono::seconds(2),
            "failed start exceeded its bounded cleanup window");
    require_port_bindable(failed_start_port);
    close(read_only_ready_fd);

    std::cout << "PASS" << std::endl;
    return 0;
}
