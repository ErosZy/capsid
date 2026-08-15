// M2 E-1 admission control RED/GREEN (design §10.3): the five-level gate
// chain in its v1 fixed-pool form —
//   ① listener/header gate;
//   ② Host-global inflight/queue gate — merged with ③ in the v1 single-App
//     pool: admission is enforced per shard, and the pool's Host budget is
//     the shard budget times the shard count (recorded in
//     static_pool_server.cc);
//   ③ App inflight/queue gate — the bounded per-shard queue
//     (queueRequests / queueHeaderBytes / queueTimeout);
//   ④ shard pool capacity — no READY worker maps to 503;
//   ⑤ worker max-inflight hard boundary — enforced by the worker itself,
//     the shard admission bounds what reaches it.
//
// Frozen assertions: App queue full → 429; queue deadline expiry → 504;
// worker dead / exited before the response head → 503 (not 502); the pool
// forwards its admission options into every shard.

#if __has_include("host/single_worker_server.h")
#include "host/single_worker_server.h"
#define CAPSID_HAS_SINGLE_WORKER_SERVER 1
#else
#define CAPSID_HAS_SINGLE_WORKER_SERVER 0
#endif

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
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <dirent.h>
#endif
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
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
        require(read(fd, &byte, 1) == 1,
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

// The slow fixture: every request parks in the worker for 700 ms, so a test
// can hold one inflight slot deterministically.
const std::vector<std::uint8_t>& slow_bundle() {
    static const std::string source =
        "export default { fetch: async () => { "
        "  await new Promise((resolve) => setTimeout(resolve, 700)); "
        "  return new Response('admission-ok'); } };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

// The fast fixture: immediate response, no worker-side delay.
const std::vector<std::uint8_t>& fast_bundle() {
    static const std::string source =
        "export default { fetch: () => new Response('admission-ok') };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

int connect_to(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "cannot create admission HTTP socket");
    struct timeval timeout = {};
    timeout.tv_sec = 3;
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot set admission HTTP receive timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode admission loopback address");
    require(connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to admission server");
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
            send(fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "cannot write admission HTTP request");
        sent += static_cast<std::size_t>(count);
    }
}

struct RawHttpResponse {
    unsigned status = 0;
    std::string body;
};

// Reads exactly one line ("\r\n"-terminated) from fd, refilling `buffer`
// as needed. The line is appended to `buffer` (so the caller keeps a
// consistent stream view) and returned without the terminator.
std::string read_line(int fd, std::string& buffer, char* scratch,
                      const std::size_t scratch_size,
                      const std::chrono::steady_clock::time_point& deadline) {
    std::string::size_type line_end = buffer.find("\r\n");
    while (line_end == std::string::npos) {
        require(std::chrono::steady_clock::now() < deadline,
                "admission response line timed out");
        const ssize_t count = recv(fd, scratch, scratch_size, 0);
        if (count == 0) {
            fail("admission response hit EOF mid-line");
        }
        require(count > 0, "admission response recv failed");
        buffer.append(scratch, static_cast<std::size_t>(count));
        line_end = buffer.find("\r\n");
    }
    const std::string line = buffer.substr(0, line_end);
    buffer.erase(0, line_end + 2);
    return line;
}

void require_bytes(int fd, std::string& buffer, std::size_t count,
                   char* scratch, const std::size_t scratch_size,
                   const std::chrono::steady_clock::time_point& deadline) {
    while (buffer.size() < count) {
        require(std::chrono::steady_clock::now() < deadline,
                "admission response body timed out");
        const ssize_t got = recv(fd, scratch, scratch_size, 0);
        require(got > 0, "admission response body recv failed");
        buffer.append(scratch, static_cast<std::size_t>(got));
    }
}

// Reads one complete HTTP/1.1 response: status line and header block, then
// the body by Content-Length or chunked transfer decoding (the worker's
// normal responses are chunked). The connection stays open when keep_alive
// was used.
RawHttpResponse read_response(int fd) {
    char scratch[2048];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string buffer;
    const std::string status_line = read_line(fd, buffer, scratch,
                                              sizeof(scratch), deadline);
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
                // Consume the trailer block up to its terminating blank
                // line, so a keep-alive connection starts the next response
                // on a clean stream.
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
            // The chunk terminator "\r\n" follows the data.
            const std::string terminator =
                read_line(fd, buffer, scratch, sizeof(scratch), deadline);
            require(terminator.empty(),
                    "chunked response had a non-empty chunk terminator");
        }
    } else {
        require(saw_content_length,
                "admission response has neither Content-Length nor chunked");
        require_bytes(fd, buffer, body_length, scratch, sizeof(scratch),
                      deadline);
        response.body = buffer.substr(0, body_length);
    }
    return response;
}

// One-shot GET on a fresh connection.
RawHttpResponse http_get(std::uint16_t port, const std::string& target) {
    const int fd = connect_to(port);
    send_request(fd, target, false);
    const RawHttpResponse response = read_response(fd);
    close(fd);
    return response;
}

// The server spawns the worker as OUR direct child; find that child by
// scanning /proc for processes whose parent is this test process, and kill
// it. The kill is the worker-fault injection for the ④ → 503 path.
pid_t find_worker_child_pid() {
    const pid_t self = getpid();
    DIR* directory = opendir("/proc");
    require(directory != nullptr, "cannot open /proc to find the worker");
    pid_t found = -1;
    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        char path[320];  // NAME_MAX (255) + "/proc/" + "/stat" + NUL
        std::snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            continue;
        }
        char comm[256];
        long ppid = -1;
        // Format: pid (comm) state ppid ... — the scanned conversions are
        // comm and ppid; %*d is suppressed, and the field separators
        // between the comm's ')' and ppid are ')', a space, the state
        // character and a space, consumed one at a time (the %[^)] scan
        // absorbs the comm and its leading '(').
        const int scanned = std::fscanf(
            file, "%*d %255[^)]%*c %*c %ld", comm, &ppid);
        std::fclose(file);
        if (scanned == 2 && ppid == static_cast<long>(self)) {
            found = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
            break;
        }
    }
    closedir(directory);
    return found;
}

void kill_worker_child() {
    const pid_t worker_pid = find_worker_child_pid();
    require(worker_pid > 0, "cannot find the live worker child process");
    require(kill(worker_pid, SIGKILL) == 0, "cannot SIGKILL the worker");
}

// After the worker exits, the shard closes its acceptor: a refused new
// connection is the deterministic "worker-exit processed" signal.
void require_acceptor_closed(std::uint16_t port) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        require(std::chrono::steady_clock::now() < deadline,
                "acceptor never closed after the worker was killed");
        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
                "cannot encode admission loopback address");
        const int connected =
            connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address));
        close(fd);
        if (connected != 0) {
            return;  // refused: the shard stopped accepting
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

capsid::host::SingleWorkerServerOptions make_worker_options(
    const char* worker_path, int ready_fd) {
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = worker_path;
    options.source_bundle_path = "admission-inline";
    options.source_name = "file://admission/v1/bundle.mjs";
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

// ③ gate: inflight-full with queueing disabled rejects directly with 429
// (App quota full; §10.3). ⑤ boundary: the worker itself would reject an
// over-commit, but the shard admission never lets one through.
void test_inflight_full_rejects(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create admission READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_worker_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 1;  // one in-flight slot
    options.max_streaming_inflight_per_worker = 1;  // E-2 1/1 boundary
    options.queue_requests = 0;           // queueing disabled
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(slow_bundle(), &error),
            "cannot start admission server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    // Hold the single inflight slot.
    const int holder = connect_to(port);
    send_request(holder, "/@capsid/orders/hold", true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second request: inflight full and no queue → 429.
    const RawHttpResponse rejected = http_get(port, "/@capsid/orders/second");
    require(rejected.status == 429,
            "inflight-full with queueing disabled must reject with 429, got " +
                std::to_string(rejected.status));

    // The holder still completes normally.
    const RawHttpResponse held = read_response(holder);
    require(held.status == 200, "held request did not complete with 200");
    close(holder);

    server.request_stop();
    require(server.wait(&error), "admission server wait failed: " + error);
}

// ③ gate: the bounded queue accepts up to queueRequests; the request beyond
// the queue depth gets 429.
void test_queue_full_rejects(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create admission READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_worker_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 1;
    options.max_streaming_inflight_per_worker = 1;  // E-2 1/1 boundary
    options.queue_requests = 1;
    options.queue_timeout_ms = 3000;  // long enough to never fire
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(slow_bundle(), &error),
            "cannot start admission server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int holder = connect_to(port);
    send_request(holder, "/@capsid/orders/hold", true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // One slot in the queue: accepted.
    const int queued = connect_to(port);
    send_request(queued, "/@capsid/orders/queued", true);

    // Queue is now full → 429.
    const RawHttpResponse rejected = http_get(port, "/@capsid/orders/third");
    require(rejected.status == 429,
            "queue-full must reject with 429, got " +
                std::to_string(rejected.status));

    // The held request completes, the queued request is served next.
    const RawHttpResponse held = read_response(holder);
    require(held.status == 200, "held request did not complete with 200");
    const RawHttpResponse served = read_response(queued);
    require(served.status == 200,
            "queued request was not served after the slot freed");
    close(holder);
    close(queued);

    server.request_stop();
    require(server.wait(&error), "admission server wait failed: " + error);
}

// ③ gate: a queued request whose queue deadline expires gets 504 (§10.3
// "queue 或 Host deadline 到期 → 504").
void test_queue_timeout_returns_504(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create admission READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_worker_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 1;
    options.max_streaming_inflight_per_worker = 1;  // E-2 1/1 boundary
    options.queue_requests = 2;
    options.queue_timeout_ms = 200;  // expires while the holder parks
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(slow_bundle(), &error),
            "cannot start admission server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int holder = connect_to(port);
    send_request(holder, "/@capsid/orders/hold", true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The second request enters the queue and times out after 200 ms.
    const int queued = connect_to(port);
    send_request(queued, "/@capsid/orders/queued", true);
    const RawHttpResponse timed_out = read_response(queued);
    require(timed_out.status == 504,
            "queue deadline expiry must return 504, got " +
                std::to_string(timed_out.status));

    const RawHttpResponse held = read_response(holder);
    require(held.status == 200, "held request did not complete with 200");
    close(holder);
    close(queued);

    server.request_stop();
    require(server.wait(&error), "admission server wait failed: " + error);
}

// ④ gate: after the worker is killed, an established connection gets 503
// (not 502) on its next request once the exit has been processed.
void test_worker_death_returns_503(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create admission READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_worker_options(worker_path, ready[1]);
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(fast_bundle(), &error),
            "cannot start admission server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    // The worker answers one request, then the test kills it.
    const int connection = connect_to(port);
    send_request(connection, "/@capsid/orders/alive", true);
    const RawHttpResponse alive = read_response(connection);
    require(alive.status == 200, "worker did not answer before the kill");
    kill_worker_child();

    // The worker exit closes the acceptor; wait for that deterministic
    // signal before asserting the 503 on the already-established connection.
    require_acceptor_closed(port);
    send_request(connection, "/@capsid/orders/after-death", true);
    const RawHttpResponse after = read_response(connection);
    require(after.status == 503,
            "request after worker death must return 503, got " +
                std::to_string(after.status));
    close(connection);

    server.request_stop();
    require(server.wait(&error), "admission server wait failed: " + error);
}

#endif  // CAPSID_HAS_SINGLE_WORKER_SERVER

#if CAPSID_HAS_STATIC_POOL_SERVER

// The pool forwards its admission options into every shard: the shard's
// queue depth comes from StaticPoolServerOptions, not from per-shard code.
void test_pool_forwards_admission(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create admission READY pipe");
    capsid::host::StaticPoolServerOptions options;
    options.workers = 1;
    options.max_inflight_per_worker = 1;
    options.max_streaming_inflight_per_worker = 1;  // E-2 1/1 boundary
    options.queue_requests = 1;
    options.queue_timeout_ms = 3000;
    options.worker_options.worker_path = worker_path;
    options.worker_options.source_bundle_path = "admission-pool-inline";
    options.worker_options.source_name = "file://admission/pool/v1/bundle.mjs";
    options.worker_options.application = "orders";
    options.worker_options.listen_address = "127.0.0.1";
    options.worker_options.listen_port = 0;
    options.worker_options.public_scheme = "http";
    options.worker_options.public_authority = "public.example";
    options.worker_options.request_timeout_ms = 5000;
    options.worker_options.initial_stream_window = 64U * 1024U;
    options.worker_options.strict_sandbox = false;
    options.worker_options.ready_fd = ready[1];
    capsid::host::StaticPoolServer pool(std::move(options));
    std::string error;
    require(pool.start(slow_bundle(), &error),
            "cannot start admission pool: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int holder = connect_to(port);
    send_request(holder, "/@capsid/orders/hold", true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // One queued slot forwarded from the pool options.
    const int queued = connect_to(port);
    send_request(queued, "/@capsid/orders/queued", true);
    const RawHttpResponse rejected = http_get(port, "/@capsid/orders/third");
    require(rejected.status == 429,
            "pool queue-full must reject with 429, got " +
                std::to_string(rejected.status));

    const RawHttpResponse held = read_response(holder);
    require(held.status == 200, "pool held request did not complete");
    const RawHttpResponse served = read_response(queued);
    require(served.status == 200, "pool queued request was not served");
    close(holder);
    close(queued);

    pool.request_stop();
    require(pool.wait(&error), "admission pool wait failed: " + error);
}

#endif  // CAPSID_HAS_STATIC_POOL_SERVER

}  // namespace

int main(int argc, char** argv) {
    require(argc == 3, "expected mode and capsid-worker path");
#if !CAPSID_HAS_SINGLE_WORKER_SERVER
    (void)argv;
    fail("SingleWorkerServer is not implemented");
#else
    const std::string mode = argv[1];
    if (mode == "inflight-full-rejects") {
        test_inflight_full_rejects(argv[2]);
    } else if (mode == "queue-full-rejects") {
        test_queue_full_rejects(argv[2]);
    } else if (mode == "queue-timeout-504") {
        test_queue_timeout_returns_504(argv[2]);
    } else if (mode == "worker-death-503") {
        test_worker_death_returns_503(argv[2]);
#if CAPSID_HAS_STATIC_POOL_SERVER
    } else if (mode == "pool-forwards-admission") {
        test_pool_forwards_admission(argv[2]);
#endif
    } else {
        fail("unknown admission mode");
    }
    std::cout << "PASS" << std::endl;
    return 0;
#endif
}
