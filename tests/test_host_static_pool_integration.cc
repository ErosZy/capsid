// Frozen M2 Batch C RED: prove pool-wide activation and Linux process-fault
// isolation through the public HTTP surface. No queue/P2C/SSE policy here.

#include "host/static_pool_server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
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

std::uint16_t reserve_test_port() {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "cannot create integration port socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode integration loopback address");
    require(bind(fd, reinterpret_cast<struct sockaddr*>(&address),
                 sizeof(address)) == 0,
            "cannot reserve integration port");
    socklen_t length = sizeof(address);
    require(getsockname(fd, reinterpret_cast<struct sockaddr*>(&address),
                        &length) == 0,
            "cannot inspect integration port");
    const std::uint16_t port = ntohs(address.sin_port);
    close(fd);
    return port;
}

bool http_succeeds(std::uint16_t port, int timeout_ms) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                sizeof(address)) != 0) {
        close(fd);
        return false;
    }
    const std::string request =
        "GET /@capsid/orders/pool-integration HTTP/1.1\r\n"
        "Host: public.example\r\n"
        "Connection: close\r\n\r\n";
    if (send(fd, request.data(), request.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(request.size())) {
        close(fd);
        return false;
    }
    std::string response;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        struct pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = poll(
            &descriptor, 1,
            static_cast<int>(remaining.count() > 0 ? remaining.count() : 1));
        if (polled <= 0) {
            break;
        }
        char bytes[2048];
        const ssize_t count = recv(fd, bytes, sizeof(bytes), 0);
        if (count <= 0) {
            break;
        }
        response.append(bytes, static_cast<std::size_t>(count));
    }
    close(fd);
    return response.find(" 200 ") != std::string::npos &&
           response.find("pool-integration-ok") != std::string::npos;
}

capsid::host::StaticPoolServerOptions make_options(
    const std::string& worker_path, int ready_fd, std::uint16_t port) {
    capsid::host::StaticPoolServerOptions options;
    options.workers = 3;
    options.worker_options.worker_path = worker_path;
    options.worker_options.source_bundle_path = "pool-integration-inline";
    options.worker_options.source_name = "file://orders/v1/bundle.mjs";
    options.worker_options.application = "orders";
    options.worker_options.listen_address = "127.0.0.1";
    options.worker_options.listen_port = port;
    options.worker_options.public_scheme = "http";
    options.worker_options.public_authority = "public.example";
    options.worker_options.request_timeout_ms = 5000;
    options.worker_options.initial_stream_window = 64U * 1024U;
    options.worker_options.strict_sandbox = false;
    options.worker_options.ready_fd = ready_fd;
    return options;
}

const std::vector<std::uint8_t>& fixture_bundle() {
    static const std::string source =
        "export default { fetch: () => new Response('pool-integration-ok') };";
    static const std::vector<std::uint8_t> bundle(source.begin(), source.end());
    return bundle;
}

void write_all(int fd, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count =
            write(fd, bytes.data() + offset, bytes.size() - offset);
        require(count > 0, "cannot write delayed worker wrapper");
        offset += static_cast<std::size_t>(count);
    }
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char byte : value) {
        if (byte == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(byte);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

void test_activation_barrier(const char* real_worker_path) {
    char directory_template[] = "/tmp/capsid-pool-barrier-XXXXXX";
    char* directory_bytes = mkdtemp(directory_template);
    require(directory_bytes != nullptr, "cannot create activation fixture");
    const std::string directory(directory_bytes);
    const std::string wrapper = directory + "/worker-wrapper";
    const std::string first = directory + "/first";
    const std::string second = directory + "/second";
    const std::string delayed = directory + "/second-delayed";
    const std::string script =
        "#!/bin/sh\n"
        "if mkdir " + shell_quote(first) + " 2>/dev/null; then\n"
        "  :\n"
        "elif mkdir " + shell_quote(second) + " 2>/dev/null; then\n"
        "  : > " + shell_quote(delayed) + "\n"
        "  sleep 1\n"
        "fi\n"
        "exec " + shell_quote(real_worker_path) + " \"$@\"\n";
    const int wrapper_fd =
        open(wrapper.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    require(wrapper_fd >= 0, "cannot create delayed worker wrapper");
    write_all(wrapper_fd, script);
    require(close(wrapper_fd) == 0, "cannot close delayed worker wrapper");
    require(chmod(wrapper.c_str(), 0700) == 0,
            "cannot make delayed worker wrapper executable");

    int ready[2];
    require(pipe(ready) == 0, "cannot create activation READY pipe");
    const std::uint16_t port = reserve_test_port();
    capsid::host::StaticPoolServer pool(make_options(wrapper, ready[1], port));
    bool started = false;
    std::string error;
    std::thread starter([&] { started = pool.start(fixture_bundle(), &error); });
    const auto delayed_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (access(delayed.c_str(), F_OK) != 0 &&
           std::chrono::steady_clock::now() < delayed_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(access(delayed.c_str(), F_OK) == 0,
            "second shard never entered delayed READY fixture");
    require(!http_succeeds(port, 200),
            "an early shard served HTTP before the whole pool was READY");
    starter.join();
    require(started, "activation-barrier pool failed to start: " + error);
    require(http_succeeds(port, 1000),
            "fully READY pool did not begin serving HTTP");
    pool.request_stop();
    require(pool.wait(&error), "activation-barrier pool did not stop");
    close(ready[0]);
    close(ready[1]);
    unlink(delayed.c_str());
    rmdir(second.c_str());
    rmdir(first.c_str());
    unlink(wrapper.c_str());
    rmdir(directory.c_str());
}

#if defined(__linux__)
std::string canonical_path(const char* path) {
    char resolved[PATH_MAX];
    require(realpath(path, resolved) != nullptr,
            "cannot resolve capsid-worker path");
    return resolved;
}

std::set<pid_t> matching_worker_children(const std::string& worker_path) {
    std::set<pid_t> matches;
    DIR* tasks = opendir("/proc/self/task");
    require(tasks != nullptr, "cannot inspect process tasks");
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(tasks);
        if (entry == nullptr) {
            require(errno == 0, "cannot enumerate process tasks");
            break;
        }
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        std::ifstream children(std::string("/proc/self/task/") +
                               entry->d_name + "/children");
        long long child = 0;
        while (children >> child) {
            char executable[PATH_MAX];
            const std::string link =
                "/proc/" + std::to_string(child) + "/exe";
            const ssize_t size =
                readlink(link.c_str(), executable, sizeof(executable) - 1);
            if (size <= 0) {
                continue;
            }
            executable[size] = '\0';
            if (worker_path == executable) {
                matches.insert(static_cast<pid_t>(child));
            }
        }
    }
    closedir(tasks);
    return matches;
}

void test_worker_exit_isolation(const char* worker_path_bytes) {
    const std::string worker_path = canonical_path(worker_path_bytes);
    int ready[2];
    require(pipe(ready) == 0, "cannot create isolation READY pipe");
    const std::uint16_t port = reserve_test_port();
    capsid::host::StaticPoolServer pool(
        make_options(worker_path, ready[1], port));
    std::string error;
    require(pool.start(fixture_bundle(), &error),
            "cannot start fault-isolation pool: " + error);
    std::set<pid_t> children;
    const auto child_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (children.size() != 3 &&
           std::chrono::steady_clock::now() < child_deadline) {
        children = matching_worker_children(worker_path);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(children.size() == 3,
            "three-shard pool did not own three distinct worker processes");
    for (int request = 0; request < 48; ++request) {
        require(http_succeeds(port, 1000),
                "healthy multi-process pool failed an HTTP request");
    }

    auto victim = children.begin();
    ++victim;
    require(kill(*victim, SIGKILL) == 0,
            "cannot kill one worker isolation fixture");
    const auto exit_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (kill(*victim, 0) == 0 &&
           std::chrono::steady_clock::now() < exit_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(kill(*victim, 0) != 0 && errno == ESRCH,
            "dead shard worker was not reaped");
    // Reaping happens on the worker owner thread; allow the already-queued
    // exit event one short reactor turn to remove that shard's listener.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(pool.active_workers() == 2,
            "worker exit did not reduce the observable pool capacity to N-1");
    for (int request = 0; request < 48; ++request) {
        require(http_succeeds(port, 1000),
                "dead shard kept receiving traffic or stopped healthy shards");
    }
    pool.request_stop();
    require(pool.wait(&error), "fault-isolation pool did not stop");
    close(ready[0]);
    close(ready[1]);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    require(argc == 3, "expected mode and capsid-worker path");
    const std::string mode = argv[1];
    if (mode == "activation-barrier") {
        test_activation_barrier(argv[2]);
    } else if (mode == "worker-exit") {
#if defined(__linux__)
        test_worker_exit_isolation(argv[2]);
#else
        std::cout << "SKIP: Linux /proc process evidence is unavailable"
                  << std::endl;
#endif
    } else {
        fail("unknown static-pool integration mode");
    }
    std::cout << "PASS" << std::endl;
    return 0;
}
