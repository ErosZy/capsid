#include "capsid/runtime.h"
#include "egress_test_policy.h"

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <arpa/inet.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <netinet/in.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/socket.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

#include <atomic>
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
        fail("cannot open fixture");
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

class HangingServer {
public:
    HangingServer()
        : listener_(-1),
          client_(-1),
          port_(0),
          accepted_(false),
          closed_(false) {
        listener_ = capsid::win32::create_tcp_socket_fd();
        if (listener_ < 0) {
            fail("cannot create hanging server");
        }
        const int reuse = 1;
        capsid::win32::setsockopt_reuseaddr_fd(listener_);
        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (capsid::win32::bind_fd(
                listener_,
                reinterpret_cast<const struct sockaddr *>(&address),
                sizeof(address)) != 0 ||
            capsid::win32::listen_fd(listener_, 1) != 0) {
            fail("cannot bind hanging server");
        }
        socklen_t size = sizeof(address);
        if (capsid::win32::getsockname_fd(
                listener_,
                reinterpret_cast<struct sockaddr *>(&address),
                &size) != 0) {
            fail("cannot resolve hanging server port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread(&HangingServer::serve, this);
    }

    ~HangingServer() {
        const int client = client_.load();
        if (client >= 0) {
            capsid::win32::shutdown_fd(client);
        }
        if (listener_ >= 0) {
            capsid::win32::shutdown_fd(listener_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (client >= 0) {
            close(client);
        }
        if (listener_ >= 0) {
            close(listener_);
        }
    }

    std::string application_url() const {
        return std::string(
                   "https://example.test/cancel-fetch?"
                   "target=http%3A%2F%2F127.0.0.1%3A") +
               std::to_string(port_) + "%2Fhang";
    }

    bool accepted() const { return accepted_.load(); }
    bool closed() const { return closed_.load(); }

private:
    void serve() {
        capsid_pollfd descriptor = {};
        descriptor.fd = listener_;
        descriptor.events = POLLIN;
        if (capsid::win32::capsid_poll(&descriptor, 1, 5000) <= 0) {
            return;
        }
        const int client = capsid::win32::accept_fd(listener_);
        client_ = client;
        if (client < 0) {
            return;
        }
        accepted_ = true;
        // Blocking recv with a receive timeout: WSAPoll cannot be trusted
        // to report a peer FIN, but a blocking recv returns 0 the moment
        // the close lands.
        capsid::win32::setsockopt_recv_timeout_fd(client, 500);
        char buffer[2048];
        for (;;) {
#if defined(_WIN32)
            // accept_fd returns a CRT fd; Winsock recv takes the raw
            // SOCKET handle.
            const ssize_t count =
                capsid::win32::recv_fd(client, buffer, sizeof(buffer), 0);
#else
            const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
#endif
            if (count == 0) {
                closed_ = true;
                return;
            }
            if (count < 0) {
#if defined(_WIN32)
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
#endif
                return;
            }
        }
    }

    int listener_;
    std::atomic<int> client_;
    uint16_t port_;
    std::atomic<bool> accepted_;
    std::atomic<bool> closed_;
    std::thread thread_;
};

void pump(capsid_worker *worker) {
    const capsid_result flush = capsid_worker_flush(worker);
    if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
        fail("worker flush failed");
    }
    capsid_pollfd descriptor = {};
    descriptor.fd = capsid_worker_fd(worker);
    descriptor.events =
        POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
    capsid::win32::capsid_poll(&descriptor, 1, 10);
}

bool next_event(capsid_worker *worker, capsid_event *event) {
    std::memset(event, 0, sizeof(*event));
    event->struct_size = sizeof(*event);
    const capsid_result result = capsid_worker_next_event(worker, event);
    if (result == CAPSID_OK) {
        return true;
    }
    if (result != CAPSID_WOULD_BLOCK) {
        fail("worker event failed");
    }
    return false;
}

void wait_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_EXIT) {
                fail("worker failed before READY");
            }
        }
    }
    fail("READY timeout");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        fail("expected worker path and fixture path");
    }
    HangingServer server;
    const std::string bundle = read_file(argv[2]);
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.strict_sandbox = 0;
    config.request_timeout_ms = 5000;
    LoopbackEgressPolicy egress_policy;
    egress_policy.attach(&config);

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn");
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load");
    wait_ready(worker);

    const std::string url = server.application_url();
    require_result(
        capsid_worker_begin_request(
            worker, 61, "GET", url.c_str(), NULL, 0),
        "begin fetch cancel");
    require_result(
        capsid_worker_end_request(worker, 61),
        "end fetch cancel");

    const std::chrono::steady_clock::time_point accept_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server.accepted() &&
           std::chrono::steady_clock::now() < accept_deadline) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_EXIT) {
                fail("fetch failed before cancellation");
            }
        }
    }
    if (!server.accepted()) {
        fail("txiki fetch did not connect to hanging target");
    }

    require_result(capsid_worker_cancel(worker, 61), "cancel fetch");
    require_result(capsid_worker_cancel(worker, 61), "repeat fetch cancel");
    const std::chrono::steady_clock::time_point close_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server.closed() &&
           std::chrono::steady_clock::now() < close_deadline) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited while canceling fetch");
            }
        }
    }
    if (!server.closed()) {
        fail("cancel did not close txiki outbound fetch connection");
    }

    require_result(
        capsid_worker_begin_request(
            worker, 61, "GET", "https://example.test/reuse", NULL, 0),
        "reuse fetch-canceled id");
    require_result(
        capsid_worker_end_request(worker, 61),
        "end fetch-canceled reuse");
    bool completed = false;
    const std::chrono::steady_clock::time_point response_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!completed &&
           std::chrono::steady_clock::now() < response_deadline) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                event.request_id == 61) {
                require_result(
                    capsid_worker_grant_response_credit(
                        worker,
                        61,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant reuse credit");
            } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                       event.request_id == 61) {
                completed = true;
            } else if (event.type == CAPSID_EVENT_ERROR ||
                       event.type == CAPSID_EVENT_EXIT) {
                fail("worker failed after fetch cancellation");
            }
        }
    }
    if (!completed) {
        fail("worker was not reusable after fetch cancellation");
    }
    capsid_worker_destroy(worker);
    return 0;
}
