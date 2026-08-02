#include "capsid/runtime.h"
#include "egress_test_policy.h"
#include "wpt_report.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open fixture: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool contains(const std::string &text, const char *fragment) {
    return text.find(fragment) != std::string::npos;
}

class LocalHttpServer {
public:
    LocalHttpServer() : fd_(-1), port_(0), served_(false) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            fail("cannot create local HTTP server socket");
        }
        const int reuse = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(fd_,
                 reinterpret_cast<const struct sockaddr *>(&address),
                 sizeof(address)) != 0) {
            fail(std::string("cannot bind local HTTP server: ") +
                 std::strerror(errno));
        }
        if (listen(fd_, 1) != 0) {
            fail(std::string("cannot listen on local HTTP server: ") +
                 std::strerror(errno));
        }

        socklen_t address_size = sizeof(address);
        if (getsockname(fd_,
                        reinterpret_cast<struct sockaddr *>(&address),
                        &address_size) != 0) {
            fail("cannot resolve local HTTP server port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread(&LocalHttpServer::serve, this);
    }

    ~LocalHttpServer() {
        if (thread_.joinable()) {
            shutdown(fd_, SHUT_RDWR);
            thread_.join();
        }
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    std::string application_url() const {
        return std::string(
                   "https://example.test/fetch?"
                   "target=http%3A%2F%2F127.0.0.1%3A") +
               std::to_string(port_) + "%2Fdata";
    }

    void wait() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (!served_) {
            fail("local HTTP server did not complete a request");
        }
    }

private:
    void serve() {
        struct pollfd descriptor = {};
        descriptor.fd = fd_;
        descriptor.events = POLLIN;
        if (poll(&descriptor, 1, 15000) <= 0) {
            return;
        }

        const int client = accept(fd_, NULL, NULL);
        if (client < 0) {
            return;
        }
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        setsockopt(client,
                   SOL_SOCKET,
                   SO_NOSIGPIPE,
                   &no_sigpipe,
                   sizeof(no_sigpipe));
#endif

        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64u * 1024u) {
            const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                close(client);
                return;
            }
            request.append(buffer, static_cast<size_t>(count));
        }

        static const char response[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "X-Capsid-Upstream: direct-egress\r\n"
            "Content-Length: 15\r\n"
            "Connection: close\r\n"
            "\r\n"
            "capsid-fetch-ok";
        size_t offset = 0;
        while (offset < sizeof(response) - 1) {
#ifdef MSG_NOSIGNAL
            const ssize_t count = send(
                client,
                response + offset,
                sizeof(response) - 1 - offset,
                MSG_NOSIGNAL);
#else
            const ssize_t count =
                send(client, response + offset, sizeof(response) - 1 - offset, 0);
#endif
            if (count <= 0) {
                close(client);
                return;
            }
            offset += static_cast<size_t>(count);
        }
        served_ = true;
        close(client);
    }

    int fd_;
    uint16_t port_;
    std::atomic<bool> served_;
    std::thread thread_;
};

void wait_for_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup flush: ") + capsid_result_string(flush));
        }

        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker startup error: ") +
                     std::string(reinterpret_cast<const char *>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for READY");
        }

        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
    }
}

// Frozen RED (bodyless request-end fail-closed): the fused path calls the
// request_end bridge synchronously inside begin. A throwing bridge must
// propagate exactly like the standalone request-end frame path — the worker
// must surface an error event; swallowing the exception leaves the request
// hanging with no error (the pre-fix fused path returned the begin result
// and dropped the end-bridge failure).
void bodyless_request_end_failure_fails_closed(capsid_worker *worker) {
    require_result(
        capsid_worker_begin_bodyless_request(
            worker, 1, "GET", "https://example.test/bodyless", NULL, 0),
        "begin bodyless request");
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("bodyless flush: ") +
                 capsid_result_string(flush));
        }

        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_ERROR) {
                return;  // fail closed: the bridge failure surfaced
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited without reporting the "
                     "request-end failure");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("bodyless event: ") +
                 capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("request-end bridge failure was swallowed: "
                 "no error event surfaced");
        }

        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events =
            POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
    }
}

std::string run_request(capsid_worker *worker, const char *url) {
    require_result(
        capsid_worker_begin_request(worker, 1, "GET", url, NULL, 0),
        "begin request");
    require_result(capsid_worker_end_request(worker, 1), "end request");

    bool received_head = false;
    std::string body;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("request flush: ") + capsid_result_string(flush));
        }

        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                if (event.request_id != 1 || event.status != 200) {
                    fail("unexpected response head");
                }
                received_head = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                if (!received_head || event.request_id != 1) {
                    fail("response body arrived before its head");
                }
                body.append(reinterpret_cast<const char *>(event.payload.data),
                            event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, event.request_id, static_cast<uint32_t>(event.payload.size)),
                    "replenish response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                if (!received_head || event.request_id != 1) {
                    fail("unexpected response end");
                }
                return body;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker request error: ") +
                     std::string(reinterpret_cast<const char *>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("request event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for response");
        }

        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fail("expected worker path, JavaScript fixture path, optional mode and source name");
    }
    const std::string mode = argc >= 4 ? argv[3] : "surface";
    LocalHttpServer *server = mode == "fetch" ? new LocalHttpServer() : NULL;
    const std::string request_url =
        server ? server->application_url() :
                 "https://example.test/integration";
    const std::string bundle = read_file(argv[2]);

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];

    // Frozen RED: the app layer cannot make the fused request-end bridge
    // fail (tjs:internal/* is capability-forbidden for apps and the
    // bootstrap requestEnd early-returns for bodyless requests), so the
    // failure is injected through the host-provided capability-policy
    // environment snapshot. The test asserts the fused begin propagates it
    // (fail closed) — reverting the propagation in the fused path must fail
    // this test.
    capsid_env_entry fail_end_entry;
    capsid_permission_rule fail_end_rule;
    capsid_capability_policy fail_end_policy;
    const char *const fail_end_modules[] = { "capsid:env" };
    if (mode == "bodyless-end-failure") {
        capsid_env_entry_init(&fail_end_entry);
        fail_end_entry.name = "CAPSID_TEST_FAIL_REQUEST_END_BRIDGE";
        fail_end_entry.value = "1";

        capsid_permission_rule_init(&fail_end_rule);
        fail_end_rule.permission = CAPSID_PERMISSION_ENV;
        fail_end_rule.action = CAPSID_PERMISSION_ALLOW;
        fail_end_rule.resource = "CAPSID_TEST_FAIL_REQUEST_END_BRIDGE";
        fail_end_rule.rule_id = 1;

        capsid_capability_policy_init(&fail_end_policy);
        fail_end_policy.allowed_modules = fail_end_modules;
        fail_end_policy.allowed_module_count = 1;
        fail_end_policy.rules = &fail_end_rule;
        fail_end_policy.rule_count = 1;
        fail_end_policy.env_entries = &fail_end_entry;
        fail_end_policy.env_entry_count = 1;
        config.capability_policy = &fail_end_policy;
    }
    LoopbackEgressPolicy egress_policy;
    if (server) {
        egress_policy.attach(&config);
    }

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    const capsid_result load_result =
        argc == 5
            ? capsid_worker_load_bundle_named(
                  worker,
                  reinterpret_cast<const uint8_t *>(bundle.data()),
                  bundle.size(),
                  argv[4])
            : capsid_worker_load_bundle(
                  worker,
                  reinterpret_cast<const uint8_t *>(bundle.data()),
                  bundle.size());
    require_result(load_result, "load bundle");

    wait_for_ready(worker);

    if (mode == "bodyless-end-failure") {
        bodyless_request_end_failure_fails_closed(worker);
        capsid_worker_destroy(worker);
        return 0;
    }

    const std::string body = run_request(worker, request_url.c_str());

    if (mode == "wasm") {
        if (!contains(body, "\"profile\":\"CAPSID-MIN-2025-subset-v0\"") ||
            !contains(body, "\"passed\":true") ||
            !contains(body, "\"failures\":[]")) {
            fail(std::string("unexpected WebAssembly report: ") + body);
        }
    } else if (mode == "platform") {
        if (!contains(body, "\"profile\":\"CAPSID-MIN-2025-subset-v0\"") ||
            !contains(body, "\"passed\":true") ||
            !contains(body, "\"failures\":[]")) {
            fail(std::string("unexpected platform contract report: ") + body);
        }
    } else if (mode == "wpt") {
        if (!contains(body, "\"profile\":\"CAPSID-MIN-2025-subset-v0\"") ||
            !contains(body, "\"source\":\"web-platform-tests/wpt\"") ||
            !contains(body, "\"passed\":true") ||
            !contains(body, "\"failures\":[]")) {
            fail(std::string("unexpected WPT report: ") + body);
        }
        /*
         * Independently reject a realm that executed no subtests. app.js also
         * guards this via `ranNothing`, but that guard lives in the artifact
         * under test; asserting it here as well means a regression in the
         * fixture cannot turn an empty run into a green test.
         */
        std::string report_error;
        if (!capsid_test::validate_wpt_nonzero_report(body, &report_error)) {
            fail(std::string("invalid WPT execution evidence: ") +
                 report_error + ": " + body);
        }
    } else if (mode == "fetch") {
        if (!contains(body, "\"passed\":true") ||
            !contains(body, "\"status\":200") ||
            !contains(body, "\"upstreamHeader\":\"direct-egress\"") ||
            !contains(body, "\"body\":\"capsid-fetch-ok\"")) {
            fail(std::string("unexpected direct fetch report: ") + body);
        }
    } else if (mode == "surface") {
        if (!contains(body, "\"profile\":\"CAPSID-MIN-2025-subset-v0\"") ||
            !contains(body, "\"matches\":true") ||
            !contains(body, "\"missing\":[]") ||
            !contains(body, "\"unexpected\":[]") ||
            !contains(body, "\"userAgent\":\"capsid-runtime\"") ||
            !contains(body, "\"fetch\":\"function\"") ||
            !contains(body, "\"Request\":\"function\"") ||
            !contains(body, "\"Response\":\"function\"") ||
            !contains(body, "\"ReadableStream\":\"function\"") ||
            !contains(body, "\"crypto\":\"object\"") ||
            !contains(body, "\"Crypto\":\"function\"") ||
            !contains(body, "\"CryptoKey\":\"function\"") ||
            !contains(body, "\"SubtleCrypto\":\"function\"") ||
            !contains(body, "\"performance\":\"object\"") ||
            !contains(body, "\"Performance\":\"function\"") ||
            !contains(body, "\"reportError\":\"function\"") ||
            !contains(body, "\"WebAssembly\":\"object\"")) {
            fail(std::string("unexpected global surface report: ") + body);
        }
    } else {
        fail(std::string("unknown integration test mode: ") + mode);
    }

    if (server) {
        server->wait();
        delete server;
    }
    capsid_worker_destroy(worker);
    return 0;
}
