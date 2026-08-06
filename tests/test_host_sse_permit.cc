// M2 E-2 SSE streaming permit RED/GREEN (design §9.3):
//   - a text/event-stream response must acquire the worker's streaming
//     permit BEFORE its head reaches the client; a permit-exhausted request
//     is cancelled and answered with a synthesized 503 (never a 200 that is
//     then torn down);
//   - the permit is held only by Content-Type, never by a missing
//     Content-Length (plain chunked responses stay on the ordinary
//     inflight/credit path);
//   - the permit is returned exactly once on every completion path
//     (response end, cancel via idle timeout) — a leaked permit would make
//     the next SSE request fail with 503;
//   - maxStreamingInflightPerWorker defaults to 2, must stay below
//     maxInflightPerWorker, with the single documented exception that both
//     are 1 (no concurrency reservation);
//   - stream idle timeout (default 60s, overridden short in these tests)
//     cancels a silent stream and closes its connection.
//
// Frozen assertions: permit-full → 503; completion/cancel releases the
// permit; idle timeout cancels; plain chunked never holds the permit;
// 1/1 boundary starts and serves.

#if __has_include("host/single_worker_server.h")
#include "host/single_worker_server.h"
#define CAPSID_HAS_SINGLE_WORKER_SERVER 1
#else
#define CAPSID_HAS_SINGLE_WORKER_SERVER 0
#endif

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
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

#if CAPSID_HAS_SINGLE_WORKER_SERVER

std::string read_one_ready_line(int fd) {
    std::string line;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (line.empty() || line.back() != '\n') {
        require(std::chrono::steady_clock::now() < deadline,
                "server did not publish READY after start returned");
        struct pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = poll(&descriptor, 1, 50);
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

// One SSE frame, then the stream closes: the normal completed stream.
const std::vector<std::uint8_t>& sse_done_bundle() {
    static const std::string source =
        "export default { fetch: () => { "
        "  const stream = new ReadableStream({ start(controller) { "
        "    controller.enqueue(new TextEncoder().encode('data: hello\\n\\n')); "
        "    controller.close(); } }); "
        "  return new Response(stream, "
        "    { headers: { 'content-type': 'text/event-stream' } }); } };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

// An SSE stream that never emits a frame and never closes: the idle-timeout
// and cancel fixtures.
const std::vector<std::uint8_t>& sse_open_bundle() {
    static const std::string source =
        "export default { fetch: () => { "
        "  return new Response(new ReadableStream({ start() {} }), "
        "    { headers: { 'content-type': 'text/event-stream' } }); } };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

// A plain chunked response (no Content-Length, no SSE type): must never
// hold the streaming permit.
const std::vector<std::uint8_t>& plain_chunked_bundle() {
    static const std::string source =
        "export default { fetch: () => { "
        "  const stream = new ReadableStream({ start(controller) { "
        "    controller.enqueue(new TextEncoder().encode('x'.repeat(4096))); "
        "    controller.close(); } }); "
        "  return new Response(stream, "
        "    { headers: { 'content-type': 'application/octet-stream' } }); } };";
    static const std::vector<std::uint8_t> bundle(source.begin(),
                                                  source.end());
    return bundle;
}

int connect_to(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "cannot create SSE HTTP socket");
    struct timeval timeout = {};
    timeout.tv_sec = 3;
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot set SSE HTTP receive timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode SSE loopback address");
    require(connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to SSE server");
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
        require(count > 0, "cannot write SSE HTTP request");
        sent += static_cast<std::size_t>(count);
    }
}

struct RawHttpResponse {
    unsigned status = 0;
    std::string body;
};

// Exactly one line ("\r\n"-terminated); refills `buffer` from fd.
std::string read_line(int fd, std::string& buffer, char* scratch,
                      const std::size_t scratch_size,
                      const std::chrono::steady_clock::time_point& deadline) {
    std::string::size_type line_end = buffer.find("\r\n");
    while (line_end == std::string::npos) {
        require(std::chrono::steady_clock::now() < deadline,
                "SSE response line timed out");
        const ssize_t count = recv(fd, scratch, scratch_size, 0);
        if (count == 0) {
            fail("SSE response hit EOF mid-line; buffered so far: [" +
                 buffer + "]");
        }
        require(count > 0, "SSE response recv failed");
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
                "SSE response body timed out");
        const ssize_t got = recv(fd, scratch, scratch_size, 0);
        require(got > 0, "SSE response body recv failed");
        buffer.append(scratch, static_cast<std::size_t>(got));
    }
}

// The response head only: status line plus the header block. The caller
// owns what happens after — for an open SSE stream the body never ends.
unsigned read_head(int fd) {
    char scratch[2048];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string buffer;
    const std::string status_line = read_line(fd, buffer, scratch,
                                              sizeof(scratch), deadline);
    const std::string::size_type sp1 = status_line.find(' ');
    require(sp1 != std::string::npos, "malformed status line");
    const unsigned status = static_cast<unsigned>(
        std::strtoul(status_line.c_str() + sp1 + 1, nullptr, 10));
    require(status >= 100, "invalid response status");
    std::string header_line;
    while (!(header_line = read_line(fd, buffer, scratch, sizeof(scratch),
                                     deadline)).empty()) {
    }
    return status;
}

// One complete response: status line, header block, then the body by
// Content-Length or chunked transfer decoding.
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
            const std::string terminator =
                read_line(fd, buffer, scratch, sizeof(scratch), deadline);
            require(terminator.empty(),
                    "chunked response had a non-empty chunk terminator");
        }
    } else {
        require(saw_content_length,
                "SSE response has neither Content-Length nor chunked");
        require_bytes(fd, buffer, body_length, scratch, sizeof(scratch),
                      deadline);
        response.body = buffer.substr(0, body_length);
    }
    return response;
}

// Waits until the server closes the connection (EOF): the idle-timeout
// signature. The connection must already have a response in flight.
void require_connection_closed(int fd) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        require(std::chrono::steady_clock::now() < deadline,
                "server did not close the idle stream");
        char byte = 0;
        const ssize_t count = recv(fd, &byte, 1, 0);
        if (count == 0) {
            return;  // clean EOF: the server cancelled and closed
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count < 0) {
            return;  // reset / error: the server tore the connection down
        }
    }
}

// One-shot GET on a fresh connection.
RawHttpResponse http_get(std::uint16_t port, const std::string& target) {
    const int fd = connect_to(port);
    send_request(fd, target, false);
    const RawHttpResponse response = read_response(fd);
    close(fd);
    return response;
}

capsid::host::SingleWorkerServerOptions make_options(
    const char* worker_path, int ready_fd) {
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = worker_path;
    options.source_bundle_path = "sse-inline";
    options.source_name = "file://sse/v1/bundle.mjs";
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

// Permit exhausted while the head has not reached the client: the second
// SSE request must be cancelled and answered with a synthesized 503 (never
// a 200 that is torn down).
void test_permit_full_rejects_503(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create SSE READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 2;
    options.max_streaming_inflight_per_worker = 1;
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(sse_open_bundle(), &error),
            "cannot start SSE server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    // The first stream acquires the only permit and never ends (the open
    // bundle emits no frame and never closes), so the slot stays held
    // while the second request arrives — the permit-exhausted case.
    const int first = connect_to(port);
    send_request(first, "/@capsid/orders/stream", true);
    const int second = connect_to(port);
    send_request(second, "/@capsid/orders/stream", true);
    const unsigned first_status = read_head(first);
    const unsigned second_status = read_head(second);
    require(first_status == 200,
            "first SSE request failed: " + std::to_string(first_status));
    // The second stream cannot acquire the permit before its head: it must
    // be cancelled and answered with a synthesized 503 — never a 200 that
    // is then torn down.
    require(second_status == 503,
            "permit-exhausted SSE must be synthesized 503, got " +
                std::to_string(second_status));
    close(first);
    close(second);

    server.request_stop();
    require(server.wait(&error), "SSE server wait failed: " + error);
}

// The permit is returned exactly once when the stream ends: a fresh SSE
// request after a completed one must acquire the permit and succeed.
void test_permit_released_on_completion(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create SSE READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 2;
    options.max_streaming_inflight_per_worker = 1;
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(sse_done_bundle(), &error),
            "cannot start SSE server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    for (int round = 0; round < 2; ++round) {
        const RawHttpResponse response =
            http_get(port, "/@capsid/orders/stream");
        require(response.status == 200,
                "completed SSE round " + std::to_string(round) +
                    " failed: " + std::to_string(response.status));
    }

    server.request_stop();
    require(server.wait(&error), "SSE server wait failed: " + error);
}

// The idle timeout cancels a silent stream and closes its connection; the
// permit is returned so a fresh SSE request still succeeds.
void test_idle_timeout_cancels_and_releases(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create SSE READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 2;
    options.max_streaming_inflight_per_worker = 1;
    options.stream_idle_timeout_ms = 250;  // well below the 60s default
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(sse_open_bundle(), &error),
            "cannot start SSE server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    // The open stream holds the permit; its head reaches the client first
    // (the head may not be withheld — only the permit gate may 503 before
    // the head), then silence trips the idle timer.
    const int connection = connect_to(port);
    send_request(connection, "/@capsid/orders/stream", true);
    const unsigned head = read_head(connection);
    require(head == 200, "SSE head failed: " + std::to_string(head));
    require_connection_closed(connection);
    close(connection);

    // The idle cancel returned the permit exactly once: a fresh stream's
    // head succeeds (read_response would block on the never-ending body, so
    // only the head is read).
    const int next_connection = connect_to(port);
    send_request(next_connection, "/@capsid/orders/stream", true);
    const unsigned next_head = read_head(next_connection);
    require(next_head == 200, "permit leaked after idle cancel: " +
                                  std::to_string(next_head));
    close(next_connection);

    server.request_stop();
    require(server.wait(&error), "SSE server wait failed: " + error);
}

// Plain chunked responses (no Content-Length, no event-stream type) never
// hold the streaming permit: two concurrent plain chunked requests both
// succeed under a 1-slot streaming permit.
void test_plain_chunked_does_not_hold_permit(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create SSE READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 2;
    options.max_streaming_inflight_per_worker = 1;
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(plain_chunked_bundle(), &error),
            "cannot start SSE server: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const int first = connect_to(port);
    send_request(first, "/@capsid/orders/plain", true);
    const int second = connect_to(port);
    send_request(second, "/@capsid/orders/plain", true);
    const RawHttpResponse first_response = read_response(first);
    const RawHttpResponse second_response = read_response(second);
    require(first_response.status == 200 && second_response.status == 200,
            "plain chunked responses must not consume the streaming permit");
    require(first_response.body.size() == 4096,
            "plain chunked body truncated");
    close(first);
    close(second);

    server.request_stop();
    require(server.wait(&error), "SSE server wait failed: " + error);
}

// The documented 1/1 boundary: maxInflightPerWorker == 1 with
// maxStreamingInflightPerWorker == 1 starts and serves (no concurrency
// reservation).
void test_max_inflight_one_boundary(const char* worker_path) {
    int ready[2];
    require(pipe(ready) == 0, "cannot create SSE READY pipe");
    capsid::host::SingleWorkerServerOptions options =
        make_options(worker_path, ready[1]);
    options.max_inflight_per_worker = 1;
    options.max_streaming_inflight_per_worker = 1;
    capsid::host::SingleWorkerServer server(std::move(options));
    std::string error;
    require(server.start(sse_done_bundle(), &error),
            "1/1 SSE boundary must start: " + error);
    const std::uint16_t port = ready_port(read_one_ready_line(ready[0]));

    const RawHttpResponse response = http_get(port, "/@capsid/orders/stream");
    require(response.status == 200, "1/1 SSE request failed: " +
                                        std::to_string(response.status));

    server.request_stop();
    require(server.wait(&error), "SSE server wait failed: " + error);
}

#endif  // CAPSID_HAS_SINGLE_WORKER_SERVER

}  // namespace

int main(int argc, char** argv) {
    require(argc == 3, "expected mode and capsid-worker path");
#if !CAPSID_HAS_SINGLE_WORKER_SERVER
    (void)argv;
    fail("SingleWorkerServer is not implemented");
#else
    const std::string mode = argv[1];
    if (mode == "permit-full-rejects-503") {
        test_permit_full_rejects_503(argv[2]);
    } else if (mode == "permit-released-on-completion") {
        test_permit_released_on_completion(argv[2]);
    } else if (mode == "idle-timeout-cancels-and-releases") {
        test_idle_timeout_cancels_and_releases(argv[2]);
    } else if (mode == "plain-chunked-does-not-hold-permit") {
        test_plain_chunked_does_not_hold_permit(argv[2]);
    } else if (mode == "max-inflight-one-boundary") {
        test_max_inflight_one_boundary(argv[2]);
    } else {
        fail("unknown SSE permit mode");
    }
    std::cout << "PASS" << std::endl;
    return 0;
#endif
}
