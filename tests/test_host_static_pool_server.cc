// Frozen M2 Batch B RED: compose one independently owned HTTP/reactor/worker
// shard per fixed-pool member. This batch freezes only shared-port ownership
// and atomic lifecycle; queueing, P2C and SSE admission belong to later work.

#if __has_include("host/static_pool_server.h")
#include "host/static_pool_server.h"
#define CAPSID_HAS_STATIC_POOL_SERVER 1
#else
#define CAPSID_HAS_STATIC_POOL_SERVER 0
#endif

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <arpa/inet.h>
#endif
#include <fcntl.h>
#include "win32_compat.h"
#include <signal.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/socket.h>
#endif

// macOS does not define SOCK_CLOEXEC; these IPC pairs do not cross exec
// on the test paths, so a plain socket type is the portable fallback.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/time.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/wait.h>
#endif

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

#if CAPSID_HAS_STATIC_POOL_SERVER

std::string read_one_ready_line(int fd) {
    std::string line;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (line.empty() || line.back() != '\n') {
        require(std::chrono::steady_clock::now() < deadline,
                "pool did not publish READY after start returned");
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = capsid::win32::capsid_poll(&descriptor, 1, 50);
        require(polled >= 0, "cannot poll pool READY pipe");
        if (polled == 0) {
            continue;
        }
        char byte = 0;
#if defined(_WIN32)
        require(capsid::win32::read_fd(fd, &byte, 1) == 1,
#else
        require(read(fd, &byte, 1) == 1,
#endif
                "pool READY pipe closed without a complete record");
        line.push_back(byte);
        require(line.size() <= 1024, "pool READY record is unbounded");
    }
    return line;
}

void require_no_second_ready_record(int fd) {
    capsid_pollfd descriptor = {};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const int polled = capsid::win32::capsid_poll(&descriptor, 1, 100);
    require(polled == 0,
            "pool exposed per-shard READY records instead of one pool record");
}

std::uint16_t ready_port(const std::string& line) {
    const std::string marker = "\"port\":";
    const std::string::size_type begin = line.find(marker);
    require(begin != std::string::npos, "pool READY record has no port");
    const char* digits = line.c_str() + begin + marker.size();
    char* end = nullptr;
    const unsigned long port = std::strtoul(digits, &end, 10);
    require(end != digits && port > 0 && port <= 65535,
            "pool READY record has an invalid port");
    return static_cast<std::uint16_t>(port);
}

void require_http_response(std::uint16_t port) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create static-pool HTTP socket");
    require(capsid::win32::setsockopt_recv_timeout_fd(fd, 3000) == 0,
            "cannot set static-pool HTTP timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode static-pool loopback address");
    require(capsid::win32::connect_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to active static pool");
    const std::string request =
        "GET /@capsid/orders/static-pool HTTP/1.1\r\n"
        "Host: public.example\r\n"
        "Connection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            capsid::win32::send_fd(fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "cannot write static-pool HTTP request");
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char bytes[2048];
    for (;;) {
        const ssize_t count = capsid::win32::recv_fd(fd, bytes, sizeof(bytes), 0);
        if (count == 0) {
            break;
        }
        require(count > 0, "cannot read static-pool HTTP response");
        response.append(bytes, static_cast<std::size_t>(count));
    }
    close(fd);
    require(response.find(" 200 ") != std::string::npos &&
                response.find("static-pool-ok") != std::string::npos,
            "static pool did not preserve shard HTTP behavior");
}

std::uint16_t reserve_test_port() {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create pool port-reservation socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode pool port-reservation address");
    require(capsid::win32::bind_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                 sizeof(address)) == 0,
            "cannot reserve static-pool test port");
    socklen_t length = sizeof(address);
    require(capsid::win32::getsockname_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                        &length) == 0,
            "cannot inspect static-pool test port");
    const std::uint16_t port = ntohs(address.sin_port);
    close(fd);
    require(port != 0, "kernel selected an invalid static-pool test port");
    return port;
}

void require_port_bindable(std::uint16_t port, const std::string& message) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create pool listener probe socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode pool listener probe address");
    const int bound = capsid::win32::bind_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                           sizeof(address));
    close(fd);
    require(bound == 0, message);
}

void require_port_closed(std::uint16_t port) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create stopped-pool probe socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode stopped-pool probe address");
    const int connected = capsid::win32::connect_fd(
        fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address));
    close(fd);
    require(connected != 0, "stopped static pool still accepted connections");
}

capsid::host::StaticPoolServerOptions make_options(
    const char* worker_path, int ready_fd, std::uint32_t workers) {
    capsid::host::StaticPoolServerOptions options;
    options.workers = workers;
    options.worker_options.worker_path = worker_path;
    options.worker_options.source_bundle_path = "static-pool-inline";
    options.worker_options.source_name = "file://orders/v1/bundle.mjs";
    options.worker_options.application = "orders";
    options.worker_options.listen_address = "127.0.0.1";
    options.worker_options.listen_port = 0;
    options.worker_options.public_scheme = "http";
    options.worker_options.public_authority = "public.example";
    options.worker_options.request_timeout_ms = 5000;
    options.worker_options.initial_stream_window = 64U * 1024U;
    options.worker_options.strict_sandbox = false;
    options.worker_options.ready_fd = ready_fd;
    return options;
}

const std::vector<std::uint8_t>& fixture_bundle() {
    static const std::string source =
        "export default { fetch: () => new Response('static-pool-ok') };";
    static const std::vector<std::uint8_t> bundle(source.begin(), source.end());
    return bundle;
}

// The slow fixture: every request parks in the worker for 700 ms, so a test
// can hold one inflight slot deterministically (same pattern as E-1).
const std::vector<std::uint8_t>& slow_bundle() {
    static const std::string source =
        "export default { fetch: async () => { "
        "  await new Promise((resolve) => setTimeout(resolve, 700)); "
        "  return new Response('static-pool-ok'); } };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

// The hang fixture: the fetch promise never settles, so the request stays
// inflight until the Host cancels it. The Host-side worker request timeout
// must be disabled in the options for this to model a wedged worker.
const std::vector<std::uint8_t>& hang_bundle() {
    static const std::string source =
        "export default { fetch: () => new Promise(() => {}) };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

int connect_to(std::uint16_t port) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create drain HTTP socket");
    require(capsid::win32::setsockopt_recv_timeout_fd(fd, 5000) == 0,
            "cannot set drain HTTP receive timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode drain loopback address");
    require(capsid::win32::connect_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to draining pool");
    return fd;
}

void send_hold_request(int fd) {
    const std::string request =
        "GET /@capsid/orders/hold HTTP/1.1\r\n"
        "Host: public.example\r\n"
        "Connection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            capsid::win32::send_fd(fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "cannot write drain HTTP request");
        sent += static_cast<std::size_t>(count);
    }
}

// Reads the connection until the server closes it. A normal response ends
// with EOF because the drain fixture sends Connection: close; a drained
// (force-cancelled) connection also ends in EOF/RST.
std::string read_until_close(int fd) {
    std::string response;
    char bytes[2048];
    for (;;) {
        const ssize_t count = capsid::win32::recv_fd(fd, bytes, sizeof(bytes), 0);
        if (count == 0) {
            return response;  // clean EOF
        }
        if (count < 0) {
            return response;  // RST: the server force-closed the session
        }
        response.append(bytes, static_cast<std::size_t>(count));
    }
}

// §7.5 rows 1-3, 7 (normal path): after begin_drain the listener must stop
// accepting new connections, the held inflight request must complete
// normally (200, no cancellation), and the pool must then shut itself down
// — the drain report shows zero forced cancellations and a positive drain
// time.
void test_drain_inflight_completes(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create drain READY pipe");
    capsid::host::StaticPoolServer pool(
        make_options(worker_path, ready[1], 1));
    std::string error;
    require(pool.start(slow_bundle(), &error),
            "cannot start draining pool: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    // Hold the single inflight slot (the slow bundle answers in ~700 ms).
    const int holder = connect_to(port);
    send_hold_request(holder);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    pool.begin_drain(5000);
    // Row 1: the listener stops accepting once the drain lands on the io
    // thread; a new connection must be refused.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require_port_closed(port);

    // Row 2: the held request keeps running and completes normally.
    const std::string response = read_until_close(holder);
    close(holder);
    require(response.find(" 200 ") != std::string::npos &&
                response.find("static-pool-ok") != std::string::npos,
            "drained inflight request did not complete normally");

    // Row 3: inflight cleared → the pool shuts itself down; wait() must
    // return without any further stop request and within a bounded window.
    const auto began = std::chrono::steady_clock::now();
    require(pool.wait(&error), "drained pool wait failed: " + error);
    require(std::chrono::steady_clock::now() - began <
                std::chrono::seconds(3),
            "drain shutdown exceeded its bounded window");
    require(pool.active_workers() == 0,
            "drained pool still reports active worker shards");

    // Row 7: the drain report.
    const capsid::host::SingleWorkerServer::DrainMetrics metrics =
        pool.drain_metrics();
    require(metrics.draining && metrics.finished,
            "drain report does not describe a finished drain");
    require(metrics.forced_cancellations == 0,
            "a naturally drained pool reported forced cancellations");
    require(metrics.total_ms > 0,
            "drain report omitted its total drain time");
    close(ready[0]);
    close(ready[1]);
}

// §7.5 rows 4-5, 7 (bounded path): a wedged inflight request is not waited
// for forever — the drain deadline cancels it (counted), the grace period
// runs, and the pool still terminates; the report records the forced
// cancellation.
void test_drain_deadline_forces(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create drain deadline READY pipe");
    auto options = make_options(worker_path, ready[1], 1);
    options.worker_options.request_timeout_ms = 0;  // wedged worker model
    capsid::host::StaticPoolServer pool(std::move(options));
    std::string error;
    require(pool.start(hang_bundle(), &error),
            "cannot start drain deadline pool: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    // The hung request holds the only inflight slot forever.
    const int holder = connect_to(port);
    send_hold_request(holder);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // A short deadline: the drain must not wait for the wedged worker.
    pool.begin_drain(600);
    // Row 4-5: the deadline cancels the request and the server closes the
    // session; the client sees EOF/RST, then wait() returns.
    const std::string response = read_until_close(holder);
    close(holder);
    require(response.empty() ||
                response.find(" 200 ") == std::string::npos,
            "a force-cancelled request completed with a 200 response");

    const auto began = std::chrono::steady_clock::now();
    require(pool.wait(&error), "force-drained pool wait failed: " + error);
    require(std::chrono::steady_clock::now() - began <
                std::chrono::seconds(3),
            "force drain exceeded its bounded window");
    require(pool.active_workers() == 0,
            "force-drained pool still reports active worker shards");

    const capsid::host::SingleWorkerServer::DrainMetrics metrics =
        pool.drain_metrics();
    require(metrics.finished,
            "force drain report is not finished");
    require(metrics.forced_cancellations == 1,
            "drain deadline did not count its forced cancellation");
    require(metrics.total_ms > 0,
            "force drain report omitted its total drain time");
    close(ready[0]);
    close(ready[1]);
}

// §7.5 row 3 direct path: a pool with no inflight work shuts down
// immediately when the drain begins — no deadline wait, no cancellation.
void test_drain_idle_exits(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create idle drain READY pipe");
    capsid::host::StaticPoolServer pool(
        make_options(worker_path, ready[1], 1));
    std::string error;
    require(pool.start(fixture_bundle(), &error),
            "cannot start idle draining pool: " + error);
    (void)ready_port(read_one_ready_line(ready[0]));

    pool.begin_drain(60000);  // an idle drain must not wait anywhere near this
    const auto began = std::chrono::steady_clock::now();
    require(pool.wait(&error), "idle drained pool wait failed: " + error);
    require(std::chrono::steady_clock::now() - began <
                std::chrono::seconds(3),
            "idle drain did not shut down promptly");

    const capsid::host::SingleWorkerServer::DrainMetrics metrics =
        pool.drain_metrics();
    require(metrics.finished && metrics.forced_cancellations == 0,
            "idle drain reported an unexpected forced cancellation");
    close(ready[0]);
    close(ready[1]);
}

void test_shared_port_lifecycle(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create static-pool READY pipe");
    capsid::host::StaticPoolServer pool(make_options(worker_path, ready[1], 3));
    std::string error;
    require(pool.start(fixture_bundle(), &error),
            "cannot start three-shard static pool: " + error);
    require(!pool.start(fixture_bundle(), &error),
            "static pool accepted a duplicate start");
    require(pool.active_workers() == 3,
            "active pool does not own exactly three worker shards");
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));
    require_no_second_ready_record(ready[0]);
    for (int request = 0; request < 24; ++request) {
        require_http_response(port);
    }
    const auto began = std::chrono::steady_clock::now();
    pool.request_stop();
    pool.request_stop();
    require(pool.wait(&error), "static pool wait failed: " + error);
    require(std::chrono::steady_clock::now() - began <
                std::chrono::seconds(2),
            "static pool stop/wait exceeded its bounded shutdown window");
    require(pool.active_workers() == 0,
            "stopped pool still reports active worker shards");
    require_port_closed(port);
    close(ready[0]);
    close(ready[1]);
}

void test_atomic_start_failure(const char* worker_path) {
    const std::uint16_t port = reserve_test_port();
#if defined(_WIN32)
    const int read_only_ready_fd = _open("NUL", _O_RDONLY);
#else
    const int read_only_ready_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
#endif
    require(read_only_ready_fd >= 0,
            "cannot create failed pool READY fixture");
    auto options = make_options(worker_path, read_only_ready_fd, 3);
    options.worker_options.listen_port = port;
    capsid::host::StaticPoolServer pool(std::move(options));
    std::string error;
    const auto began = std::chrono::steady_clock::now();
    require(!pool.start(fixture_bundle(), &error),
            "pool accepted an unwritable public READY descriptor");
    require(std::chrono::steady_clock::now() - began <
                std::chrono::seconds(3),
            "failed pool start exceeded its bounded rollback window");
    require(pool.active_workers() == 0,
            "failed pool start retained active worker shards");
    require_port_bindable(
        port, "failed pool start returned while a shard listener remained");
    pool.request_stop();
    require(pool.wait(&error), "failed pool could not complete wait: " + error);
    close(read_only_ready_fd);

    auto zero_options = make_options(worker_path, -1, 0);
    capsid::host::StaticPoolServer zero(std::move(zero_options));
    require(!zero.start(fixture_bundle(), &error),
            "zero-worker static pool activated");
    require(zero.active_workers() == 0,
            "zero-worker static pool reported an active shard");
}

// Lifecycle hardening: request_stop() before start() must fail start
// synchronously with every resource unwound — no listener, no shards, and
// wait() must still complete without a thread that never started.
void test_stop_before_start(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create static-pool READY pipe");
    capsid::host::StaticPoolServer pool(make_options(worker_path, ready[1], 2));
    pool.request_stop();
    std::string error;
    require(!pool.start(fixture_bundle(), &error),
            "static pool started after a prior stop request");
    require(!error.empty(), "stop-before-start produced no error text");
    require(pool.active_workers() == 0,
            "stop-before-start pool retained worker shards");
    pool.request_stop();
    require(pool.wait(&error),
            "stop-before-start pool did not wait cleanly: " + error);
    close(ready[0]);
    close(ready[1]);
}

// Lifecycle hardening: a stop that arrives while start() is mid-shard must
// never leave a partial pool running. The stop gate fails start, and the
// starting shard is reclaimed by the atomic rollback (starting_shard_
// publication) rather than leaked into a live pool.
void test_start_stop_race(const char* worker_path) {
#if defined(_WIN32)
    // Windows has no fork: the probe runs on a plain thread (the pool
    // facade is thread-safe and the probe is crash-free by contract).
    std::thread child_thread([&]() {
        for (int round = 0; round < 8; ++round) {
            int ready[2];
            require(capsid::win32::create_socket_pair(ready),
                    "cannot create race READY pipe");
            capsid::host::StaticPoolServer pool(
                make_options(worker_path, ready[1], 1));
            std::thread racer([&]() {
                capsid::win32::usleep(
                    static_cast<unsigned long>(round % 3) * 200U);
                pool.request_stop();
            });
            std::string error;
            const bool started = pool.start(fixture_bundle(), &error);
            racer.join();
            pool.request_stop();
            require(pool.wait(&error),
                    "start/stop race wedged pool wait: " + error);
            require(pool.active_workers() == 0,
                    "start/stop race leaked worker shards");
            close(ready[0]);
            close(ready[1]);
            (void)started;
        }
    });
    child_thread.join();
#else
    const pid_t child = fork();
    require(child >= 0, "cannot fork static-pool start/stop race probe");
    if (child == 0) {
        // Eight rounds of one shard: each round spawns and tears down a
        // real worker process, so the probe stays inside its bounded
        // window while still interleaving stop across every start phase.
        for (int round = 0; round < 8; ++round) {
            int ready[2];
            require(capsid::win32::create_socket_pair(ready),
                    "cannot create race READY pipe");
            capsid::host::StaticPoolServer pool(
                make_options(worker_path, ready[1], 1));
            std::thread racer([&]() {
                usleep(static_cast<useconds_t>(round % 3) * 200U);
                pool.request_stop();
            });
            std::string error;
            const bool started = pool.start(fixture_bundle(), &error);
            racer.join();
            pool.request_stop();
            require(pool.wait(&error),
                    "start/stop race wedged pool wait: " + error);
            require(pool.active_workers() == 0,
                    "start/stop race leaked worker shards");
            close(ready[0]);
            close(ready[1]);
            (void)started;
        }
        _exit(0);
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        require(waited >= 0, "cannot wait for static-pool race probe");
        if (waited == child) {
            require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "static-pool start/stop race probe failed");
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)kill(child, SIGKILL);
            (void)waitpid(child, &status, 0);
            fail("static-pool start/stop race never converged");
        }
        usleep(1000);
    }
#endif
}

#endif  // CAPSID_HAS_STATIC_POOL_SERVER

}  // namespace

int main(int argc, char** argv) {
    require(argc == 3, "expected mode and capsid-worker path");
#if !CAPSID_HAS_STATIC_POOL_SERVER
    (void)argv;
    fail("StaticPoolServer is not implemented");
#else
    const std::string mode = argv[1];
    if (mode == "lifecycle") {
        test_shared_port_lifecycle(argv[2]);
    } else if (mode == "atomic-failure") {
        test_atomic_start_failure(argv[2]);
    } else if (mode == "drain-inflight-completes") {
        test_drain_inflight_completes(argv[2]);
    } else if (mode == "drain-deadline-forces") {
        test_drain_deadline_forces(argv[2]);
    } else if (mode == "drain-idle-exits") {
        test_drain_idle_exits(argv[2]);
    } else if (mode == "stop-before-start") {
        test_stop_before_start(argv[2]);
    } else if (mode == "start-stop-race") {
        test_start_stop_race(argv[2]);
    } else {
        fail("unknown static-pool server mode");
    }
    std::cout << "PASS" << std::endl;
    return 0;
#endif
}
