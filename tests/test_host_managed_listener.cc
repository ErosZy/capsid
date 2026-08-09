// WP-05 PR-09b §9.2: Managed listener data-plane contract. A REAL
// GenerationPool fleet (create_adopted over pre-warmed workers) behind a
// REAL listener bound on loopback, served over raw HTTP/1.1 sockets:
//   - path routing resolves the App through the shared RoutingTable
//     (one atomic snapshot load + pool pin, no re-query);
//   - 404 for a missing /@capsid/ prefix, 503 for an unrouted App,
//     504 for a worker that never answers;
//   - request-direction body credit (POST echo) and HEAD;
//   - the trusted-header gate: header routing on an untrusted listener
//     fails at bind time, and a trusted listener routes by Capsid-App;
//   - the listener connection ceiling closes excess connections.
//
// The response events travel pool pump → event_sink → listener io thread,
// so every HTTP assert here also pins the pump's event dispatch.

#include "host/generation_pool.h"
#include "host/host_config_model.h"
#include "host/managed_listener.h"
#include "host/routing_snapshot.h"
#include "host/worker_event_source.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using capsid::host::GenerationPool;
using capsid::host::GenerationPoolOptions;
using capsid::host::ListenerConfig;
using capsid::host::ManagedListener;
using capsid::host::ManagedListenerOptions;
using capsid::host::RoutingSnapshot;
using capsid::host::RoutingTable;
using capsid::host::WorkerExecutor;
using capsid::host::WorkerRecoveryPolicy;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-managed-listener: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

const char* kBundleA =
    "export default { async fetch(request) {"
    " return new Response('hello-app-a'); } };\n";
const char* kBundleB =
    "export default { async fetch(request) {"
    " return new Response('hello-app-b'); } };\n";
const char* kBundleEcho =
    "export default { async fetch(request) {"
    " const text = await request.text();"
    " return new Response('echo:' + text); } };\n";
const char* kBundleHang =
    "export default { async fetch(request) {"
    " await new Promise(() => {}); } };\n";

// Spawn/load/flush (NOT yet READY) — the executor factory contract. The
// per-test request timeout lets the 504 gate run fast.
WorkerExecutor::WorkerFactory spawn_factory(const std::string& worker_path,
                                            std::uint32_t request_timeout_ms,
                                            const char* bundle) {
    return [worker_path, request_timeout_ms,
            bundle](capsid_worker** out, std::string* factory_error) -> bool {
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = worker_path.c_str();
        config.request_timeout_ms = request_timeout_ms;
        capsid_worker* worker = nullptr;
        if (capsid_worker_spawn(&config, &worker) != CAPSID_OK) {
            *factory_error = "worker spawn failed";
            return false;
        }
        if (capsid_worker_load_bundle(
                worker, reinterpret_cast<const std::uint8_t*>(bundle),
                std::char_traits<char>::length(bundle)) != CAPSID_OK) {
            capsid_worker_destroy(worker);
            *factory_error = "bundle load failed";
            return false;
        }
        if (capsid_worker_flush(worker) != CAPSID_OK) {
            capsid_worker_destroy(worker);
            *factory_error = "bundle flush failed";
            return false;
        }
        *out = worker;
        return true;
    };
}

// The Managed coordinator's warm-up: spawn + load + flush, then consume the
// READY handshake (including the compatibility check) BEFORE adopt.
capsid_worker* warm_worker(const std::string& worker_path,
                           std::uint32_t request_timeout_ms,
                           const char* bundle) {
    std::string error;
    capsid_worker* worker = nullptr;
    require(spawn_factory(worker_path, request_timeout_ms, bundle)(&worker,
                                                                   &error),
            "warm worker spawn failed: " + error);
    capsid::host::WorkerEventSource event_source;
    event_source.set_worker(worker);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    capsid_event event = {};
    event.struct_size = sizeof(event);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            capsid_worker_destroy(worker);
            fail("warm worker flush failed before READY");
        }
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                break;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                const std::string detail(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
                capsid_worker_destroy(worker);
                fail("warm worker error before READY: " +
                     (detail.empty() ? "(empty)" : detail));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                capsid_worker_destroy(worker);
                fail("warm worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            capsid_worker_destroy(worker);
            fail("warm worker event error before READY");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            capsid_worker_destroy(worker);
            fail("warm worker READY timeout");
        }
        event_source.wait(std::min(deadline, std::chrono::steady_clock::now() +
                                                 std::chrono::milliseconds(100)));
    }
    const std::string payload(
        reinterpret_cast<const char*>(event.payload.data), event.payload.size);
    capsid_build_info info;
    capsid_build_info_init(&info);
    require(capsid_runtime_build_info(&info) == CAPSID_OK &&
                info.compatibility_id != nullptr &&
                payload == info.compatibility_id,
            "warm worker compatibility ID mismatch");
    return worker;
}

WorkerRecoveryPolicy test_policy() {
    WorkerRecoveryPolicy policy;
    policy.max_events = 2;
    policy.window_ms = 60000;
    policy.backoff_initial_ms = 20;
    policy.backoff_maximum_ms = 1000;
    policy.jitter_basis_points = 0;
    policy.stable_reset_ms = 60000;
    policy.replacements_concurrent_per_app = 1;
    return policy;
}

GenerationPoolOptions pool_options(const std::string& worker_path,
                                   const std::string& application,
                                   std::uint32_t workers,
                                   std::uint32_t request_timeout_ms,
                                   const char* bundle) {
    GenerationPoolOptions options;
    options.application_id = application;
    options.version = "v1";
    options.generation_digest =
        "sha256:" + std::string(64, application[0]);
    options.workers = workers;
    options.factory =
        spawn_factory(worker_path, request_timeout_ms, bundle);
    options.recovery = test_policy();
    return options;
}

std::vector<capsid_worker*> warm_fleet(const std::string& worker_path,
                                       std::uint32_t count,
                                       std::uint32_t request_timeout_ms,
                                       const char* bundle) {
    std::vector<capsid_worker*> workers;
    workers.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        workers.push_back(
            warm_worker(worker_path, request_timeout_ms, bundle));
    }
    return workers;
}

std::shared_ptr<GenerationPool> make_pool(const std::string& worker_path,
                                          const std::string& application,
                                          const char* bundle,
                                          std::uint32_t request_timeout_ms) {
    std::string error;
    std::shared_ptr<GenerationPool> pool = GenerationPool::create_adopted(
        pool_options(worker_path, application, 1, request_timeout_ms, bundle),
        warm_fleet(worker_path, 1, request_timeout_ms, bundle), &error);
    require(pool != nullptr, "adopt-create failed: " + error);
    return pool;
}

ListenerConfig path_listener(const std::string& name) {
    ListenerConfig config;
    config.name = name;
    config.tcp = "127.0.0.1:0";
    config.public_scheme = "http";
    config.public_authority = "localhost";
    config.routing.mode = "path";
    return config;
}

// One raw HTTP/1.1 exchange over an existing connection; reads until the
// response body matches Content-Length, until the terminal chunk of a
// chunked body (the worker runtime emits no Content-Length — the listener
// serializes chunked), or until the connection closes.
struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_exchange(int fd, const std::string& request) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            send(fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            fail("cannot write the HTTP request");
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string wire;
    char buffer[4096];
    bool done = false;
    bool chunked = false;      // the response used Transfer-Encoding: chunked
    std::string chunked_body;  // its de-framed body
    while (!done) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0) {
            // The connection-ceiling listener closes an excess connection
            // immediately, before reading anything: the client sees an
            // RST with zero bytes received. That is the expected outcome
            // of an over-limit connection, not a response error.
            if (errno == ECONNRESET && wire.empty()) {
                break;
            }
            fail("cannot read the HTTP response");
        }
        if (count == 0) {
            break;  // connection closed: everything received is the body
        }
        wire.append(buffer, static_cast<std::size_t>(count));
        const std::string::size_type head_end = wire.find("\r\n\r\n");
        if (head_end == std::string::npos) {
            continue;
        }
        const std::string head = wire.substr(0, head_end);
        std::string::size_type offset = head.find("Content-Length:");
        if (offset == std::string::npos) {
            offset = head.find("content-length:");
        }
        if (offset != std::string::npos) {
            offset += std::string("content-length:").size();
            while (offset < head.size() && head[offset] == ' ') {
                ++offset;
            }
            const std::string::size_type end = head.find("\r\n", offset);
            const std::size_t length = static_cast<std::size_t>(
                std::strtoull(head.substr(offset, end - offset).c_str(),
                              nullptr, 10));
            if (wire.size() >= head_end + 4 + length) {
                wire = wire.substr(0, head_end + 4 + length);
                done = true;
            }
            continue;
        }
        offset = head.find("Transfer-Encoding:");
        if (offset == std::string::npos) {
            offset = head.find("transfer-encoding:");
        }
        if (offset != std::string::npos &&
            head.find("chunked", offset) != std::string::npos) {
            // Walk chunk-size lines and chunk data until the terminal
            // 0-chunk, de-framing the body as we go. The response is
            // complete only once the terminal "0\r\n" and its closing
            // "\r\n" have arrived.
            chunked = true;
            std::string::size_type pos = head_end + 4;
            chunked_body.clear();
            for (;;) {
                const std::string::size_type line_end =
                    wire.find("\r\n", pos);
                if (line_end == std::string::npos) {
                    break;  // need more bytes
                }
                const std::string size_field =
                    wire.substr(pos, line_end - pos);
                const std::string::size_type extension = size_field.find(';');
                const std::uint64_t size = std::strtoull(
                    size_field.substr(0, extension).c_str(), nullptr, 16);
                if (size == 0) {
                    if (wire.size() < line_end + 4) {
                        break;  // the terminal chunk is not complete yet
                    }
                    done = true;
                    break;
                }
                if (wire.size() < line_end + 2 + size + 2) {
                    break;  // need more bytes
                }
                chunked_body.append(wire, line_end + 2, size);
                pos = line_end + 2 + size + 2;
            }
        }
    }
    HttpResponse response;
    // "HTTP/1.1 200 OK"
    const std::string::size_type code_start = wire.find(' ');
    if (code_start != std::string::npos) {
        response.status =
            std::atoi(wire.c_str() + code_start + 1);
    }
    const std::string::size_type head_end = wire.find("\r\n\r\n");
    if (head_end != std::string::npos) {
        response.body =
            chunked ? chunked_body : wire.substr(head_end + 4);
    }
    return response;
}

int connect_listener(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "cannot create the HTTP socket");
    struct timeval timeout;
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot set the HTTP receive timeout");
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot build the loopback address");
    require(connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to the managed listener");
    return fd;
}

void test_routes_two_apps(const std::string& worker_path) {
    std::shared_ptr<GenerationPool> pool_a =
        make_pool(worker_path, "app-a", kBundleA, 2000);
    std::shared_ptr<GenerationPool> pool_b =
        make_pool(worker_path, "app-b", kBundleB, 2000);
    std::shared_ptr<RoutingTable> routing = std::make_shared<RoutingTable>();
    routing->publish(RoutingSnapshot::build({{"app-a", pool_a},
                                             {"app-b", pool_b}}));

    ManagedListenerOptions options;
    options.config = path_listener("public");
    options.routing = routing;
    ManagedListener listener(options);
    std::string error;
    require(listener.start(&error), "listener start failed: " + error);
    require(listener.bound_port() != 0, "listener bound no port");

    const int fd = connect_listener(listener.bound_port());
    const HttpResponse first = http_exchange(
        fd, "GET /@capsid/app-a/welcome HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n\r\n");
    require(first.status == 200 && first.body == "hello-app-a",
            "path routing did not reach app-a (" +
                std::to_string(first.status) + " '" + first.body + "')");
    // Keep-alive: the second request rides the same connection.
    const HttpResponse second = http_exchange(
        fd, "GET /@capsid/app-b/ HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n\r\n");
    require(second.status == 200 && second.body == "hello-app-b",
            "path routing did not reach app-b (" +
                std::to_string(second.status) + " '" + second.body + "')");
    close(fd);

    // HEAD consumes the worker body without exposing it.
    const int head_fd = connect_listener(listener.bound_port());
    const HttpResponse head = http_exchange(
        head_fd, "HEAD /@capsid/app-a/ HTTP/1.1\r\n"
                 "Host: localhost\r\n"
                 "Connection: close\r\n\r\n");
    require(head.status == 200 && head.body.empty(),
            "HEAD leaked a body (" + std::to_string(head.status) + ")");
    close(head_fd);

    listener.request_stop();
    listener.wait(&error);
    pool_a->stop_and_join();
    pool_b->stop_and_join();
    std::cout << "PASS: listener routes two apps over one path" << std::endl;
}

void test_404_503_504(const std::string& worker_path) {
    std::shared_ptr<GenerationPool> pool_a =
        make_pool(worker_path, "app-a", kBundleA, 2000);
    std::shared_ptr<GenerationPool> pool_hang =
        make_pool(worker_path, "app-hang", kBundleHang, 400);
    std::shared_ptr<RoutingTable> routing = std::make_shared<RoutingTable>();
    routing->publish(RoutingSnapshot::build({{"app-a", pool_a},
                                             {"app-hang", pool_hang}}));

    ManagedListenerOptions options;
    options.config = path_listener("public");
    options.routing = routing;
    ManagedListener listener(options);
    std::string error;
    require(listener.start(&error), "listener start failed: " + error);

    // No /@capsid/ prefix: route-not-found → 404.
    const int fd = connect_listener(listener.bound_port());
    const HttpResponse missing_prefix = http_exchange(
        fd, "GET /welcome HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n\r\n");
    require(missing_prefix.status == 404,
            "missing /@capsid/ prefix did not map to 404 (" +
                std::to_string(missing_prefix.status) + ")");
    close(fd);

    // App not in the snapshot: the router's 503 point.
    const int no_route_fd = connect_listener(listener.bound_port());
    const HttpResponse no_route = http_exchange(
        no_route_fd, "GET /@capsid/app-absent/ HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Connection: close\r\n\r\n");
    require(no_route.status == 503,
            "unrouted App did not map to 503 (" +
                std::to_string(no_route.status) + ")");
    close(no_route_fd);

    // A worker that never answers: the Runtime request timeout → 504.
    const int timeout_fd = connect_listener(listener.bound_port());
    const HttpResponse timeout = http_exchange(
        timeout_fd, "GET /@capsid/app-hang/ HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Connection: close\r\n\r\n");
    require(timeout.status == 504,
            "worker timeout did not map to 504 (" +
                std::to_string(timeout.status) + ")");
    close(timeout_fd);

    listener.request_stop();
    listener.wait(&error);
    pool_a->stop_and_join();
    pool_hang->stop_and_join();
    std::cout << "PASS: 404/503/504 gates" << std::endl;
}

void test_post_echo_and_ceiling(const std::string& worker_path) {
    std::shared_ptr<GenerationPool> pool_echo =
        make_pool(worker_path, "app-echo", kBundleEcho, 2000);
    std::shared_ptr<RoutingTable> routing = std::make_shared<RoutingTable>();
    routing->publish(RoutingSnapshot::build({{"app-echo", pool_echo}}));

    ManagedListenerOptions options;
    options.config = path_listener("public");
    options.config.limits.connections = 1;
    options.routing = routing;
    ManagedListener listener(options);
    std::string error;
    require(listener.start(&error), "listener start failed: " + error);

    // POST body travels through request credit; the worker echoes it. The
    // first request keeps the connection alive (HTTP/1.1 default), so the
    // ceiling below is deterministic: the session is still counted when the
    // second connection arrives.
    const int fd = connect_listener(listener.bound_port());
    const std::string body(64 * 1024, 'x');
    const HttpResponse echo = http_exchange(
        fd, "POST /@capsid/app-echo/ HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" +
            body);
    require(echo.status == 200 && echo.body == "echo:" + body,
            "POST echo mismatch (status " + std::to_string(echo.status) +
                ", body " + std::to_string(echo.body.size()) + " bytes)");

    // Connection ceiling 1: while the first connection is still open, a
    // second connection is closed without service.
    const int ceiling_fd = connect_listener(listener.bound_port());
    const HttpResponse refused = http_exchange(
        ceiling_fd, "GET /@capsid/app-echo/ HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Connection: close\r\n\r\n");
    require(refused.status == 0 && refused.body.empty(),
            "connection ceiling did not close the excess connection");
    close(ceiling_fd);
    close(fd);

    listener.request_stop();
    listener.wait(&error);
    pool_echo->stop_and_join();
    std::cout << "PASS: POST echo + connection ceiling" << std::endl;
}

void test_trusted_header_gate(const std::string& worker_path) {
    std::shared_ptr<GenerationPool> pool_a =
        make_pool(worker_path, "app-a", kBundleA, 2000);
    std::shared_ptr<RoutingTable> routing = std::make_shared<RoutingTable>();
    routing->publish(RoutingSnapshot::build({{"app-a", pool_a}}));

    // Untrusted listener configured for header routing: bind must FAIL —
    // the trust boundary is decided at startup, not request time.
    {
        ManagedListenerOptions options;
        options.config = path_listener("edge");
        options.config.routing.mode = "header";
        options.config.trusted = false;
        options.routing = routing;
        ManagedListener listener(options);
        std::string error;
        require(!listener.start(&error),
                "untrusted header listener started");
        require(!error.empty(), "untrusted header listener gave no error");
    }

    // Trusted listener: header routing resolves the App from Capsid-App.
    {
        ManagedListenerOptions options;
        options.config = path_listener("edge");
        options.config.routing.mode = "header";
        options.config.trusted = true;
        options.routing = routing;
        ManagedListener listener(options);
        std::string error;
        require(listener.start(&error), "trusted listener start failed: " + error);
        const int fd = connect_listener(listener.bound_port());
        const HttpResponse routed = http_exchange(
            fd, "GET /private/ HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Capsid-App: app-a\r\n"
                "Connection: close\r\n\r\n");
        require(routed.status == 200 && routed.body == "hello-app-a",
                "header routing did not reach app-a (" +
                    std::to_string(routed.status) + ")");
        close(fd);
        listener.request_stop();
        listener.wait(&error);
    }

    pool_a->stop_and_join();
    std::cout << "PASS: trusted-header gate" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fail("expected capsid-worker path");
    }
    const std::string worker_path = argv[1];
    test_routes_two_apps(worker_path);
    test_404_503_504(worker_path);
    test_post_echo_and_ceiling(worker_path);
    test_trusted_header_gate(worker_path);
    std::cout << "PASS: Managed listener data plane (WP-05 §9.2)" << std::endl;
    return 0;
}
