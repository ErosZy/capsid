// v0.1.3 local capsid.json permissions (--capsid-json) on the single-worker
// data plane: the document is its own authority, so an env grant must reach
// the worker, an absent default file must keep the deny-all defaults, and
// every section this path cannot honor (worker.* / request.* / healthCheck,
// env valueFrom, an explicitly requested but missing file) must fail
// startup closed. The fetch egress grant is exercised end-to-end against a
// loopback listener so the egress descriptor path is covered, not just the
// env table.

#include "host/single_worker_server.h"

#include "host/binding_registry.h"
#include "host/generation_identity.h"

#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <arpa/inet.h>
#endif
#if defined(_WIN32)
#else
#include <sys/socket.h>
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#if defined(_WIN32)
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
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

std::string read_ready_line(int fd) {
    std::string line;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (line.find('\n') == std::string::npos) {
        require(std::chrono::steady_clock::now() < deadline,
                "server did not publish READY after start returned");
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = capsid::win32::capsid_poll(&descriptor, 1, 50);
        require(polled >= 0, "cannot poll READY pipe");
        if (polled == 0) {
            continue;
        }
        char bytes[512];
#if defined(_WIN32)
        const ssize_t count =
            capsid::win32::read_fd(fd, bytes, sizeof(bytes));
#else
        const ssize_t count = read(fd, bytes, sizeof(bytes));
#endif
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

// Issues one GET against the started server and returns the response body
// (everything after the header block), matching the lifecycle harness.
std::string get_body(std::uint16_t port, const std::string& path) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create local-policy HTTP socket");
    require(capsid::win32::setsockopt_recv_timeout_fd(fd, 3000) == 0,
            "cannot set local-policy HTTP timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode loopback address");
    require(capsid::win32::connect_fd(
                fd, reinterpret_cast<struct sockaddr*>(&address),
                sizeof(address)) == 0,
            "cannot connect after server start");
    const std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: public.example\r\n"
        "Connection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = capsid::win32::send_fd(
            fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "cannot write local-policy HTTP request");
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char bytes[2048];
    for (;;) {
        const ssize_t count =
            capsid::win32::recv_fd(fd, bytes, sizeof(bytes), 0);
        if (count == 0) {
            break;
        }
        require(count > 0, "cannot read local-policy HTTP response");
        response.append(bytes, static_cast<std::size_t>(count));
    }
    close(fd);
    require(response.find(" 200 ") != std::string::npos,
            "local-policy worker did not answer 200: " + response);
    const std::string::size_type body_begin = response.find("\r\n\r\n");
    if (body_begin == std::string::npos) {
        return "";
    }
    return response.substr(body_begin + 4);
}

void write_json_file(const std::string& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "cannot write capsid.json fixture");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(output), "cannot flush capsid.json fixture");
}

capsid::host::SingleWorkerServerOptions make_options(const char* worker_path,
                                                     int ready_fd) {
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = worker_path;
    options.source_bundle_path = "local-policy-inline";
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

std::shared_ptr<const capsid::host::BindingRegistrySnapshot>
make_echo_binding_registry() {
    auto registry =
        std::make_shared<capsid::host::BindingRegistrySnapshot>();
    capsid::host::BindingPackageSnapshot package;
    package.id = "echo";
    package.manifest_json =
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":[]}})json";
    package.source =
        "export default ({ config }) => ({"
        "  echo(input) { return config.prefix + input; }"
        "});";
    package.manifest_digest = capsid::host::compute_binding_manifest_digest(
        package.manifest_json);
    package.source_digest = capsid::host::sha256_hex(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(package.source.data()),
            package.source.size()));
    registry->packages.push_back(std::move(package));
    return registry;
}

// The data plane routes requests under the /@capsid/{app} prefix; the
// worker branches on the path suffix so one inline bundle covers every
// scenario: /env echoes the granted variable (or the no-var sentinel),
// /fetch proxies the loopback target granted by capsid.json. The loopback
// port is only known at runtime; the fetch scenario splices it into the
// bundle text before spawn, every other scenario leaves the marker out.
// The env snapshot is only readable through the capsid:env module; a plain
// process.env does not exist in the restricted core. Reading an ungranted
// variable throws "environment access denied" instead of yielding
// undefined, so the denial scenario surfaces the throw.
const std::string kEnvWorkerSource =
    "import { env } from 'capsid:env';"
    "export default {"
    "  async fetch(request) {"
    "    const url = new URL(request.url);"
    "    if (url.pathname.endsWith('/fetch')) {"
    "      const response = await fetch('http://127.0.0.1:__CAPSID_LOOPBACK_PORT__/data');"
    "      return new Response(await response.text());"
    "    }"
    "    if (url.pathname.endsWith('/env')) {"
    "      let value = null;"
    "      try { value = env.get('CAPSID_TEST_VAR'); }"
    "      catch (error) { value = 'denied'; }"
    "      return new Response(value ?? 'no-var');"
    "    }"
    "    return new Response('unexpected-path');"
    "  }"
    "};";

// A worker with no module imports: the only one that can run under the
// absent-default deny-all baseline (any import would be unauthorized).
const std::string kPlainWorkerSource =
    "export default { fetch: () => new Response('plain-ok') };";

// Binds a loopback listener on a kernel-assigned port, then answers the
// first connection with a fixed body. One-shot: the accept happens on the
// given thread and the listener is closed by it.
struct LoopbackFixture {
    std::uint16_t port = 0;
    std::thread thread;
};

LoopbackFixture start_loopback_fixture(const std::string& body) {
    LoopbackFixture fixture;
    const int listener = capsid::win32::create_tcp_socket_fd();
    require(listener >= 0, "cannot create loopback fixture socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode loopback fixture address");
    require(capsid::win32::bind_fd(listener,
                reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) ==
                0,
            "cannot bind loopback fixture");
    require(capsid::win32::listen_fd(listener, 1) == 0,
            "cannot listen on loopback fixture");
    socklen_t length = sizeof(address);
    require(capsid::win32::getsockname_fd(listener,
                        reinterpret_cast<struct sockaddr*>(&address), &length) ==
                0,
            "cannot inspect loopback fixture port");
    fixture.port = ntohs(address.sin_port);
    fixture.thread = std::thread([listener, body]() {
        const int client = capsid::win32::accept_fd(listener);
        if (client < 0) {
            close(listener);
            return;
        }
        char bytes[1024];
        const ssize_t count = capsid::win32::recv_fd(
            client, bytes, sizeof(bytes), 0);
        if (count > 0) {
            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: " +
                std::to_string(body.size()) +
                "\r\n"
                "Connection: close\r\n\r\n" +
                body;
            std::size_t sent = 0;
            while (sent < response.size()) {
                const ssize_t written = capsid::win32::send_fd(
                    client, response.data() + sent,
                    response.size() - sent, 0);
                if (written <= 0) {
                    break;
                }
                sent += static_cast<std::size_t>(written);
            }
        }
        close(client);
        close(listener);
    });
    return fixture;
}

// Success scenario: writes the local document (when non-empty), starts the
// server with it, issues one request and expects the body fragment.
// loopback_port != 0 splices the /fetch target into the env worker source.
void run_success_scenario(const char* worker_path,
                          const std::string& name,
                          const std::string& capsid_json,
                          const std::string& worker_source,
                          std::uint16_t loopback_port,
                          const std::string& request_path,
                          const std::string& expected_body,
                          std::shared_ptr<const
                              capsid::host::BindingRegistrySnapshot>
                              binding_registry = {}) {
    const std::string json_path = "capsid-test-" + name + ".json";
    if (!capsid_json.empty()) {
        write_json_file(json_path, capsid_json);
    }
    std::string source = worker_source;
    if (loopback_port != 0) {
        const std::string marker = "__CAPSID_LOOPBACK_PORT__";
        const std::string::size_type marker_at = source.find(marker);
        require(marker_at != std::string::npos,
                "fetch scenario lost its port marker");
        source.replace(marker_at, marker.size(),
                       std::to_string(loopback_port));
    }
    const std::vector<std::uint8_t> bundle(source.begin(), source.end());

    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.capsid_json_path = json_path;
    options.binding_registry = std::move(binding_registry);
    capsid::host::SingleWorkerServer server(options);
    std::string error;
    require(server.start(bundle, &error), name + ": start failed: " + error);
    close(ready[1]);
    const std::uint16_t port = ready_port(read_ready_line(ready[0]));
    close(ready[0]);
    const std::string body = get_body(port, request_path);
    require(body.find(expected_body) != std::string::npos,
            name + ": expected \"" + expected_body + "\" in \"" + body + "\"");
    server.request_stop();
    require(server.wait(&error), name + ": wait failed: " + error);
    if (!capsid_json.empty()) {
        remove(json_path.c_str());
    }
    std::cout << "ok " << name << std::endl;
}

// Failure scenario: startup must fail closed with the expected error
// fragment. An empty capsid_json means the referenced file does not exist
// (explicit --capsid-json semantics via capsid_json_required below).
void run_failure_scenario(const char* worker_path,
                          const std::string& name,
                          const std::string& capsid_json,
                          bool required,
                          const std::string& expected_error,
                          std::shared_ptr<const
                              capsid::host::BindingRegistrySnapshot>
                              binding_registry = {}) {
    const std::string json_path = "capsid-test-" + name + ".json";
    if (!capsid_json.empty()) {
        write_json_file(json_path, capsid_json);
    }
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create READY pipe");
    const std::string source =
        "export default { fetch: () => new Response('unused') };";
    const std::vector<std::uint8_t> bundle(source.begin(), source.end());

    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.capsid_json_path = json_path;
    options.capsid_json_required = required;
    options.binding_registry = std::move(binding_registry);
    capsid::host::SingleWorkerServer server(options);
    std::string error;
    require(!server.start(bundle, &error),
            name + ": startup did not fail closed");
    require(error.find(expected_error) != std::string::npos,
            name + ": error \"" + error + "\" misses \"" + expected_error + "\"");
    server.request_stop();
    require(server.wait(&error), name + ": wait failed: " + error);
    close(ready[0]);
    close(ready[1]);
    if (!capsid_json.empty()) {
        remove(json_path.c_str());
    }
    std::cout << "ok " << name << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected capsid-worker path");

    const std::string pool = "\"pool\":{\"minReady\":1,\"maxWorkers\":1}";

    // 1. An env grant in the local document reaches the worker: the module
    //    and the exact env name are both authorized through the permissive
    //    host mirror, and the literal value arrives through capsid:env.
    run_success_scenario(
        argv[1], "env-grant",
        "{\"apiVersion\":\"capsid/app-v1\","
        "\"permissions\":{\"modules\":[\"capsid:env\"],"
        "\"env\":{\"CAPSID_TEST_VAR\":{\"value\":\"granted-value\"}}},"
        + pool + "}",
        kEnvWorkerSource, 0, "/@capsid/orders/env", "granted-value");

    // 2. A document that grants no env entries keeps the deny-all default:
    //    the module is usable, but reading the variable is denied (the
    //    runtime throws "environment access denied").
    run_success_scenario(
        argv[1], "env-denied",
        "{\"apiVersion\":\"capsid/app-v1\","
        "\"permissions\":{\"modules\":[\"capsid:env\"]},"
        + pool + "}",
        kEnvWorkerSource, 0, "/@capsid/orders/env", "denied");

    // 3. An absent default file (no capsid.json next to the server) is the
    //    pre-v0.1.3 no-policy case: startup succeeds and the server serves
    //    (a plain worker without module imports runs; any import would be
    //    unauthorized under the deny-all baseline).
    run_success_scenario(argv[1], "absent-default-noop", "",
                         kPlainWorkerSource, 0, "/@capsid/orders/env",
                         "plain-ok");

    // 4. A fetch egress grant is honored end-to-end against a loopback
    //    listener (permissive host mirror -> egress descriptors -> worker).
    {
        LoopbackFixture fixture = start_loopback_fixture("fetch-ok");
        const std::string capsid_json =
            "{\"apiVersion\":\"capsid/app-v1\","
            "\"permissions\":{\"modules\":[\"capsid:env\"],"
            "\"fetch\":{\"allow\":[\"127.0.0.1:" +
            std::to_string(fixture.port) + "\"]}},"
            + pool + "}";
        run_success_scenario(argv[1], "fetch-egress-grant", capsid_json,
                             kEnvWorkerSource, fixture.port,
                             "/@capsid/orders/fetch", "fetch-ok");
        fixture.thread.join();
    }

    // 5. worker.* is CLI-owned on this path and must fail loudly instead of
    //    silently skipping the capacity the operator asked for.
    run_failure_scenario(
        argv[1], "worker-section-rejected",
        "{\"apiVersion\":\"capsid/app-v1\","
        "\"permissions\":{},"
        "\"worker\":{\"memoryMax\":\"64MiB\"}," + pool + "}",
        false, "not applicable in local mode");

    // 6. env valueFrom has no secret store on this path: reject, never
    //    degrade to an empty value.
    run_failure_scenario(
        argv[1], "value-from-rejected",
        "{\"apiVersion\":\"capsid/app-v1\","
        "\"permissions\":{\"modules\":[\"capsid:env\"],"
        "\"env\":{\"CAPSID_TEST_VAR\":{\"valueFrom\":\"capsid-test-secret\"}}},"
        + pool + "}",
        false, "valueFrom is unavailable in local mode");

    // 7. An explicit --capsid-json that is missing is an operator error.
    run_failure_scenario(argv[1], "explicit-missing-required", "", true,
                         "cannot find");

    // 8. The frozen schema still runs first: an unknown field fails closed.
    run_failure_scenario(
        argv[1], "unknown-field-rejected",
        "{\"apiVersion\":\"capsid/app-v1\",\"permissions\":{},"
        "\"notAField\":true," + pool + "}",
        false, "rejected at");

    // 9. `entry` is honored locally (v0.2.x) when no explicit source is
    //    given; with an explicit bundle the CLI wins over the document
    //    (entry-derivation itself is covered by the host_single_worker
    //    integration fixture, which runs without --source-bundle).
    run_success_scenario(
        argv[1], "entry-overridden-by-explicit-bundle",
        "{\"apiVersion\":\"capsid/app-v1\",\"entry\":\"bundle.mjs\","
        "\"permissions\":{}," + pool + "}",
        kPlainWorkerSource, 0, "/@capsid/orders/env", "plain-ok");

    // 10. pool.queue* arms the bounded admission queue locally (v0.2.x);
    //     document presence decides, 0 means queueing disabled.  The
    //     admission behavior itself is covered by the host_single_worker
    //     integration fixture.
    run_success_scenario(
        argv[1], "pool-queue-armed",
        "{\"apiVersion\":\"capsid/app-v1\",\"permissions\":{},"
        "\"pool\":{\"minReady\":1,\"maxWorkers\":1,"
        "\"queueRequests\":8}}",
        kPlainWorkerSource, 0, "/@capsid/orders/env", "plain-ok");

    // 11. Binding development uses the same app-v2 declaration and
    //     capsid:binding/<id> facade as managed mode. The explicit Host
    //     Registry supplies implementation bytes; the App only sees the
    //     declared API and receives an async result.
    const auto echo_registry = make_echo_binding_registry();
    run_success_scenario(
        argv[1], "binding-call",
        R"json({"apiVersion":"capsid/app-v2","permissions":{},"bindings":{"echo":{"config":{"prefix":"binding:"}}},"pool":{"minReady":1,"maxWorkers":1}})json",
        "import echo from 'capsid:binding/echo';"
        "export default { async fetch() {"
        "  return new Response(await echo.echo('ok'));"
        "} };",
        0, "/@capsid/orders/binding", "binding:ok", echo_registry);

    // 12. A Binding declaration without a Host Registry cannot fall back
    //     to user-controlled code or a global search path.
    run_failure_scenario(
        argv[1], "binding-registry-required",
        R"json({"apiVersion":"capsid/app-v2","permissions":{},"bindings":{"echo":{"config":{"prefix":"binding:"}}},"pool":{"minReady":1,"maxWorkers":1}})json",
        false, "bindings require an explicit Host Registry");

    // 13. Local modes intentionally have no secret provider. A Binding
    //     secret reference is rejected instead of becoming empty/plaintext.
    run_failure_scenario(
        argv[1], "binding-secret-rejected",
        R"json({"apiVersion":"capsid/app-v2","permissions":{},"bindings":{"echo":{"secrets":{"password":{"valueFrom":"echo-password"}}}},"pool":{"minReady":1,"maxWorkers":1}})json",
        false, "binding secrets are unavailable in local mode",
        echo_registry);

    std::cout << "PASS" << std::endl;
    return 0;
}
