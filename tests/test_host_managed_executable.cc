// Frozen M1D managed executable RED suite.
//
// This is intentionally a process test, not another direct coordinator
// call. It proves that capsid-host has a production managed mode independent
// of the benchmark-only single-worker fixture: host.json is validated and
// compiled, the long-lived Unix Admin service exposes a real source deploy,
// the warmed worker remains owned by the process, and SIGTERM closes the
// control plane and reaps that worker within a bounded time.

#include <jansson.h>

#include <fcntl.h>
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

    void create_application(const std::string& application) {
        const std::string app = applications_root + "/" + application;
        require(mkdir(app.c_str(), 0700) == 0,
                "cannot create managed executable App fixture");
        create_version(application, "v1");
    }

    void create_version(const std::string& application,
                        const std::string& version_id) {
        const std::string version = applications_root + "/" + application +
                                    "/" + version_id;
        require(mkdir(version.c_str(), 0700) == 0,
                "cannot create managed executable Version fixture");
        write_file(
            version + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1}})json");
        write_file(
            version + "/bundle.mjs",
            "export default { fetch: () => new Response('managed-executable-ok') };");
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
        struct stat metadata = {};
        if (lstat(fixture.socket_path.c_str(), &metadata) == 0 &&
            S_ISSOCK(metadata.st_mode)) {
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

    fail("unknown managed executable test mode");
}
