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

#include "win32_compat.h"
#include "worker_exit_assertions.h"
#if defined(_WIN32)
#else
#include <arpa/inet.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <dirent.h>
#endif
#include "win32_compat.h"
#include <signal.h>
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

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    // Unwind the server so failed assertions also join/reap its Worker.
    throw std::runtime_error(message);
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
                "server READY pipe closed without a complete record");
#else
        require(read(fd, &byte, 1) == 1,
                "server READY pipe closed without a complete record");
#endif
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

// A warm-up response plus requests that cannot complete before the fault.
// Queue occupancy is established by a real 429, rather than a timed sleep.
const std::vector<std::uint8_t>& worker_death_bundle() {
    static const std::string source =
        "export default { fetch: request => "
        "  new URL(request.url).pathname.endsWith('/alive') ? "
        "  new Response('admission-ok') : new Promise(() => {}) };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

// A streamed (non-fixed) response with an exact Content-Length whose end
// arrives 80 ms after the body: the Host emits it through the buffer_body
// serializer, and keep-alive clients send the next request the moment the
// body is complete — before RESPONSE_END has reached the Host.
const std::vector<std::uint8_t>& streamed_bundle() {
    static const std::string source =
        "const body = new TextEncoder().encode('streamed-ok');\n"
        "export default { fetch: request => new Response(\n"
        "  new ReadableStream({ start(controller) {\n"
        "    controller.enqueue(body);\n"
        "    setTimeout(() => controller.close(),\n"
        "      new URL(request.url).pathname.endsWith('/pending-end') ? 600000 : 80);\n"
        "  }}),\n"
        "  { headers: { 'content-length': String(body.length) } }) };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                   source.end());
    return bundle;
}

int connect_to(std::uint16_t port) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create admission HTTP socket");
    require(capsid::win32::setsockopt_recv_timeout_fd(fd, 3000) == 0,
            "cannot set admission HTTP receive timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode admission loopback address");
    require(capsid::win32::connect_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
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
            capsid::win32::send_fd(fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "cannot write admission HTTP request");
        sent += static_cast<std::size_t>(count);
    }
}

struct RawHttpResponse {
    unsigned status = 0;
    std::string body;
    bool connection_close = false;
};

bool lower_starts_with(const std::string& value, const char* prefix) {
    const std::size_t prefix_size = std::strlen(prefix);
    if (value.size() < prefix_size) {
        return false;
    }
    for (std::size_t index = 0; index < prefix_size; ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

bool lower_contains(const std::string& value, const char* needle) {
    const std::size_t needle_size = std::strlen(needle);
    if (needle_size == 0 || value.size() < needle_size) {
        return false;
    }
    for (std::size_t start = 0; start + needle_size <= value.size(); ++start) {
        bool matches = true;
        for (std::size_t index = 0; index < needle_size; ++index) {
            if (std::tolower(static_cast<unsigned char>(value[start + index])) !=
                std::tolower(static_cast<unsigned char>(needle[index]))) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}


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
        const ssize_t count = capsid::win32::recv_fd(fd, scratch, scratch_size, 0);
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
        const ssize_t got = capsid::win32::recv_fd(fd, scratch, scratch_size, 0);
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
        if (lower_starts_with(header_line, "connection:")) {
            std::string value = header_line.substr(11);
            const auto first = value.find_first_not_of(" \t");
            const auto last = value.find_last_not_of(" \t");
            value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
            response.connection_close = value.size() == 5 && lower_starts_with(value, "close");
        } else if (lower_starts_with(header_line, "content-length:")) {

            body_length = static_cast<std::size_t>(
                std::strtoul(header_line.c_str() + 15, nullptr, 10));
            saw_content_length = true;
        } else if (lower_starts_with(header_line, "transfer-encoding:") &&

                   lower_contains(header_line, "chunked")) {
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
        buffer.erase(0, body_length);
    }
    require(buffer.empty(), "admission response contains unexpected trailing bytes");
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
// (Linux-only: the worker-death case is not registered on Windows, where
// the child-pid scan would need Toolhelp and SIGKILL has no portable
// form; see docs/windows.md.)
pid_t find_worker_child_pid(const char* worker_path) {
#if defined(_WIN32)
    (void)worker_path;
    return static_cast<pid_t>(0);
#else
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
        if (scanned == 2 && ppid == static_cast<long>(self) &&
            std::strcmp(comm, "(capsid-worker") == 0) {
            const auto executable = std::filesystem::path("/proc") / entry->d_name / "exe";
            require(std::filesystem::canonical(executable) == std::filesystem::canonical(worker_path),
                    "worker fault must target the requested executable");
            require(found == -1, "worker-death fixture has more than one Worker child");
            found = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
        }
    }
    closedir(directory);
    return found;
#endif
}

pid_t kill_worker_child(const char* worker_path) {
#if defined(_WIN32)
    (void)worker_path;
    fail("worker-death fault injection is unavailable on Windows");
#else
    const pid_t worker_pid = find_worker_child_pid(worker_path);
    require(worker_pid > 0, "cannot find the live worker child process");
    std::cout << "worker-fault-pid: " << worker_pid << std::endl;
    require(kill(worker_pid, SIGKILL) == 0, "cannot SIGKILL the worker");
    return worker_pid;
#endif
}

// After the worker exits, the shard closes its acceptor: a refused new
// connection is the deterministic "worker-exit processed" signal.
void require_acceptor_closed(std::uint16_t port) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        require(std::chrono::steady_clock::now() < deadline,
                "acceptor never closed after the worker was killed");
        const int fd = capsid::win32::create_tcp_socket_fd();
        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
                "cannot encode admission loopback address");
        const int connected =
            capsid::win32::connect_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
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
    require(capsid::win32::create_socket_pair(ready), "cannot create admission READY pipe");
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
    require(capsid::win32::create_socket_pair(ready), "cannot create admission READY pipe");
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

// Third gate: a queued request whose queue or Host deadline expires gets 504
// (§10.3).
void test_queue_timeout_returns_504(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create admission READY pipe");
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

// ④ gate: requests already admitted before Worker exit get complete 503
// responses before their connections close. An idle keep-alive socket has
// no pending request; its bounded reclamation is covered by the external-
// accept exit tests, and it cannot be kept alive to await a future request.
void test_worker_death_returns_503(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create admission READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_worker_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 1;
    options.max_streaming_inflight_per_worker = 1;
    options.queue_requests = 1;
    options.request_timeout_ms = 15000;
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(worker_death_bundle(), &error),
            "cannot start admission server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int warm = connect_to(port);
    send_request(warm, "/@capsid/orders/alive", false);
    const RawHttpResponse alive = read_response(warm);
    require(alive.status == 200 && alive.body == "admission-ok",
            "worker did not answer before the fault");
    char warm_extra = 0;
    require(capsid::win32::recv_fd(warm, &warm_extra, 1, 0) == 0,
            "warm-up response must finish before filling admission slots");
    close(warm);

    std::vector<capsid_pollfd> pending(3);
    for (auto& descriptor : pending) {
        const int connection = connect_to(port);
        descriptor.fd = connection;
        descriptor.events = POLLIN;
        send_request(connection, "/@capsid/orders/pending", true);
    }
    // Which request wins the execution/queue slots is intentionally not
    // assumed. A third request can get 429 only after both slots are full;
    // neither parked request can finish or time out at this barrier.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    int rejected = -1;
    while (rejected < 0) {
        require(std::chrono::steady_clock::now() < deadline,
                "worker-death fixture never filled inflight and queue slots");
        const int polled = capsid::win32::capsid_poll(pending.data(), pending.size(), 50);
        require(polled >= 0, "cannot poll admission capacity barrier");
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (pending[index].revents == 0) continue;
            require(rejected < 0 && (pending[index].revents & POLLIN) != 0,
                    "unexpected connection termination at admission barrier");
            // These poll descriptors were populated exclusively from the
            // int-valued compatibility handles returned by connect_to().
            const RawHttpResponse full = read_response(static_cast<int>(pending[index].fd));
            require(full.status == 429 && full.body == "app queue full",
                    "capacity barrier must be an actual queue-full 429");
            rejected = static_cast<int>(index);
        }
    }
    close(static_cast<int>(pending[static_cast<std::size_t>(rejected)].fd));
    require(server.worker_available(), "Worker exited before the admission barrier");
    std::cout << "admission-full: inflight=1 queued=1 rejected=1" << std::endl;
    const pid_t worker_pid = kill_worker_child(worker_path);
    require_acceptor_closed(port);
    unsigned unavailable_responses = 0;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        if (static_cast<int>(index) == rejected) continue;
        const int connection = static_cast<int>(pending[index].fd);
        const RawHttpResponse after = read_response(connection);
        require(after.status == 503 && after.body == "worker exited" && after.connection_close,
                "admitted request must receive a complete 503 with Connection: close");
        char extra = 0;
        require(capsid::win32::recv_fd(connection, &extra, 1, 0) == 0,
                "503 response must be followed by TCP EOF without extra bytes");
        close(connection);
        ++unavailable_responses;
    }
    require(unavailable_responses == 2, "inflight and queued requests must both receive 503");

    server.request_stop();
    require(server.wait(&error), "admission server wait failed: " + error);
#if !defined(_WIN32)
    require(kill(worker_pid, 0) != 0 && errno == ESRCH, "faulted Worker PID was not reaped");
#else
    (void)worker_pid;
#endif
    std::cout << "worker-exit: complete503=2 tcpEof=2 workerReaped=true" << std::endl;
}


// Keep-alive clients may legally send the next request the instant they
// have read the full response body. The streamed (buffer_body) path writes
// the end block as a separate completion; the next request's data can
// therefore arrive while the current request is still registered. It must
// be left in the socket buffer for the next read_request, not treated as
// forbidden pipelining (the probe closes the connection). Frozen by ab -k:
// before the fix every run had dozens of failed requests.
void test_keepalive_streamed_responses(const char* worker_path, const std::string& mode) {
    const bool drain = mode != "keepalive-streamed";
    const bool forced = mode == "keepalive-streamed-drain-deadline";
    const bool idle = mode == "idle-drain";
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create admission READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_worker_options(worker_path, ready[1]);
    options.install_process_signals = mode != "keepalive-streamed-drain-no-signals";
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(streamed_bundle(), &error),
            "cannot start admission server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int kConnections = idle ? 0 : 4;
    constexpr int kRequestsPerConnection = 12;
    std::vector<int> connections;
    connections.reserve(kConnections);
    for (int connection = 0; connection < kConnections; ++connection) {
        const int fd = connect_to(port);
        send_request(fd, "/@capsid/orders/stream", true);
        connections.push_back(fd);
    }
    for (const int fd : connections) {
        for (int request = 0; request < kRequestsPerConnection; ++request) {
            const RawHttpResponse response = read_response(fd);
            require(response.status == 200,
                    "streamed keep-alive response status was " +
                        std::to_string(response.status));
            require(response.body == "streamed-ok",
                    "streamed keep-alive response body was corrupted");
            if (request + 1 < kRequestsPerConnection) {
                // Send the next request immediately, exactly like ab -k /
                // a keep-alive agent: the previous response is complete
                // from the client's point of view.
                // The stop control leaves the final stream genuinely open;
                // scheduling cannot turn its cancellation into a clean end.
                const bool final_pending = (!drain || forced) && request + 2 == kRequestsPerConnection;
                send_request(fd, final_pending ? "/@capsid/orders/pending-end"
                                               : "/@capsid/orders/stream", true);
            }
        }
        if (!drain) close(fd);
    }

    const auto started = std::chrono::steady_clock::now();
    if (drain) server.begin_drain(forced ? 200 : 5000);
    else server.request_stop();
    if (drain) {
        const auto deadline = started + std::chrono::milliseconds(2500);
        while (!server.worker_exit_status().observed &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Separate a completed Worker reap from an io_context still held
        // alive by an unrelated outstanding operation (such as signal wait).
        std::cout << "HOST_WORKER_BEFORE_WAIT ";
        capsid_test::print_worker_exit(server.worker_exit_status());
        std::cout << std::endl;
    }
    require(server.wait(&error), "admission server wait failed: " + error);
    if (drain) {
        for (const int fd : connections) close(fd);
        const auto metrics = server.drain_metrics();
        std::cout << "HOST_DRAIN {\"finished\":" << (metrics.finished ? "true" : "false")
                  << ",\"forcedCancellations\":" << metrics.forced_cancellations
                  << ",\"totalMs\":" << metrics.total_ms << "}" << std::endl;
        require(metrics.draining && metrics.finished &&
                    metrics.forced_cancellations == static_cast<std::uint64_t>(forced ? kConnections : 0),
                "streamed drain cancellation count disagrees with its deadline");
        require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(2500),
                "drain did not join promptly after completion/deadline");
    }
    const auto status = server.worker_exit_status();
    std::cout << "HOST_WORKER_EXIT ";
    capsid_test::print_worker_exit(status);
    std::cout << std::endl;
    require(drain && !forced ? status.clean() : capsid_test::natural_exit(status, 1),
            "streamed drain/unfinished stop has the wrong actual Worker exit");
}

#endif  // CAPSID_HAS_SINGLE_WORKER_SERVER

#if CAPSID_HAS_STATIC_POOL_SERVER

// The pool forwards its admission options into every shard: the shard's
// queue depth comes from StaticPoolServerOptions, not from per-shard code.
void test_pool_forwards_admission(const char* worker_path) {
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create admission READY pipe");
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

int main(int argc, char** argv) try {
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
    } else if (mode == "keepalive-streamed") {
        test_keepalive_streamed_responses(argv[2], mode);
    } else if (mode == "keepalive-streamed-drain" || mode == "idle-drain" ||
               mode == "keepalive-streamed-drain-deadline" ||
               mode == "keepalive-streamed-drain-no-signals") {
        test_keepalive_streamed_responses(argv[2], mode);
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
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
}
