#include "capsid/runtime.h"
#include "egress_test_policy.h"
#include "graceful_worker_exit.h"

#include "win32_compat.h"
#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <netinet/in.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/socket.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <netdb.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <unistd.h>
#endif

#include <libwebsockets.h>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

void require_clean_worker_shutdown(
    capsid_worker *worker,
    const char *context) {
    const capsid_test::GracefulWorkerExit result =
        capsid_test::shutdown_and_wait(worker, 2000);
    capsid_worker_destroy(worker);

    if (result.shutdown_result != CAPSID_OK) {
        fail(std::string(context) + " could not queue graceful shutdown: " +
             capsid_result_string(result.shutdown_result));
    }
    if (result.flush_result != CAPSID_OK) {
        fail(std::string(context) + " could not flush graceful shutdown: " +
             capsid_result_string(result.flush_result));
    }
    if (result.wait_error != 0) {
        fail(std::string(context) + " waitpid failed: " +
             std::strerror(result.wait_error));
    }
    if (!result.reaped) {
        fail(std::string(context) +
             " did not exit within the graceful shutdown deadline");
    }
    if (WIFSIGNALED(result.status)) {
        fail(std::string(context) + " terminated by signal " +
             std::to_string(WTERMSIG(result.status)));
    }
    if (!WIFEXITED(result.status) || WEXITSTATUS(result.status) != 0) {
        fail(std::string(context) + " exited abnormally with status " +
             std::to_string(result.status));
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

std::string lower(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(value[i])));
    }
    return value;
}

std::string percent_decode(const std::string &value) {
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            char hex[3] = { value[i + 1], value[i + 2], '\0' };
            char *end = NULL;
            const long decoded = std::strtol(hex, &end, 16);
            if (end && *end == '\0') {
                result.push_back(static_cast<char>(decoded));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return result;
}

std::string query_value(const std::string &target, const std::string &name) {
    const size_t query = target.find('?');
    if (query == std::string::npos) {
        return std::string();
    }
    size_t cursor = query + 1;
    while (cursor <= target.size()) {
        const size_t amp = target.find('&', cursor);
        const size_t end = amp == std::string::npos ? target.size() : amp;
        const size_t equal = target.find('=', cursor);
        if (equal != std::string::npos && equal < end &&
            target.substr(cursor, equal - cursor) == name) {
            return percent_decode(target.substr(equal + 1, end - equal - 1));
        }
        if (amp == std::string::npos) {
            break;
        }
        cursor = amp + 1;
    }
    return std::string();
}

bool send_all(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
#ifdef MSG_NOSIGNAL
        const ssize_t count = capsid::win32::send_fd(fd, data + offset, size - offset, 0);
#else
        const ssize_t count = capsid::win32::send_fd(fd, data + offset, size - offset, 0);
#endif
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool send_all(int fd, const std::string &data) {
    return send_all(fd, data.data(), data.size());
}

struct HttpRequest {
    std::string method;
    std::string target;
    std::map<std::string, std::string> headers;
    std::string body;
};

bool receive_more(int fd, std::string *wire) {
    char buffer[8192];
    for (;;) {
        const ssize_t count = capsid::win32::recv_fd(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        wire->append(buffer, static_cast<size_t>(count));
        return true;
    }
}

bool read_http_request(int fd, HttpRequest *request) {
    std::string wire;
    size_t header_end = std::string::npos;
    while ((header_end = wire.find("\r\n\r\n")) == std::string::npos) {
        if (!receive_more(fd, &wire) || wire.size() > 2u * 1024u * 1024u) {
            return false;
        }
        if (!wire.empty() &&
            !std::isalpha(static_cast<unsigned char>(wire[0]))) {
            return false;
        }
    }

    const size_t request_line_end = wire.find("\r\n");
    if (request_line_end == std::string::npos) {
        return false;
    }
    std::istringstream request_line(wire.substr(0, request_line_end));
    std::string version;
    if (!(request_line >> request->method >> request->target >> version)) {
        return false;
    }

    size_t cursor = request_line_end + 2;
    while (cursor < header_end) {
        const size_t line_end = wire.find("\r\n", cursor);
        if (line_end == std::string::npos || line_end > header_end) {
            return false;
        }
        const size_t colon = wire.find(':', cursor);
        if (colon == std::string::npos || colon > line_end) {
            return false;
        }
        std::string name = lower(wire.substr(cursor, colon - cursor));
        size_t value_start = colon + 1;
        while (value_start < line_end &&
               (wire[value_start] == ' ' || wire[value_start] == '\t')) {
            ++value_start;
        }
        const std::string value = wire.substr(value_start, line_end - value_start);
        std::map<std::string, std::string>::iterator existing =
            request->headers.find(name);
        if (existing == request->headers.end()) {
            request->headers[name] = value;
        } else {
            existing->second += ", " + value;
        }
        cursor = line_end + 2;
    }

    cursor = header_end + 4;
    const std::map<std::string, std::string>::const_iterator transfer =
        request->headers.find("transfer-encoding");
    if (transfer != request->headers.end() &&
        lower(transfer->second).find("chunked") != std::string::npos) {
        for (;;) {
            size_t line_end = wire.find("\r\n", cursor);
            while (line_end == std::string::npos) {
                if (!receive_more(fd, &wire)) {
                    return false;
                }
                line_end = wire.find("\r\n", cursor);
            }
            const std::string size_text = wire.substr(cursor, line_end - cursor);
            char *end = NULL;
            const unsigned long chunk_size =
                std::strtoul(size_text.c_str(), &end, 16);
            if (!end || (*end != '\0' && *end != ';')) {
                return false;
            }
            cursor = line_end + 2;
            if (chunk_size == 0) {
                /*
                 * Last chunk.  The request is complete here; any trailer
                 * bytes are optional and the peer may already have shut the
                 * write side down, so do not block waiting for them.
                 */
                return true;
            }
            while (wire.size() < cursor + chunk_size + 2) {
                if (!receive_more(fd, &wire)) {
                    return false;
                }
            }
            request->body.append(wire, cursor, chunk_size);
            cursor += chunk_size;
            if (wire.compare(cursor, 2, "\r\n") != 0) {
                return false;
            }
            cursor += 2;
        }
    }

    size_t content_length = 0;
    const std::map<std::string, std::string>::const_iterator length =
        request->headers.find("content-length");
    if (length != request->headers.end()) {
        content_length = static_cast<size_t>(
            std::strtoull(length->second.c_str(), NULL, 10));
    }
    while (wire.size() < cursor + content_length) {
        if (!receive_more(fd, &wire)) {
            return false;
        }
    }
    request->body.assign(wire, cursor, content_length);
    return true;
}

class HttpMatrixServer {
public:
    explicit HttpMatrixServer(bool ipv6 = false)
        : fd_(-1),
          port_(0),
          ipv6_(ipv6),
          stopping_(false),
          requests_(0),
          accepts_(0),
          streaming_connections_reused_(0) {
        fd_ = capsid::win32::create_socket_fd(ipv6_ ? AF_INET6 : AF_INET);
        if (fd_ < 0) {
            fail("cannot create HTTP matrix socket");
        }
        const int reuse = 1;
        capsid::win32::setsockopt_reuseaddr_fd(fd_);
#if defined(IPV6_V6ONLY)
        if (ipv6_) {
#if defined(_WIN32)
            setsockopt(
                fd_, IPPROTO_IPV6, IPV6_V6ONLY,
                reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
            setsockopt(
                fd_, IPPROTO_IPV6, IPV6_V6ONLY, &reuse, sizeof(reuse));
#endif
        }
#endif

        sockaddr_storage storage = {};
        socklen_t size = 0;
        if (ipv6_) {
            sockaddr_in6 *address =
                reinterpret_cast<sockaddr_in6 *>(&storage);
            address->sin6_family = AF_INET6;
            address->sin6_addr = in6addr_loopback;
            address->sin6_port = 0;
            size = sizeof(*address);
        } else {
            sockaddr_in *address =
                reinterpret_cast<sockaddr_in *>(&storage);
            address->sin_family = AF_INET;
            address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address->sin_port = 0;
            size = sizeof(*address);
        }
        if (capsid::win32::bind_fd(fd_,
                 reinterpret_cast<const struct sockaddr *>(&storage),
                 size) != 0 ||
            capsid::win32::listen_fd(fd_, 16) != 0) {
            fail(std::string("cannot start HTTP matrix server: ") +
                 std::strerror(errno));
        }
        size = sizeof(storage);
        if (capsid::win32::getsockname_fd(
                fd_,
                reinterpret_cast<struct sockaddr *>(&storage),
                &size) != 0) {
            fail("cannot resolve HTTP matrix port");
        }
        port_ = ipv6_
                    ? ntohs(reinterpret_cast<sockaddr_in6 *>(&storage)
                                ->sin6_port)
                    : ntohs(reinterpret_cast<sockaddr_in *>(&storage)
                                ->sin_port);
        thread_ = std::thread(&HttpMatrixServer::serve, this);
    }

    ~HttpMatrixServer() {
        stopping_ = true;
        capsid::win32::shutdown_fd(fd_);
        if (thread_.joinable()) {
            thread_.join();
        }
        close(fd_);
    }

    uint16_t port() const { return port_; }

    std::string base_url() const {
        return std::string(ipv6_ ? "http://[::1]:"
                                 : "http://127.0.0.1:") +
               std::to_string(port_);
    }

    unsigned int requests() const { return requests_; }

private:
    static std::string header(const HttpRequest &request, const char *name) {
        const std::map<std::string, std::string>::const_iterator found =
            request.headers.find(name);
        return found == request.headers.end() ? std::string() : found->second;
    }

    bool respond(int client, const HttpRequest &request) {
        const std::string path =
            request.target.substr(0, request.target.find('?'));
        {
            std::lock_guard<std::mutex> lock(served_path_mutex_);
            ++served_path_counts_[path];
        }
        if (path == "/served-path-count") {
            const std::string name = query_value(request.target, "name");
            std::lock_guard<std::mutex> lock(served_path_mutex_);
            const std::string body = std::to_string(served_path_counts_[name]);
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Connection: keep-alive\r\n\r\n"
                     << body;
            send_all(client, response.str());
            return true;
        }
        if (path == "/headers") {
            const std::string body = header(request, "x-request-duplicate");
            std::ostringstream response;
            response << "HTTP/1.1 200 Matrix Phrase\r\n"
                     << "X-Duplicate: one\r\n"
                     << "X-Duplicate: two\r\n"
                     << "Set-Cookie: a=1; Path=/\r\n"
                     << "Set-Cookie: b=2; Path=/\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Connection: keep-alive\r\n\r\n"
                     << body;
            send_all(client, response.str());
            return true;
        }
        if (path == "/cookie-set") {
            const std::string body = "cookie set";
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Set-Cookie: session=abc123def456; Path=/; Secure; "
                        "HttpOnly\r\n"
                     << "Set-Cookie: domain_session=domain_value; "
                        "Domain=127.0.0.1; Path=/; HttpOnly\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Connection: keep-alive\r\n\r\n"
                     << body;
            send_all(client, response.str());
            return true;
        }
        if (path == "/redirect") {
            const std::string code = query_value(request.target, "code");
            const std::string destination = query_value(request.target, "to");
            std::ostringstream response;
            response << "HTTP/1.1 " << code << " Redirect\r\n"
                     << "Location: " << destination << "\r\n"
                     << "Content-Length: 0\r\n"
                     << "Connection: keep-alive\r\n\r\n";
            send_all(client, response.str());
            return true;
        }
        if (path == "/redirect-loop") {
            send_all(client,
                "HTTP/1.1 302 Found\r\n"
                "Location: /redirect-loop\r\n"
                "Content-Length: 0\r\n"
                "Connection: keep-alive\r\n\r\n");
            return true;
        }
        if (path == "/not-modified") {
            send_all(client,
                "HTTP/1.1 304 Not Modified\r\n"
                "X-Not-Modified: yes\r\n"
                "Connection: keep-alive\r\n\r\n");
            return true;
        }
        if (path == "/inspect") {
            const std::string body =
                request.method + "|" + request.body + "|" +
                header(request, "x-preserved") + "|" +
                header(request, "content-type") + "|" +
                header(request, "authorization");
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Connection: keep-alive\r\n\r\n"
                     << body;
            send_all(client, response.str());
            return true;
        }
        if (path == "/upload") {
            const unsigned int first = request.body.empty()
                                           ? 0
                                           : static_cast<unsigned char>(
                                                 request.body.front());
            const unsigned int last = request.body.empty()
                                          ? 0
                                          : static_cast<unsigned char>(
                                                request.body.back());
            const std::string body =
                std::to_string(request.body.size()) + "|" +
                std::to_string(first) + "|" + std::to_string(last);
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Connection: keep-alive\r\n\r\n"
                     << body;
            send_all(client, response.str());
            return true;
        }
        if (path == "/stream-response") {
            send_all(client,
                "HTTP/1.1 200 OK\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: keep-alive\r\n\r\n"
                "5\r\nalpha\r\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            send_all(client, "4\r\nbeta\r\n0\r\n\r\n");
            return true;
        }
        if (path == "/large-response") {
            send_all(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 524288\r\n"
                "Connection: keep-alive\r\n\r\n");
            const std::string chunk(8192, 'z');
            for (size_t sent = 0; sent < 524288; sent += chunk.size()) {
                if (!send_all(client, chunk)) {
                    return false;
                }
            }
            return true;
        }
        if (path == "/sized-response") {
            const size_t body_size = static_cast<size_t>(
                std::strtoull(
                    query_value(request.target, "size").c_str(), NULL, 10));
            const bool chunked =
                query_value(request.target, "chunked") == "1";
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n";
            if (chunked) {
                response << "Transfer-Encoding: chunked\r\n";
            } else {
                response << "Content-Length: " << body_size << "\r\n";
            }
            response << "Connection: keep-alive\r\n\r\n";
            if (!send_all(client, response.str())) {
                return false;
            }
            const std::string chunk(8192, 'l');
            size_t sent = 0;
            while (sent < body_size) {
                const size_t count =
                    std::min(chunk.size(), body_size - sent);
                if (chunked) {
                    std::ostringstream prefix;
                    prefix << std::hex << count << "\r\n";
                    if (!send_all(client, prefix.str()) ||
                        !send_all(client, chunk.data(), count) ||
                        !send_all(client, "\r\n", 2)) {
                        return false;
                    }
                } else if (!send_all(client, chunk.data(), count)) {
                    return false;
                }
                sent += count;
            }
            if (chunked && !send_all(client, "0\r\n\r\n", 5)) {
                return false;
            }
            return true;
        }
        if (path == "/abort-body") {
            send_all(client,
                "HTTP/1.1 200 OK\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: keep-alive\r\n\r\n"
                "5\r\nfirst\r\n");
            capsid_pollfd descriptor = {};
            descriptor.fd = client;
            descriptor.events = POLLIN | POLLHUP;
            capsid::win32::capsid_poll(&descriptor, 1, 5000);
            return false;
        }
        if (path == "/close") {
            send_all(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 0\r\n"
                "Connection: keep-alive\r\n\r\n");
            return false;
        }
        if (path == "/accept-count") {
            // Reported to the fixture so connection-reuse stages can assert
            // that sequential fetches share one pooled connection.
            const std::string body = std::to_string(accepts_);
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Connection: keep-alive\r\n\r\n"
                     << body;
            send_all(client, response.str());
            return true;
        }
        if (path == "/streaming-reuse-count") {
            // Incremented per request served on a connection that already
            // carried the chunked POST upload. The streaming-request stage
            // asserts it stays 0: that connection must never re-enter the
            // keep-alive pool.
            const std::string body =
                std::to_string(streaming_connections_reused_.load());
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Connection: keep-alive\r\n\r\n"
                     << body;
            send_all(client, response.str());
            return true;
        }
        if (path == "/conn-close") {
            // A `Connection: close` response must evict the connection from
            // the fetch pool; returning false closes the socket afterwards.
            send_all(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n");
            return false;
        }

        send_all(client,
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n\r\n");
        return false;
    }

    void serve_client(int client) {
#ifdef SO_NOSIGPIPE
        // Linux/macOS only: suppress SIGPIPE on the accepted socket.
        const int no_sigpipe = 1;
        setsockopt(
            client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        bool served_streaming_upload = false;
        for (;;) {
            HttpRequest request;
            if (!read_http_request(client, &request)) {
                if (!stopping_ && !request.method.empty()) {
                    std::cerr << "HTTP matrix parse failed: method="
                              << request.method << " target=" << request.target
                              << " content-length="
                              << header(request, "content-length")
                              << " transfer-encoding="
                              << header(request, "transfer-encoding")
                              << " body=" << request.body.size() << std::endl;
                }
                break;
            }
            ++requests_;
            const bool streaming_upload =
                request.method == "POST" &&
                (request.target == "/upload" ||
                 request.target.rfind("/upload?", 0) == 0);
            if (served_streaming_upload && !streaming_upload) {
                // A connection that carried the chunked POST must never be
                // handed to a later keep-alive request.
                streaming_connections_reused_.fetch_add(
                    1, std::memory_order_relaxed);
            } else if (streaming_upload) {
                served_streaming_upload = true;
            }
            if (!respond(client, request)) {
                break;
            }
        }
        close(client);
    }

    /*
     * Each accepted connection is served on its own thread.  A single-threaded
     * accept/serve loop deadlocks the redirect cases: the client opens the next
     * hop from inside the lws COMPLETED_CLIENT_HTTP callback, before this
     * process returns to accept(), so the new connection sits in the backlog
     * while lws observes POLLHUP and fails it with "Peer hung up".
     */
    void serve() {
        std::vector<std::thread> workers;
        while (!stopping_) {
            capsid_pollfd descriptor = {};
            descriptor.fd = fd_;
            descriptor.events = POLLIN;
            if (capsid::win32::capsid_poll(&descriptor, 1, 100) <= 0) {
                continue;
            }
            const int client = capsid::win32::accept_fd(fd_);
            if (client < 0) {
                continue;
            }
            ++accepts_;
            workers.push_back(std::thread(&HttpMatrixServer::serve_client,
                                          this, client));
        }
        for (size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
    }

    int fd_;
    uint16_t port_;
    bool ipv6_;
    std::atomic<bool> stopping_;
    std::atomic<unsigned int> requests_;
    std::atomic<unsigned int> accepts_;
    std::atomic<unsigned int> streaming_connections_reused_;
    std::mutex served_path_mutex_;
    std::map<std::string, unsigned int> served_path_counts_;
    std::thread thread_;
};

struct CookieJarProbe {
    bool completed;
    std::string error;

    CookieJarProbe() : completed(false) {}
};

int cookie_jar_callback(struct lws *wsi,
                        enum lws_callback_reasons reason,
                        void *,
                        void *in,
                        size_t len) {
    CookieJarProbe *probe = static_cast<CookieJarProbe *>(
        lws_context_user(lws_get_context(wsi)));
    switch (reason) {
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        probe->error = in ? static_cast<const char *>(in)
                          : "cookie probe connection error";
        probe->completed = true;
        lws_cancel_service(lws_get_context(wsi));
        break;
    case LWS_CALLBACK_RECEIVE_CLIENT_HTTP: {
        char buffer[1024 + LWS_PRE];
        char *payload = buffer + LWS_PRE;
        int available = sizeof(buffer) - LWS_PRE;
        if (lws_http_client_read(wsi, &payload, &available) < 0) {
            probe->error = "cookie probe response read failed";
            probe->completed = true;
            return -1;
        }
        break;
    }
    case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
    case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
        probe->completed = true;
        lws_cancel_service(lws_get_context(wsi));
        break;
    default:
        break;
    }
    return lws_callback_http_dummy(wsi, reason, NULL, in, len);
}

const struct lws_protocols cookie_jar_protocols[] = {
    { "capsid-cookie-jar-probe", cookie_jar_callback, 0, 0, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

void verify_cookie_attribute_storage(const HttpMatrixServer &server) {
    // txiki.js #1064: lws used the attribute-table index for the cookie
    // element array, so HttpOnly overwrote the name and Secure overwrote the
    // value. Exercise the actual lws library built from Capsid's overlay and
    // assert its persisted Netscape-cookie fields, independent of the
    // worker's intentionally empty cookie-jar path.
    const std::string jar_path =
        ".capsid-cookie-jar-" +
        std::to_string(capsid::win32::getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count()) +
        ".txt";

    CookieJarProbe probe;
    struct lws_context_creation_info context_info = {};
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = cookie_jar_protocols;
    context_info.http_nsc_filepath = jar_path.c_str();
    context_info.user = &probe;
    context_info.fd_limit_per_thread = 4;
    struct lws_context *context = lws_create_context(&context_info);
    require(context != NULL, "cannot create lws cookie probe context");

    struct lws_client_connect_info connect_info = {};
    connect_info.context = context;
    connect_info.address = "127.0.0.1";
    connect_info.port = server.port();
    connect_info.path = "/cookie-set";
    connect_info.host = connect_info.address;
    connect_info.origin = connect_info.address;
    connect_info.method = "GET";
    connect_info.protocol = cookie_jar_protocols[0].name;
    connect_info.ssl_connection = LCCSCF_CACHE_COOKIES;
    require(lws_client_connect_via_info(&connect_info) != NULL,
            "cannot start lws cookie probe request");

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!probe.completed &&
           std::chrono::steady_clock::now() < deadline) {
        lws_service(context, 20);
    }
    lws_context_destroy(context);
    require(probe.completed, "lws cookie probe timed out");
    require(probe.error.empty(), probe.error.c_str());

    const std::string jar = read_file(jar_path.c_str());
    std::remove(jar_path.c_str());
    std::remove((jar_path + ".LCK").c_str());
    require(jar.find("\tTRUE\t0\tsession\tabc123def456") !=
                std::string::npos,
            "Secure/HttpOnly cookie name or value was corrupted in the jar");
    require(jar.find("127.0.0.1\tFALSE\t/\tFALSE\t0\t"
                     "domain_session\tdomain_value") != std::string::npos,
            "HttpOnly domain cookie was incorrectly changed to host-only");
}

uint16_t unused_port() {
    const int fd = capsid::win32::create_tcp_socket_fd();
    if (fd < 0) {
        fail("cannot create unused-port socket");
    }
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (capsid::win32::bind_fd(fd, reinterpret_cast<const struct sockaddr *>(&address),
             sizeof(address)) != 0) {
        close(fd);
        fail("cannot bind unused-port socket");
    }
    socklen_t size = sizeof(address);
    capsid::win32::getsockname_fd(fd, reinterpret_cast<struct sockaddr *>(&address), &size);
    const uint16_t port = ntohs(address.sin_port);
    close(fd);
    return port;
}

// The multi-address retry regression needs the loopback family the system
// resolver lists LAST for "localhost": the server binds that family, the
// earlier family stays closed, and a successful fetch can only come from
// retrying the next pre-resolved address. Returns 0 when localhost resolves
// to fewer than two distinct families (the test then skips).
int localhost_last_family() {
    capsid::win32::ensure_winsock();
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG | AI_V4MAPPED;
    struct addrinfo *list = NULL;
    if (getaddrinfo("localhost", NULL, &hints, &list) != 0 ||
        list == NULL) {
        return 0;
    }
    int last = 0;
    bool seen_ipv4 = false;
    bool seen_ipv6 = false;
    for (struct addrinfo *entry = list; entry != NULL;
         entry = entry->ai_next) {
        if (entry->ai_family == AF_INET && !seen_ipv4) {
            seen_ipv4 = true;
            last = AF_INET;
        } else if (entry->ai_family == AF_INET6 && !seen_ipv6) {
            seen_ipv6 = true;
            last = AF_INET6;
        }
    }
    freeaddrinfo(list);
    return seen_ipv4 && seen_ipv6 ? last : 0;
}

uint32_t wait_for_ready(capsid_worker *worker) {
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
                return event.flags;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail("worker startup error");
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK ||
            std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for READY");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

struct Result {
    uint32_t status;
    std::string body;
};

Result run_request(capsid_worker *worker, const std::string &url) {
    require_result(
        capsid_worker_begin_request(worker, 1, "GET", url.c_str(), NULL, 0),
        "begin matrix request");
    require_result(capsid_worker_end_request(worker, 1), "end matrix request");

    Result output = {};
    bool received_head = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
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
            if (event.type == CAPSID_EVENT_LOG) {
                std::cerr << std::string(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size) << std::endl;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                received_head = true;
                output.status = event.status;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                output.body.append(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, event.request_id,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant matrix response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                if (!received_head) {
                    fail("matrix response ended before head");
                }
                return output;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("matrix worker error: ") +
                     std::string(
                         reinterpret_cast<const char *>(event.payload.data),
                         event.payload.size));
            }
            if (event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
                output.status = 0;
                output.body = "matrix request timed out";
                return output;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during matrix request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK ||
            std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for matrix response");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

Result run_policy_probe(const char *worker_path,
                        const std::string &bundle,
                        const capsid_egress_policy *policy,
                        const std::string &target,
                        bool strict_worker = false) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.request_timeout_ms = 10000;
    config.egress_policy = policy;
    config.strict_sandbox = strict_worker ? 1 : 0;

    capsid_worker *worker = NULL;
    require_result(
        capsid_worker_spawn(&config, &worker),
        "spawn egress-policy worker");
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load egress-policy bundle");
    wait_for_ready(worker);
    const Result result = run_request(
        worker,
        "https://example.test/egress-probe?target=" + target);
    capsid_worker_destroy(worker);
    return result;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fail("expected worker path, direct-fetch matrix fixture, and optional --strict");
    }
    const bool strict = argc == 4 && std::string(argv[3]) == "--strict";
    if (argc == 4 && !strict) {
        fail("unknown direct-fetch matrix option");
    }

    HttpMatrixServer primary;
    HttpMatrixServer secondary;
    HttpMatrixServer ipv6(true);
    const uint16_t closed_port = unused_port();

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.request_timeout_ms = 25000;
    config.strict_sandbox = strict ? 1 : 0;
    LoopbackEgressPolicy egress_policy;
    egress_policy.attach(&config);

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn matrix worker");
    const std::string bundle = read_file(argv[2]);
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load matrix bundle");
    const uint32_t sandbox_features = wait_for_ready(worker);
    if (strict) {
        require(
            (sandbox_features & CAPSID_SANDBOX_FEATURE_STRICT_BASE) ==
                CAPSID_SANDBOX_FEATURE_STRICT_BASE,
            "strict direct-fetch worker reports mandatory sandbox features");
    }

    const std::string url =
        "https://example.test/direct-fetch-matrix?primary=" +
        primary.base_url() + "&secondary=" + secondary.base_url() +
        "&closed=" + std::to_string(closed_port);
    const Result result = run_request(worker, url);
    if (strict) {
        require_clean_worker_shutdown(
            worker, "strict direct-fetch matrix worker");
    } else {
        capsid_worker_destroy(worker);
    }

    if (result.status != 200 ||
        result.body.find("\"passed\":true") == std::string::npos ||
        result.body.find("\"network-errors\"") == std::string::npos) {
        fail(std::string("direct-fetch matrix failed with status ") +
             std::to_string(result.status) + " after primary=" +
             std::to_string(primary.requests()) + ", secondary=" +
             std::to_string(secondary.requests()) + ": " + result.body);
    }
    if (primary.requests() < 16 || secondary.requests() < 4) {
        fail("direct-fetch matrix did not exercise expected network paths");
    }

    const uint64_t body_limit = 64u * 1024u;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.request_timeout_ms = 25000;
    config.max_fetch_request_body_bytes = body_limit;
    config.max_fetch_response_body_bytes = body_limit;
    egress_policy.attach(&config);

    worker = NULL;
    require_result(
        capsid_worker_spawn(&config, &worker),
        "spawn body-limit worker");
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load body-limit bundle");
    wait_for_ready(worker);

    const std::string limit_url =
        "https://example.test/body-limits?primary=" +
        primary.base_url() + "&limit=" + std::to_string(body_limit);
    const Result limit_result = run_request(worker, limit_url);
    capsid_worker_destroy(worker);

    if (limit_result.status != 200 ||
        limit_result.body.find("\"passed\":true") == std::string::npos ||
        limit_result.body.find("\"request-body-limit\"") ==
            std::string::npos ||
        limit_result.body.find("\"response-body-limit\"") ==
            std::string::npos) {
        fail(std::string("direct-fetch body limits failed with status ") +
             std::to_string(limit_result.status) + ": " +
             limit_result.body);
    }

    const unsigned int before_default_deny = primary.requests();
    const Result default_deny = run_policy_probe(
        argv[1], bundle, NULL, primary.base_url() + "/headers");
    if (default_deny.status != 200 ||
        default_deny.body.find("\"allowed\":false") ==
            std::string::npos ||
        default_deny.body.find("\"name\":\"TypeError\"") ==
            std::string::npos) {
        fail(std::string("missing egress policy did not deny Fetch: ") +
             default_deny.body);
    }
    require(
        primary.requests() == before_default_deny,
        "deny-all policy reached loopback");

    capsid_egress_policy public_only;
    capsid_egress_policy_init(&public_only);
    public_only.default_action = CAPSID_EGRESS_ALLOW;
    const Result protected_default = run_policy_probe(
        argv[1],
        bundle,
        &public_only,
        primary.base_url() + "/headers");
    require(
        protected_default.body.find("\"allowed\":false") !=
            std::string::npos &&
            primary.requests() == before_default_deny,
        "default allow bypassed protected-address guard");

    capsid_egress_rule host_only_rule;
    capsid_egress_rule_init(&host_only_rule);
    host_only_rule.action = CAPSID_EGRESS_ALLOW;
    host_only_rule.target = "localhost";
    host_only_rule.port_start = primary.port();
    host_only_rule.port_end = primary.port();
    capsid_egress_policy host_only;
    capsid_egress_policy_init(&host_only);
    host_only.rules = &host_only_rule;
    host_only.rule_count = 1;
    const Result hostname_allowed = run_policy_probe(
        argv[1],
        bundle,
        &host_only,
        std::string("http://localhost:") +
            std::to_string(primary.port()) + "/headers");
    require(
        hostname_allowed.status == 200 &&
            hostname_allowed.body.find("\"allowed\":true") !=
            std::string::npos &&
            primary.requests() == before_default_deny + 1,
        "hostname allow did not authorize its resolved loopback address");

    const Result hostname_rule_does_not_allow_ip_literal = run_policy_probe(
        argv[1], bundle, &host_only, primary.base_url() + "/headers");
    require(
        hostname_rule_does_not_allow_ip_literal.body.find(
            "\"allowed\":false") != std::string::npos &&
            primary.requests() == before_default_deny + 1,
        "hostname allow also authorized a direct IP-literal request");

    // Pre-resolved multi-address retry (txiki 0030/0031): on a dual-stack
    // runner "localhost" resolves to both loopback families. Bind the
    // server only on the LAST family so the first connect attempt is
    // refused and the fetch must retry the next pre-resolved address.
    // Single-family runners skip the scenario instead of asserting it
    // vacuously.
    const int last_family = localhost_last_family();
    if (last_family == 0) {
        std::cout << "multi-address localhost retry skipped (single-family "
                     "resolver)"
                  << std::endl;
    } else {
        HttpMatrixServer retry_server(last_family == AF_INET6);
        capsid_egress_rule retry_rule;
        capsid_egress_rule_init(&retry_rule);
        retry_rule.action = CAPSID_EGRESS_ALLOW;
        retry_rule.target = "localhost";
        retry_rule.port_start = retry_server.port();
        retry_rule.port_end = retry_server.port();
        capsid_egress_policy retry_policy;
        capsid_egress_policy_init(&retry_policy);
        retry_policy.rules = &retry_rule;
        retry_policy.rule_count = 1;
        const Result retried = run_policy_probe(
            argv[1],
            bundle,
            &retry_policy,
            std::string("http://localhost:") +
                std::to_string(retry_server.port()) + "/headers");
        require(
            retried.status == 200 &&
                retried.body.find("\"allowed\":true") !=
                    std::string::npos &&
                retry_server.requests() == 1,
            "localhost multi-address fetch did not retry the next resolved "
            "address");
    }

    capsid_egress_rule allow_primary;
    capsid_egress_rule_init(&allow_primary);
    allow_primary.action = CAPSID_EGRESS_ALLOW;
    allow_primary.target = "127.0.0.0/8";
    allow_primary.port_start = primary.port();
    allow_primary.port_end = primary.port();
    capsid_egress_policy primary_only;
    capsid_egress_policy_init(&primary_only);
    primary_only.rules = &allow_primary;
    primary_only.rule_count = 1;
    const Result explicit_allow = run_policy_probe(
        argv[1],
        bundle,
        &primary_only,
        primary.base_url() + "/headers");
    require(
        explicit_allow.status == 200 &&
            explicit_allow.body.find("\"allowed\":true") !=
                std::string::npos &&
            primary.requests() == before_default_deny + 2,
        "explicit loopback CIDR/port allow failed");

    capsid_egress_rule redirect_rules[2];
    capsid_egress_rule_init(&redirect_rules[0]);
    redirect_rules[0] = allow_primary;
    capsid_egress_rule_init(&redirect_rules[1]);
    redirect_rules[1].action = CAPSID_EGRESS_DENY;
    redirect_rules[1].target = "127.0.0.0/8";
    redirect_rules[1].port_start = secondary.port();
    redirect_rules[1].port_end = secondary.port();
    capsid_egress_policy redirect_policy;
    capsid_egress_policy_init(&redirect_policy);
    redirect_policy.rules = redirect_rules;
    redirect_policy.rule_count = 2;
    const unsigned int before_redirect_primary = primary.requests();
    const unsigned int before_redirect_secondary = secondary.requests();
    const Result redirect_denied = run_policy_probe(
        argv[1],
        bundle,
        &redirect_policy,
        primary.base_url() +
            "/redirect?code=302&to=" +
            secondary.base_url() + "/headers");
    require(
        redirect_denied.body.find("\"allowed\":false") !=
            std::string::npos &&
            primary.requests() == before_redirect_primary + 1 &&
            secondary.requests() == before_redirect_secondary,
        "redirect target was not rechecked before connect");

    capsid_egress_rule allow_ipv6;
    capsid_egress_rule_init(&allow_ipv6);
    allow_ipv6.action = CAPSID_EGRESS_ALLOW;
    allow_ipv6.target = "::1/128";
    allow_ipv6.port_start = ipv6.port();
    allow_ipv6.port_end = ipv6.port();
    capsid_egress_policy ipv6_policy;
    capsid_egress_policy_init(&ipv6_policy);
    ipv6_policy.rules = &allow_ipv6;
    ipv6_policy.rule_count = 1;
    const Result ipv6_allowed = run_policy_probe(
        argv[1],
        bundle,
        &ipv6_policy,
        ipv6.base_url() + "/headers");
    require(
        ipv6_allowed.body.find("\"allowed\":true") !=
            std::string::npos &&
            ipv6.requests() == 1,
        "explicit IPv6 loopback CIDR/port allow failed");

    if (strict) {
        // The hostname resolution path under the strict sandbox: the
        // system resolver runs on the pre-warmed libuv work pool, and
        // the hostname allow rule must authorize the resolved loopback
        // address.
        const Result strict_hostname_allowed = run_policy_probe(
            argv[1],
            bundle,
            &host_only,
            std::string("http://localhost:") +
                std::to_string(primary.port()) + "/headers",
            true);
        require(
            strict_hostname_allowed.status == 200 &&
                strict_hostname_allowed.body.find("\"allowed\":true") !=
                    std::string::npos &&
                primary.requests() == before_default_deny + 4,
            "strict sandbox did not authorize a resolved localhost fetch");
    }
    verify_cookie_attribute_storage(primary);
    return 0;
}
