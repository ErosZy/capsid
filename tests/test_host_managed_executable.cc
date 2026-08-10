// Frozen M1D managed executable RED suite.
//
// This is intentionally a process test, not another direct coordinator
// call. It proves that capsid-host has a production managed mode independent
// of the benchmark-only single-worker fixture: host.json is validated and
// compiled, the long-lived Unix Admin service exposes a real source deploy,
// the warmed worker remains owned by the process, and SIGTERM closes the
// control plane and reaps that worker within a bounded time.

#include <jansson.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

pid_t child_process = -1;

[[noreturn]] void fail(const std::string& message) {
    if (child_process > 0) {
        (void)kill(child_process, SIGKILL);
        int ignored = 0;
        (void)waitpid(child_process, &ignored, 0);
        child_process = -1;
    }
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void write_file(const std::string& path, const std::string& bytes) {
    const int fd = open(path.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(fd >= 0, "cannot create managed executable fixture file");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + offset,
                                    bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0, "cannot write managed executable fixture file");
        offset += static_cast<std::size_t>(count);
    }
    require(close(fd) == 0,
            "cannot close managed executable fixture file");
}

void replace_file(const std::string& path, const std::string& bytes) {
    const int fd = open(path.c_str(),
                        O_WRONLY | O_TRUNC | O_CLOEXEC);
    require(fd >= 0, "cannot replace managed executable fixture file");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + offset,
                                    bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0, "cannot replace managed executable fixture file");
        offset += static_cast<std::size_t>(count);
    }
    require(close(fd) == 0,
            "cannot close replaced managed executable fixture file");
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input),
            "cannot read managed executable fixture file");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

struct Fixture {
    std::string root = "/tmp/capsid-managed-executable-XXXXXX";
    std::string applications_root;
    std::string state_root;
    std::string secret_root;
    std::string run_root;
    std::string socket_path;
    std::string host_config;
    std::string diagnostics_path;
    std::string blocking_worker_path;

    Fixture() {
        require(mkdtemp(root.data()) != nullptr,
                "cannot create managed executable fixture");
        applications_root = root + "/applications";
        state_root = root + "/state";
        secret_root = root + "/secrets";
        run_root = root + "/run";
        socket_path = run_root + "/admin.sock";
        host_config = root + "/host.json";
        diagnostics_path = root + "/host.stderr";
        blocking_worker_path = root + "/blocking-worker";
        require(mkdir(applications_root.c_str(), 0700) == 0 &&
                    mkdir(state_root.c_str(), 0700) == 0 &&
                    mkdir(secret_root.c_str(), 0700) == 0 &&
                    mkdir(run_root.c_str(), 0700) == 0,
                "cannot create managed executable directory tree");
        create_application("orders");

        // Every path and authority is explicit. No TCP listener is present:
        // managed mode must not turn a missing listeners array into an
        // accidental public endpoint.
        const std::string document =
            "{\"apiVersion\":\"capsid/host-v1\","
            "\"applicationsRoot\":\"" + applications_root + "\","
            "\"stateRoot\":\"" + state_root + "\","
            "\"secretRootTemplate\":\"" + secret_root +
            "/{application}\","
            "\"admin\":{\"unix\":\"" + socket_path +
            "\",\"mode\":\"0600\"},"
            "\"permissions\":{\"modules\":[\"capsid:env\"],"
            "\"environmentNames\":[\"APP_*\"],\"fsReadRoots\":[],"
            "\"fetchTargets\":[],\"storageNamespaces\":[],"
            "\"stdioStreams\":[]},"
            "\"isolation\":{\"mode\":\"strict\",\"required\":[]},"
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":10000}},"
            "\"capacity\":{\"workersTotal\":1,\"startupsConcurrent\":1}}";
        write_file(host_config, document);
        write_file(blocking_worker_path,
                   "#!/bin/sh\nexec /bin/sleep 30\n");
        require(chmod(blocking_worker_path.c_str(), 0700) == 0,
                "cannot make blocking worker fixture executable");
    }

    // The fetch_body parameter lets a multi-App E2E give each App a
    // distinct response so cross-App routing bleed is observable.
    void create_application(
        const std::string& application,
        const std::string& fetch_body = "managed-executable-ok") {
        const std::string app = applications_root + "/" + application;
        require(mkdir(app.c_str(), 0700) == 0,
                "cannot create managed executable App fixture");
        create_version(application, "v1", fetch_body);
    }

    void create_version(const std::string& application,
                        const std::string& version_id,
                        const std::string& fetch_body =
                            "managed-executable-ok") {
        const std::string version = applications_root + "/" + application +
                                    "/" + version_id;
        require(mkdir(version.c_str(), 0700) == 0,
                "cannot create managed executable Version fixture");
        write_file(
            version + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1}})json");
        write_file(
            version + "/bundle.mjs",
            "export default { fetch: () => new Response('" + fetch_body +
                "') };");
    }

    void replace_host_text(const std::string& old_text,
                           const std::string& new_text) {
        std::string document = read_file(host_config);
        const std::size_t offset = document.find(old_text);
        require(offset != std::string::npos,
                "managed host.json mutation target is absent");
        document.replace(offset, old_text.size(), new_text);
        replace_file(host_config, document);
    }

    ~Fixture() {
        if (child_process > 0) {
            (void)kill(child_process, SIGKILL);
            int ignored = 0;
            (void)waitpid(child_process, &ignored, 0);
            child_process = -1;
        }
        std::error_code ignored;
        (void)std::filesystem::remove_all(root, ignored);
    }

    std::string diagnostics() const {
        std::ifstream input(diagnostics_path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }
};

void start_host_with_config(const Fixture& fixture, const char* host,
                            const char* worker, const char* config_path) {
    const int diagnostics = open(fixture.diagnostics_path.c_str(),
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                                 0600);
    require(diagnostics >= 0,
            "cannot create managed executable diagnostics file");
    child_process = fork();
    require(child_process >= 0, "cannot fork managed Host executable");
    if (child_process == 0) {
        (void)dup2(diagnostics, STDERR_FILENO);
        close(diagnostics);
        execl(host, host,
              "--mode", "managed",
              "--host-config", config_path,
              "--worker", worker,
              static_cast<char*>(nullptr));
        _exit(127);
    }
    close(diagnostics);
}

void start_host(const Fixture& fixture, const char* host,
                const char* worker) {
    start_host_with_config(fixture, host, worker,
                           fixture.host_config.c_str());
}

void require_startup_rejected(const Fixture& fixture,
                              std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    for (;;) {
        struct stat socket_metadata = {};
        require(lstat(fixture.socket_path.c_str(), &socket_metadata) != 0,
                "invalid managed configuration published Admin readiness");
        int status = 0;
        const pid_t waited = waitpid(child_process, &status, WNOHANG);
        require(waited >= 0, "cannot inspect rejected managed Host");
        if (waited == child_process) {
            child_process = -1;
            require(WIFEXITED(status) && WEXITSTATUS(status) != 0,
                    "invalid managed configuration exited successfully");
            return;
        }
        require(std::chrono::steady_clock::now() < deadline,
                "invalid managed configuration blocked Host startup");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void wait_for_socket(const Fixture& fixture) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    for (;;) {
        // Readiness means "the Admin listener accepts connections", not "a
        // socket pathname exists". Crash-injection phases SIGKILL the host
        // and leave its socket file behind; the next start_host() phase
        // removes the stale pathname under the evidence rule and rebinds,
        // so waiting on the bare pathname would connect to a dead listener
        // (ECONNREFUSED) in the window before the new host's bind+listen.
        // Probe with connect(): only a live listener returns success.
        require(fixture.socket_path.size() < sizeof(sockaddr_un{}.sun_path),
                "managed Admin socket path is too long");
        const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        require(probe >= 0, "cannot create managed Admin readiness probe");
        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, fixture.socket_path.c_str(),
                    fixture.socket_path.size() + 1);
        const int probe_result =
            connect(probe, reinterpret_cast<const struct sockaddr*>(&address),
                    sizeof(address));
        close(probe);
        if (probe_result == 0) {
            return;
        }
        int status = 0;
        const pid_t waited = waitpid(child_process, &status, WNOHANG);
        require(waited >= 0, "cannot inspect managed Host process");
        if (waited == child_process) {
            child_process = -1;
            fail("managed Host exited before binding Admin: " +
                 fixture.diagnostics());
        }
        require(std::chrono::steady_clock::now() < deadline,
                "managed Host did not bind Admin: " + fixture.diagnostics());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int connect_admin(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    require(fd >= 0, "cannot create managed Admin client");
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    require(path.size() < sizeof(address.sun_path),
            "managed Admin socket path is too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    require(connect(fd, reinterpret_cast<const struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to managed Admin socket");
    const struct timeval timeout = {5, 0};
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot bound managed Admin response");
    return fd;
}

std::string http_request(const std::string& path,
                         const std::string& request) {
    const int fd = connect_admin(path);
    std::size_t offset = 0;
    while (offset < request.size()) {
#if defined(MSG_NOSIGNAL)
        const ssize_t count = send(fd, request.data() + offset,
                                   request.size() - offset, MSG_NOSIGNAL);
#else
        const ssize_t count = send(fd, request.data() + offset,
                                   request.size() - offset, 0);
#endif
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0, "cannot send managed Admin request");
        offset += static_cast<std::size_t>(count);
    }
    require(shutdown(fd, SHUT_WR) == 0,
            "cannot finish managed Admin request");
    std::string response;
    char buffer[2048];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count >= 0, "managed Admin response timed out");
        if (count == 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return response;
}

json_t* parse_response(const std::string& response, unsigned status) {
    require(response.rfind("HTTP/1.1 " + std::to_string(status) + " ", 0) == 0,
            "managed Admin returned the wrong HTTP status: " + response);
    const std::size_t boundary = response.find("\r\n\r\n");
    require(boundary != std::string::npos,
            "managed Admin returned an incomplete HTTP response");
    const std::string body = response.substr(boundary + 4);
    json_error_t error = {};
    json_t* root = json_loadb(body.data(), body.size(),
                              JSON_REJECT_DUPLICATES, &error);
    require(root != nullptr && json_is_object(root),
            "managed Admin returned invalid JSON");
    return root;
}

std::string deploy(const Fixture& fixture,
                   const std::string& application = "orders",
                   const std::string& version = "v1") {
    const std::string body = "{\"app\":\"" + application +
                             "\",\"version\":\"" + version + "\"}";
    const std::string request =
        "POST /v1/deploy HTTP/1.1\r\nHost: local\r\n"
        "Content-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    json_t* response = parse_response(
        http_request(fixture.socket_path, request), 202);
    json_t* id = json_object_get(response, "operationId");
    require(json_is_string(id),
            "managed deploy omitted its operation ID");
    const std::string operation = json_string_value(id);
    json_decref(response);
    return operation;
}

std::string wait_terminal_state(const Fixture& fixture,
                                const std::string& operation) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(20);
    for (;;) {
        const std::string request =
            "GET /v1/operations/" + operation +
            " HTTP/1.1\r\nHost: local\r\n\r\n";
        json_t* response = parse_response(
            http_request(fixture.socket_path, request), 200);
        json_t* state = json_object_get(response, "state");
        require(json_is_string(state),
                "managed operation omitted its state");
        const std::string value = json_string_value(state);
        json_decref(response);
        if (value == "active" || value == "failed") {
            return value;
        }
        require(std::chrono::steady_clock::now() < deadline,
                "managed executable operation did not become terminal");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void wait_active(const Fixture& fixture, const std::string& operation) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(20);
    for (;;) {
        const std::string request =
            "GET /v1/operations/" + operation +
            " HTTP/1.1\r\nHost: local\r\n\r\n";
        json_t* response = parse_response(
            http_request(fixture.socket_path, request), 200);
        json_t* state = json_object_get(response, "state");
        require(json_is_string(state),
                "managed operation omitted its state");
        const std::string value = json_string_value(state);
        json_decref(response);
        require(value != "failed",
                "managed executable's real deploy failed: " +
                    fixture.diagnostics());
        if (value == "active") {
            return;
        }
        require(std::chrono::steady_clock::now() < deadline,
                "managed executable deploy did not become active");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void require_active_app(const Fixture& fixture,
                        const std::string& application = "orders",
                        const std::string& version = "v1") {
    json_t* response = parse_response(
        http_request(fixture.socket_path,
                     "GET /v1/apps/" + application +
                         " HTTP/1.1\r\nHost: local\r\n\r\n"),
        200);
    require(json_is_true(json_object_get(response, "active")) &&
                json_is_string(json_object_get(response, "version")) &&
                std::string(json_string_value(
                    json_object_get(response, "version"))) == version &&
                json_is_string(json_object_get(response, "generation")),
            "managed executable did not retain its active worker state");
    json_decref(response);
}

void stop_host(const Fixture& fixture) {
    require(kill(child_process, SIGTERM) == 0,
            "cannot signal managed Host");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child_process, &status, WNOHANG);
        require(waited >= 0, "cannot wait for managed Host shutdown");
        if (waited == child_process) {
            child_process = -1;
            break;
        }
        require(std::chrono::steady_clock::now() < deadline,
                "managed Host did not stop within its shutdown bound");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "managed Host shutdown was not clean: " + fixture.diagnostics());
    struct stat metadata = {};
    require(lstat(fixture.socket_path.c_str(), &metadata) != 0 &&
                errno == ENOENT,
            "managed Host left its Admin socket after shutdown");
}

// M2 item 2a crash injection: SIGKILL the Host and require it actually died
// from the signal — the audit anchor proving the crash landed where the
// test says it did (no clean-shutdown path ran, nothing was drained).
void kill_host() {
    require(kill(child_process, SIGKILL) == 0,
            "cannot SIGKILL managed Host");
    int status = 0;
    require(waitpid(child_process, &status, 0) == child_process,
            "cannot wait for crashed managed Host");
    child_process = -1;
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "managed Host did not die from SIGKILL");
}

bool path_exists(const std::string& path) {
    struct stat metadata = {};
    return stat(path.c_str(), &metadata) == 0;
}

// Picks a likely-free loopback port by binding :0 and closing. The
// listener sets SO_REUSEADDR, so the small race is harmless.
int pick_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "cannot create the TCP port probe");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(fd, reinterpret_cast<const struct sockaddr*>(&address),
                 sizeof(address)) == 0,
            "cannot bind the TCP port probe");
    socklen_t length = sizeof(address);
    require(getsockname(fd, reinterpret_cast<struct sockaddr*>(&address),
                        &length) == 0,
            "cannot read the probed TCP port");
    const int port = ntohs(address.sin_port);
    close(fd);
    require(port > 0, "the TCP port probe returned an invalid port");
    return port;
}

// Inserts `count` path-mode public listener entries before the capacity
// block. count > 1 models the §9.2 fail-closed gate (the v1 data plane is
// exactly one listener), which must reject startup before any bind.
void add_public_listener(Fixture& fixture, int port,
                         unsigned count = 1) {
    std::string fragment = "\"listeners\":[";
    for (unsigned index = 0; index < count; ++index) {
        if (index > 0) {
            fragment += ",";
        }
        fragment +=
            "{\"name\":\"public-" + std::to_string(index) +
            "\",\"tcp\":\"127.0.0.1:" + std::to_string(port) +
            "\",\"publicScheme\":\"http\",\"publicAuthority\":\"localhost\","
            "\"trusted\":false,\"routing\":{\"mode\":\"path\"}}";
    }
    fragment += "],";
    fixture.replace_host_text("\"capacity\":", fragment + "\"capacity\":");
}

// One raw HTTP/1.1 exchange against the TCP data-plane listener; reads
// until a Content-Length body, a chunked body's terminal 0-chunk, or the
// connection close. Mirrors the listener contract harness's framing.
struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_exchange_tcp(int port, const std::string& request) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "cannot create the data-plane HTTP socket");
    const struct timeval timeout = {10, 0};
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot set the data-plane receive timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot build the loopback data-plane address");
    require(connect(fd, reinterpret_cast<const struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to the data-plane listener");
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = send(fd, request.data() + sent,
                                   request.size() - sent, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0, "cannot write the data-plane request");
        sent += static_cast<std::size_t>(count);
    }
    std::string wire;
    char buffer[4096];
    bool done = false;
    bool chunked = false;
    std::string chunked_body;
    while (!done) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count >= 0, "data-plane HTTP exchange failed or timed out");
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
    close(fd);
    HttpResponse response;
    const std::string::size_type code_start = wire.find(' ');
    if (code_start != std::string::npos) {
        response.status = std::atoi(wire.c_str() + code_start + 1);
    }
    const std::string::size_type head_end = wire.find("\r\n\r\n");
    if (head_end != std::string::npos) {
        response.body = chunked ? chunked_body : wire.substr(head_end + 4);
    }
    return response;
}

// The data-plane equivalent of require(): a status+body assertion whose
// message carries both observed values.
void require_http(const HttpResponse& response, int status,
                  const std::string& body, const std::string& context) {
    require(response.status == status && response.body == body,
            context + " (status " + std::to_string(response.status) +
                ", body '" + response.body + "')");
}

std::string retire(const Fixture& fixture,
                   const std::string& application) {
    const std::string request =
        "POST /v1/apps/" + application +
        "/retire HTTP/1.1\r\nHost: local\r\nContent-Length: 0\r\n\r\n";
    json_t* response = parse_response(
        http_request(fixture.socket_path, request), 202);
    json_t* id = json_object_get(response, "operationId");
    require(json_is_string(id),
            "managed retire omitted its operation ID");
    const std::string operation = json_string_value(id);
    json_decref(response);
    return operation;
}

// The generation digest the deployed active.json references.
std::string active_generation(const Fixture& fixture) {
    json_t* root = json_loads(
        read_file(fixture.state_root + "/apps/orders/active.json").c_str(),
        JSON_REJECT_DUPLICATES, nullptr);
    require(root != nullptr && json_is_object(root),
            "cannot parse the active state document");
    json_t* generation = json_object_get(root, "generation");
    require(json_is_string(generation),
            "active state document has no generation");
    const std::string value = json_string_value(generation);
    json_decref(root);
    return value;
}

// Plants a crashed deploy's staging remnant under stateRoot/staging/<op>.
// Recovery must never treat it as an error, never clean it, and never let
// it become the active generation. complete=false models a crash while a
// staging file was being written (boundary #1: no COMPLETE); complete=true
// models a crash after COMPLETE but before the generation rename (boundary
// #2/#3: every staging file is visible, nothing was published).
void plant_staging_remnant(const Fixture& fixture, const std::string& op,
                           bool complete) {
    const std::string staging_root = fixture.state_root + "/staging";
    require(mkdir(staging_root.c_str(), 0700) == 0 || errno == EEXIST,
            "cannot ensure the staging root exists");
    const std::string dir = staging_root + "/" + op;
    require(mkdir(dir.c_str(), 0700) == 0,
            "cannot plant staging remnant directory");
    write_file(dir + "/bundle.bin", "half of a bundle");
    if (!complete) {
        return;
    }
    write_file(dir + "/source.bin", "source");
    write_file(dir + "/effective.json", "{}");
    write_file(dir + "/env.json", "[]");
    write_file(dir + "/capsid.json", "{}");
    write_file(dir + "/artifact.json", "{}");
    write_file(dir + "/generation.json", "{}");
    write_file(dir + "/COMPLETE", "");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 4,
            "expected mode, capsid-host and capsid-worker paths");
    const std::string mode = argv[1];
    Fixture fixture;
    if (mode == "host_managed_executable_enforces_queue_maximums") {
        // E-1 §10.3: host.json maximums.pool.queueRequests caps the App
        // queue; an App whose pool.queueRequests exceeds it must fail the
        // deploy (the zero-consumption field now has a consumer).
        fixture.replace_host_text(
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":10000}}",
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":10000},"
            "\"pool\":{\"queueRequests\":8,\"queueHeaderBytes\":\"1MiB\","
            "\"queueTimeout\":\"1s\"}}");
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);

        // A compliant queue deploys active.
        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1,"queueRequests":4,"queueHeaderBytes":"512KiB","queueTimeout":"250ms"}})json");
        const std::string compliant = deploy(fixture);
        require(wait_terminal_state(fixture, compliant) == "active",
                "queue within the Host maximums did not deploy active");

        // An over-ceiling queue fails the deploy.
        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1,"queueRequests":16,"queueHeaderBytes":"512KiB","queueTimeout":"250ms"}})json");
        const std::string overreach = deploy(fixture);
        require(wait_terminal_state(fixture, overreach) == "failed",
                "queue above the Host maximums deployed");
        require_active_app(fixture);
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_enforces_streaming_maximums") {
        // E-2 §9.3: host.json maximums.request.maxStreamingInflightPerWorker
        // caps the App streaming permit; an App whose request exceeds it
        // must fail the deploy (the zero-consumption field now has a
        // consumer).
        fixture.replace_host_text(
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":10000}}",
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":10000,"
            "\"maxStreamingInflightPerWorker\":2,"
            "\"streamIdleTimeoutMs\":120000}}");
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);

        // A compliant streaming config deploys active.
        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1},"request":{"maxStreamingInflightPerWorker":1,"streamIdleTimeoutMs":90000}})json");
        const std::string compliant = deploy(fixture);
        require(wait_terminal_state(fixture, compliant) == "active",
                "SSE permit within the Host maximums did not deploy active");

        // An over-ceiling streaming config fails the deploy.
        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1},"request":{"maxStreamingInflightPerWorker":4,"streamIdleTimeoutMs":90000}})json");
        const std::string overreach = deploy(fixture);
        require(wait_terminal_state(fixture, overreach) == "failed",
                "SSE permit above the Host maximums deployed");
        require_active_app(fixture);
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_enforces_write_timeout_maximum") {
        // E-3 §9.2: host.json maximums.request.writeTimeoutMs caps the App
        // slow-client write deadline; an App that exceeds it must fail the
        // deploy (same cap-only consumption as the E-2 streaming maximums).
        fixture.replace_host_text(
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":10000}}",
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":10000,"
            "\"writeTimeoutMs\":60000}}");
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);

        // A compliant write deadline deploys active.
        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1},"request":{"writeTimeoutMs":5000}})json");
        const std::string compliant = deploy(fixture);
        require(wait_terminal_state(fixture, compliant) == "active",
                "write deadline within the Host maximum did not deploy active");

        // An over-ceiling write deadline fails the deploy.
        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1},"request":{"writeTimeoutMs":300000}})json");
        const std::string overreach = deploy(fixture);
        require(wait_terminal_state(fixture, overreach) == "failed",
                "write deadline above the Host maximum deployed");
        require_active_app(fixture);
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_deploy_and_shutdown") {
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string operation = deploy(fixture);
        wait_active(fixture, operation);
        require_active_app(fixture);
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_stops_during_deploy") {
        // This fake worker never speaks the Runtime handshake. The Admin
        // submission is already accepted, so SIGTERM races a genuinely
        // running/queued coordinator operation. Shutdown must cancel or
        // otherwise bound that work; draining the executor by waiting for
        // the fake worker's 30 seconds is forbidden.
        start_host(fixture, argv[2], fixture.blocking_worker_path.c_str());
        wait_for_socket(fixture);
        (void)deploy(fixture);
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_active_state_validation_fail_closed") {
        // Release hardening: the durable active pointer is read through a
        // verified descriptor (O_NONBLOCK + fstat: regular file, owned by
        // the Host euid, bounded size). A directory, a foreign-owned file or
        // an oversized document at active.json must make recovery fail
        // closed — the Host refuses to start rather than parse unverified
        // bytes or silently drop a durable active pointer. Run once per
        // corruption class; each class must reject startup.
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string operation = deploy(fixture);
        wait_active(fixture, operation);
        stop_host(fixture);

        const std::string active_path =
            fixture.state_root + "/apps/orders/active.json";
        require(remove(active_path.c_str()) == 0,
                "cannot detach the active state document");

        // Corruption class 1: a directory at active.json.
        require(mkdir(active_path.c_str(), 0700) == 0,
                "cannot plant a directory at active.json");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(5));

        // Corruption class 2: an oversized active document (> 16 KiB).
        require(rmdir(active_path.c_str()) == 0,
                "cannot remove the directory active-state fixture");
        write_file(active_path, std::string(17U * 1024U, 'x'));
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(5));

        // Corruption class 3: a foreign-owned active document. The Host
        // (running as euid) must reject a file it does not own even when
        // the contents are parseable.
        require(remove(active_path.c_str()) == 0,
                "cannot detach the oversized active-state fixture");
        write_file(active_path, "{}");
        require(chown(active_path.c_str(), 65534, 65534) == 0,
                "cannot chown active.json to a foreign uid");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(5));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_recovers_on_restart") {
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string operation = deploy(fixture);
        wait_active(fixture, operation);
        stop_host(fixture);

        // The durable active pointer, not an upload-directory scan or a
        // second deploy, is the sole restart authority. Admin readiness is
        // published only after that generation has been revalidated and a
        // replacement worker has reached READY.
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        require_active_app(fixture);
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_crash_mid_deploy_keeps_old") {
        // M2 item 2a, boundary #7 (post version-mapping, pre active.json) —
        // a REAL SIGKILL, no clean-shutdown path. The v2 deploy must hang
        // in the worker-warm phase: by then the generation rename (Phase C)
        // and the version mapping (Phase D) have both hit the disk, but
        // persist_active_state (Phase F) has not run yet. The old active.json
        // is then the only authority; restart must keep v1 active and
        // serving, and the v2 generation + mapping must survive as inert
        // orphans (recovery never half-clears a published generation).
        //
        // A plain blocking worker cannot serve here: once v1 is durably
        // active, boot recovery waits for the replacement worker's READY
        // BEFORE publishing Admin readiness, so a host started with a
        // blocking worker never binds Admin (main.cc, "replacement worker
        // reaches READY before Admin readiness is published"). The warm
        // phase is instead hung with a self-forwarding worker script: the
        // first spawn (v1 boot recovery) execs the real worker so Admin
        // binds normally; the second spawn (v2 deploy warm) execs
        // /bin/sleep 30 and never emits READY.
        fixture.create_version("orders", "v2");
        replace_file(
            fixture.applications_root + "/orders/v2/bundle.mjs",
            "export default { fetch: () => new Response('v2-different') };");
        const std::string warm_hang_worker =
            fixture.root + "/warm-hang-worker";
        const std::string warm_counter = fixture.root + "/warm-counter";
        write_file(warm_hang_worker,
                   "#!/bin/sh\n"
                   "if [ -f " + warm_counter + " ]; then\n"
                   "  exec /bin/sleep 30\n"
                   "fi\n"
                   ": > " + warm_counter + "\n"
                   "exec " + argv[3] + " \"$@\"\n");
        require(chmod(warm_hang_worker.c_str(), 0700) == 0,
                "cannot make warm-hang worker fixture executable");

        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string first = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, first) == "active",
                "initial v1 deploy failed before the crash matrix");
        stop_host(fixture);
        const std::string g1 = active_generation(fixture);

        start_host(fixture, argv[2], warm_hang_worker.c_str());
        wait_for_socket(fixture);
        (void)deploy(fixture, "orders", "v2");
        // The audit anchor for the crash point: wait until the v2
        // generation AND its version mapping are both visible on disk
        // (Phases C and D done, i.e. every durable trace of the new
        // generation except active.json), while the warm phase is still
        // hung on /bin/sleep (Phase F not started → active.json is still
        // the old v1 document), then SIGKILL.
        const std::string generations = fixture.state_root +
                                        "/apps/orders/generations";
        const std::string mapping = fixture.state_root +
                                    "/apps/orders/versions/v2.json";
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        bool published = false;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(generations)) {
                if (entry.path().filename().string() != g1) {
                    published = true;
                    break;
                }
            }
            if (published && path_exists(mapping)) {
                break;
            }
            published = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(published,
                "v2 generation and mapping never reached the disk before "
                "the crash");
        kill_host();
        // SIGKILL skips the clean-shutdown unlink, so the Admin socket
        // inode survives as crash evidence. The next boot removes it under
        // the three-evidence rule before rebinding (admin_api.cc), but the
        // fixture must not race that: wait_for_socket() only lstat's, so
        // the stale pathname would pass it before the new Host binds.
        require(path_exists(fixture.socket_path),
                "SIGKILL must leave the Admin socket in place");
        require(unlink(fixture.socket_path.c_str()) == 0,
                "cannot clear the crashed Host's stale Admin socket");

        // Restart: v1 must come back active and serving; the v2 generation
        // and its version mapping must still be on disk (inert orphans).
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        require_active_app(fixture, "orders", "v1");
        require(path_exists(fixture.state_root +
                            "/apps/orders/versions/v2.json"),
                "recovery cleaned a published version mapping");
        std::size_t generations_on_disk = 0;
        for (const auto& entry :
             std::filesystem::directory_iterator(generations)) {
            (void)entry;
            ++generations_on_disk;
        }
        require(generations_on_disk == 2,
                "recovery did not preserve the published orphan generation");
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_crash_staging_remnants") {
        // M2 item 2a, boundaries #1 and #2/#3 (staging phase): a crashed
        // deploy leaves stateRoot/staging/<op>/ behind. Restart must
        // succeed, keep the old version active, never treat the remnant as
        // a candidate generation, and never clean it (the COMPLETE/generation
        // boundary must not full-clean: a staged-but-unpublished tree is
        // still potential history, not garbage).
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string operation = deploy(fixture);
        require(wait_terminal_state(fixture, operation) == "active",
                "initial deploy failed before the staging crash matrix");
        stop_host(fixture);

        plant_staging_remnant(fixture, "op-crash-partial", false);
        plant_staging_remnant(fixture, "op-crash-complete", true);

        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        require_active_app(fixture);
        require(path_exists(fixture.state_root +
                            "/staging/op-crash-partial/bundle.bin"),
                "recovery cleaned a partial staging remnant");
        require(path_exists(fixture.state_root +
                            "/staging/op-crash-complete/COMPLETE"),
                "recovery cleaned a COMPLETE-marked staging remnant");
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_crash_orphan_generation") {
        // M2 item 2a, boundaries #4-#7 (published generation, old active):
        // a crash after the generation rename but before active.json
        // covers both the visible-but-unsynced generation (an fsync crash
        // leaves the same visible tree) and the committed generation with a
        // version mapping but no active flip. The old active.json is the
        // only authority: restart must serve v1 and leave the orphan
        // generation + mapping inert on disk.
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string operation = deploy(fixture);
        require(wait_terminal_state(fixture, operation) == "active",
                "initial deploy failed before the orphan matrix");
        stop_host(fixture);
        const std::string g1 = active_generation(fixture);

        const std::string g2 = "sha256:" + std::string(64, '2');
        const std::string base = fixture.state_root +
                                 "/apps/orders/generations/";
        std::error_code copy_error;
        std::filesystem::copy(base + g1, base + g2,
                              std::filesystem::copy_options::recursive,
                              copy_error);
        require(!copy_error, "cannot plant orphan generation: " +
                                 copy_error.message());
        write_file(fixture.state_root + "/apps/orders/versions/v2.json",
                   "{\"generation\":\"" + g2 + "\"}\n");

        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        require_active_app(fixture, "orders", "v1");
        require(path_exists(base + g2 + "/COMPLETE"),
                "recovery removed a published orphan generation");
        require(path_exists(fixture.state_root +
                            "/apps/orders/versions/v2.json"),
                "recovery removed an orphan version mapping");
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_crash_quarantined_not_resurrected") {
        // M2 item 2a: a quarantined tombstone (the item-5 QUARANTINING
        // protocol's persisted state) must survive a restart — the Host
        // must not resurrect a worker for it, must keep the document and
        // its reason, and must still boot.
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string operation = deploy(fixture);
        require(wait_terminal_state(fixture, operation) == "active",
                "initial deploy failed before the quarantine matrix");
        stop_host(fixture);
        const std::string g1 = active_generation(fixture);

        replace_file(
            fixture.state_root + "/apps/orders/active.json",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"quarantined\",\"version\":\"v1\","
            "\"generation\":\"" + g1 + "\","
            "\"reason\":\"CRASH_BUDGET_EXCEEDED\"}");

        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        json_t* apps = parse_response(
            http_request(fixture.socket_path,
                         "GET /v1/apps/orders HTTP/1.1\r\nHost: local\r\n\r\n"),
            200);
        require(json_is_false(json_object_get(apps, "active")),
                "quarantined App resurrected its worker on restart");
        json_decref(apps);
        json_t* state = json_loads(
            read_file(fixture.state_root + "/apps/orders/active.json").c_str(),
            JSON_REJECT_DUPLICATES, nullptr);
        require(state != nullptr &&
                    json_is_string(json_object_get(state, "state")) &&
                    std::string(json_string_value(
                        json_object_get(state, "state"))) == "quarantined" &&
                    json_is_string(json_object_get(state, "reason")) &&
                    std::string(json_string_value(
                        json_object_get(state, "reason"))) ==
                        "CRASH_BUDGET_EXCEEDED",
                "restart rewrote or lost the quarantined tombstone");
        json_decref(state);
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_host_config_fifo") {
        require(unlink(fixture.host_config.c_str()) == 0 &&
                    mkfifo(fixture.host_config.c_str(), 0600) == 0,
                "cannot create host.json FIFO fixture");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_host_config_symlink") {
        const std::string target = fixture.root + "/host.real.json";
        require(rename(fixture.host_config.c_str(), target.c_str()) == 0 &&
                    symlink(target.c_str(), fixture.host_config.c_str()) == 0,
                "cannot create host.json symlink fixture");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_embedded_nul_path") {
        fixture.replace_host_text(
            fixture.socket_path,
            fixture.socket_path + "\\u0000ignored");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_ambiguous_secret_template") {
        fixture.replace_host_text("/{application}\"",
                                  "/{application}/ignored\"");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_unsafe_admin_mode") {
        // host-v1 currently has no management-group field. Consequently a
        // group/world-accessible pathname cannot be authorized coherently;
        // managed mode must keep the production Admin socket at exact 0600.
        fixture.replace_host_text("\"mode\":\"0600\"",
                                  "\"mode\":\"0666\"");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_negative_workers_total") {
        // Release hardening: a negative capacity.workersTotal must fail
        // startup (the unsigned cast would otherwise turn -1 into
        // UINT64_MAX and grant absurd capacity).
        fixture.replace_host_text("\"workersTotal\":1",
                                  "\"workersTotal\":-1");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_oversized_workers_total") {
        // Release hardening: capacity.workersTotal beyond INT_MAX must fail
        // startup (the value is narrowed to int for the capacity permit;
        // without the ceiling the narrow wraps negative).
        fixture.replace_host_text("\"workersTotal\":1",
                                  "\"workersTotal\":2147483648");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_negative_max_inflight") {
        // Release hardening: a negative maximums.request.maxInflightPerWorker
        // must fail startup (the unsigned cast would otherwise accept it).
        fixture.replace_host_text(
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":10000}}",
            "\"maximums\":{\"request\":{\"maxInflightPerWorker\":-5}}");
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_rejects_zero_or_negative_pool_bounds") {
        // Release hardening: capsid.json pool bounds below 1 must fail the
        // deploy, never spin up a zero/negative worker pool or overflow into
        // the uint32 cast.
        fixture.create_version("orders", "v2");
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);

        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":0,"maxWorkers":1}})json");
        const std::string zero_min = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, zero_min) == "failed",
                "pool.minReady=0 deployed active");

        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":-2,"maxWorkers":1}})json");
        const std::string negative_min = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, negative_min) == "failed",
                "pool.minReady=-2 deployed active");

        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":0}})json");
        const std::string zero_max = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, zero_max) == "failed",
                "pool.maxWorkers=0 deployed active");

        replace_file(
            fixture.applications_root + "/orders/v1/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":-3}})json");
        const std::string negative_max = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, negative_max) == "failed",
                "pool.maxWorkers=-3 deployed active");

        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_enforces_global_worker_capacity") {
        fixture.create_application("payments");
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string first = deploy(fixture, "orders");
        require(wait_terminal_state(fixture, first) == "active",
                "first App did not consume the one-worker Host capacity");
        const std::string second = deploy(fixture, "payments");
        require(wait_terminal_state(fixture, second) == "failed",
                "Host activated more workers than capacity.workersTotal");
        require_active_app(fixture);
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_redeploys_with_capacity_one") {
        // §9.4: workersTotal=1 is the steady budget; a zero-downtime
        // replacement overlaps the old and new pools, so the fixture
        // grants one surge slot.
        fixture.replace_host_text(
            "\"capacity\":{\"workersTotal\":1,\"startupsConcurrent\":1}",
            "\"capacity\":{\"workersTotal\":1,\"activationSurgeWorkers\":1,"
            "\"startupsConcurrent\":1}");
        fixture.create_version("orders", "v2");
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string first = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, first) == "active",
                "initial deploy did not consume the App's worker slot");
        const std::string replacement = deploy(fixture, "orders", "v2");
        require(wait_terminal_state(fixture, replacement) == "active",
                "capacity one prevented replacement of its existing App");
        require_active_app(fixture, "orders", "v2");
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_executable_recovery_consumes_capacity") {
        fixture.create_application("payments");
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string first = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, first) == "active",
                "initial deploy did not become active before restart");
        stop_host(fixture);

        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        require_active_app(fixture, "orders", "v1");
        const std::string second = deploy(fixture, "payments", "v1");
        require(wait_terminal_state(fixture, second) == "failed",
                "recovered worker did not consume global Host capacity");
        require_active_app(fixture, "orders", "v1");
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_http_e2e_multi_app") {
        // §9.6 E2E through the real TCP data plane: listener-routed deploy,
        // multi-App isolation (item 7), in-place replacement (item 4), and
        // the retire tombstone — new requests get 404 (item 6) while the
        // surviving App keeps serving.
        const int port = pick_port();
        add_public_listener(fixture, port);
        // Two Apps each hold a 1-worker pool; the fixture's default
        // workersTotal of 1 must grow to fit both (§9.6-7). §9.4: the
        // in-place replacement below overlaps the old and new generations,
        // so one surge slot is granted.
        fixture.replace_host_text(
            "\"capacity\":{\"workersTotal\":1,\"startupsConcurrent\":1}",
            "\"capacity\":{\"workersTotal\":4,\"activationSurgeWorkers\":1,"
            "\"startupsConcurrent\":1}");
        fixture.create_application("payments", "payments-executable-ok");
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);

        const std::string first = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, first) == "active",
                "the E2E App did not reach active state");
        require_http(http_exchange_tcp(port, "GET /@capsid/orders/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     200, "managed-executable-ok",
                     "a configured listener did not reach the deployed App "
                     "(§9.6-1)");

        // A path that never names an App stays the router's 404.
        require_http(http_exchange_tcp(port, "GET /admin HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     404, "path routing requires the /@capsid/{app} prefix",
                     "a non-App path did not map to 404");

        // Second App: real multi-App routing without cross-App bleed (§9.6-7).
        const std::string second = deploy(fixture, "payments", "v1");
        require(wait_terminal_state(fixture, second) == "active",
                "the second App did not reach active state");
        require_http(http_exchange_tcp(port, "GET /@capsid/payments/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     200, "payments-executable-ok",
                     "the payments App did not serve its own response");
        require_http(http_exchange_tcp(port, "GET /@capsid/orders/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     200, "managed-executable-ok",
                     "the payments deploy bled into orders (§9.6-7)");

        // In-place replacement: new requests only reach the new generation
        // (§9.6-4); the other App is untouched.
        fixture.create_version("orders", "v2", "orders-v2-ok");
        const std::string replacement = deploy(fixture, "orders", "v2");
        require(wait_terminal_state(fixture, replacement) == "active",
                "the orders replacement did not reach active state");
        require_http(http_exchange_tcp(port, "GET /@capsid/orders/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     200, "orders-v2-ok",
                     "new requests did not move to the replacement "
                     "generation (§9.6-4)");
        require_http(http_exchange_tcp(port, "GET /@capsid/payments/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     200, "payments-executable-ok",
                     "the orders replacement bled into payments");

        // Retire: the route becomes a tombstone — new requests are 404
        // (§9.6-6), while the live App keeps serving.
        const std::string retired = retire(fixture, "payments");
        require(wait_terminal_state(fixture, retired) == "active",
                "the payments retire did not complete");
        require_http(http_exchange_tcp(port, "GET /@capsid/payments/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     404, "app retired",
                     "a retired App did not map to 404 (§9.6-6)");
        require_http(http_exchange_tcp(port, "GET /@capsid/orders/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     200, "orders-v2-ok",
                     "the retire bled into a live App");

        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_http_restart_recovers_route") {
        // §9.6-3: after a restart the recovered generation serves over the
        // real listener again — the durable active document, not the
        // upload, is the recovery source.
        const int port = pick_port();
        add_public_listener(fixture, port);
        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        const std::string first = deploy(fixture, "orders", "v1");
        require(wait_terminal_state(fixture, first) == "active",
                "the restart E2E App did not reach active state");
        require_http(http_exchange_tcp(port, "GET /@capsid/orders/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     200, "managed-executable-ok",
                     "the pre-restart listener did not serve the App");
        stop_host(fixture);

        start_host(fixture, argv[2], argv[3]);
        wait_for_socket(fixture);
        require_active_app(fixture, "orders", "v1");
        require_http(http_exchange_tcp(port, "GET /@capsid/orders/ HTTP/1.1\r\n"
                                          "Host: localhost\r\n"
                                          "Connection: close\r\n\r\n"),
                     200, "managed-executable-ok",
                     "the recovered generation did not serve over the "
                     "listener (§9.6-3)");
        stop_host(fixture);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_http_rejects_multiple_listeners") {
        // §9.2 all-or-fail / v1 single-consumer event sink: two configured
        // listeners fail startup closed instead of serving with a broken
        // response fan-out.
        const int port = pick_port();
        add_public_listener(fixture, port, 2);
        start_host(fixture, argv[2], argv[3]);
        require_startup_rejected(fixture, std::chrono::seconds(1));
        std::cout << "PASS" << std::endl;
        return 0;
    }

    fail("unknown managed executable test mode");
}
