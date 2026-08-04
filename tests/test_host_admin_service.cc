// Frozen M1D long-lived Admin service RED suite.
//
// The one-connection transport is deliberately not a service lifecycle.
// This suite freezes the owning layer: bind before start returns, serve
// multiple connections, interrupt both an idle accept and an accepted slow
// client, join within a bounded time, and remove only the socket inode that
// the service itself created.

#if __has_include("host/admin_service.h")
#include "host/admin_service.h"
#define CAPSID_HAS_ADMIN_SERVICE 1
#else
#define CAPSID_HAS_ADMIN_SERVICE 0
#endif

#include "host/admin_api.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

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

#if CAPSID_HAS_ADMIN_SERVICE

class CountingBackend final : public capsid::host::AdminBackend {
public:
    capsid::host::DeployOutcome deploy(
        const std::string& application,
        const std::string& version,
        capsid::host::OperationStatus* status) override {
        const int call = deploy_calls.fetch_add(1) + 1;
        capsid::host::DeployOutcome outcome;
        outcome.ok = true;
        outcome.operation_id = "op-service-" + std::to_string(call);
        status->operation_id = outcome.operation_id;
        status->version = version;
        status->state = capsid::host::OperationState::kWarming;
        last_application = application;
        return outcome;
    }

    capsid::host::DeployOutcome retire(
        const std::string&, capsid::host::OperationStatus*) override {
        return {};
    }

    capsid::host::OperationStatus operation_status(
        const std::string&) override {
        return {};
    }

    std::string app_status(const std::string&) override {
        return "{\"active\":false}";
    }

    std::atomic<int> deploy_calls{0};
    std::string last_application;
};

struct ServiceFixture {
    std::string directory = "/tmp/capsid-admin-service-XXXXXX";
    std::string path;
    CountingBackend backend;
    capsid::host::AdminServiceOptions options;
    capsid::host::AdminService service;
    bool joined = false;

    ServiceFixture()
        : path(),
          options(),
          service(make_options(), &backend) {
        // make_options() creates the directory and stores path/options
        // before the service copies them.
    }

    ~ServiceFixture() {
        if (!joined) {
            service.request_stop();
            std::string ignored;
            (void)service.wait(&ignored);
        }
        (void)unlink(path.c_str());
        (void)rmdir(directory.c_str());
    }

    capsid::host::AdminServiceOptions make_options() {
        require(mkdtemp(directory.data()) != nullptr,
                "cannot create Admin service fixture directory");
        path = directory + "/admin.sock";
        options.socket.path = path;
        options.socket.mode = 0600;
        options.socket.backlog = 8;
        options.http.api.authorization.allowed_uid =
            static_cast<std::uint64_t>(geteuid());
        options.http.api.max_header_bytes = 1024;
        options.http.api.max_body_bytes = 1024;
        options.http.header_timeout_ms = 5000;
        options.http.body_timeout_ms = 5000;
        options.http.write_timeout_ms = 1000;
        return options;
    }

    void start() {
        std::string error;
        require(service.start(&error),
                "cannot start Admin service: " + error);
        struct stat metadata = {};
        require(lstat(path.c_str(), &metadata) == 0 &&
                    S_ISSOCK(metadata.st_mode),
                "Admin service start returned before its socket was bound");
    }

    void stop_and_wait() {
        service.request_stop();
        std::string error;
        require(service.wait(&error),
                "Admin service did not stop cleanly: " + error);
        joined = true;
    }
};

int connect_client(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "cannot create Admin service client");
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    require(path.size() < sizeof(address.sun_path),
            "Admin service fixture path is too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    require(connect(fd, reinterpret_cast<const struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect to Admin service");
    const struct timeval timeout = {2, 0};
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
            "cannot bound Admin service client receive");
    return fd;
}

void write_all(int fd, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = send(fd, bytes.data() + offset,
                                   bytes.size() - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0, "cannot write Admin service request");
        offset += static_cast<std::size_t>(count);
    }
}

std::string read_to_close(int fd) {
    std::string response;
    char buffer[1024];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fail("Admin service response timed out");
        }
        require(count >= 0, "cannot read Admin service response");
        if (count == 0) {
            return response;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
}

std::string deploy_request(const std::string& version) {
    const std::string body =
        "{\"app\":\"orders\",\"version\":\"" + version + "\"}";
    return "POST /v1/deploy HTTP/1.1\r\nHost: local\r\n"
           "Content-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

void round_trip(const std::string& path, const std::string& request) {
    const int client = connect_client(path);
    write_all(client, request);
    require(shutdown(client, SHUT_WR) == 0,
            "cannot finish Admin service request");
    const std::string response = read_to_close(client);
    close(client);
    require(response.rfind("HTTP/1.1 202 ", 0) == 0,
            "Admin service returned an unexpected response: " + response);
}

#endif

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected one Admin service test mode");
    const std::string mode = argv[1];

#if !CAPSID_HAS_ADMIN_SERVICE
    fail("long-lived Admin service is not implemented: " + mode);
#else
    if (mode == "host_admin_service_multiple_connections") {
        ServiceFixture fixture;
        fixture.start();
        round_trip(fixture.path, deploy_request("v1"));
        round_trip(fixture.path, deploy_request("v2"));
        require(fixture.backend.deploy_calls.load() == 2,
                "Admin service stopped after its first connection");
        fixture.stop_and_wait();
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_service_idle_stop") {
        ServiceFixture fixture;
        fixture.start();
        const auto started = std::chrono::steady_clock::now();
        fixture.stop_and_wait();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require(elapsed < std::chrono::seconds(1),
                "Admin service did not interrupt its blocked accept");
        struct stat metadata = {};
        require(lstat(fixture.path.c_str(), &metadata) != 0 && errno == ENOENT,
                "Admin service left its socket pathname after stop");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_service_slow_client_stop") {
        ServiceFixture fixture;
        fixture.start();
        const int client = connect_client(fixture.path);
        write_all(client, "POST /v1/deploy HTTP/1.1\r\nHost: local\r\n");
        // Give the service time to leave accept and block on this partial
        // header. Stop must interrupt that accepted fd, not wait for the
        // five-second HTTP header deadline.
        usleep(50U * 1000U);
        const auto started = std::chrono::steady_clock::now();
        fixture.stop_and_wait();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        close(client);
        require(elapsed < std::chrono::seconds(1),
                "Admin stop waited for a slow client's HTTP deadline");
        require(fixture.backend.deploy_calls.load() == 0,
                "partial Admin request reached the backend during stop");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_service_preserves_replaced_path") {
        ServiceFixture fixture;
        fixture.start();
        require(unlink(fixture.path.c_str()) == 0,
                "cannot detach Admin service socket pathname");
        const int replacement = open(fixture.path.c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                     0600);
        require(replacement >= 0,
                "cannot install replacement Admin pathname");
        require(close(replacement) == 0,
                "cannot close replacement Admin pathname");
        fixture.stop_and_wait();
        struct stat metadata = {};
        require(lstat(fixture.path.c_str(), &metadata) == 0 &&
                    S_ISREG(metadata.st_mode),
                "Admin shutdown unlinked a pathname it did not create");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_service_stop_burst_is_nonblocking") {
        // request_stop is callable from more than one shutdown edge
        // (signal adapter, owner and destructor). It must be idempotent and
        // bounded. A blocking pipe write per call eventually fills after the
        // accept loop has consumed only the first wake byte.
        const pid_t child = fork();
        require(child >= 0, "cannot fork Admin stop-burst probe");
        if (child == 0) {
            ServiceFixture fixture;
            fixture.start();
            for (std::size_t index = 0; index < 100000; ++index) {
                fixture.service.request_stop();
            }
            fixture.stop_and_wait();
            _exit(0);
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(1);
        int status = 0;
        for (;;) {
            const pid_t waited = waitpid(child, &status, WNOHANG);
            require(waited >= 0, "cannot wait for Admin stop-burst probe");
            if (waited == child) {
                require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                        "Admin stop-burst probe failed");
                std::cout << "PASS" << std::endl;
                return 0;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                (void)kill(child, SIGKILL);
                (void)waitpid(child, &status, 0);
                fail("repeated Admin request_stop calls blocked on wakeup I/O");
            }
            usleep(1000);
        }
    }

    fail("unknown Admin service test mode: " + mode);
#endif
}
