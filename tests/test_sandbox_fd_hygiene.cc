#include "capsid/runtime.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

bool path_exists(const std::string &path) {
    struct stat info;
    return lstat(path.c_str(), &info) == 0;
}

std::string symlink_target(const std::string &path) {
    char target[256];
    const ssize_t size =
        readlink(path.c_str(), target, sizeof(target) - 1);
    if (size < 0) {
        return std::string();
    }
    target[size] = '\0';
    return std::string(target);
}

bool worker_environment_is_empty(int64_t pid) {
    std::ifstream environment(
        (std::string("/proc/") + std::to_string(pid) + "/environ").c_str(),
        std::ios::in | std::ios::binary);
    return environment.peek() == std::ifstream::traits_type::eof();
}

bool kernel_sandbox_is_active(int64_t pid) {
    std::ifstream status(
        (std::string("/proc/") + std::to_string(pid) + "/status").c_str());
    std::string line;
    bool no_new_privs = false;
    bool seccomp_filter = false;
    while (std::getline(status, line)) {
        if (line.find("NoNewPrivs:") == 0 &&
            line.find('1') != std::string::npos) {
            no_new_privs = true;
        }
        if (line.find("Seccomp:") == 0 &&
            line.find('2') != std::string::npos) {
            seccomp_filter = true;
        }
    }
    return no_new_privs && seccomp_filter;
}

bool worker_open_file_limit_is(int64_t pid, uint64_t expected) {
    std::ifstream limits(
        (std::string("/proc/") + std::to_string(pid) + "/limits").c_str());
    std::string line;
    while (std::getline(limits, line)) {
        static const char label[] = "Max open files";
        if (line.compare(0, sizeof(label) - 1, label) != 0) {
            continue;
        }
        std::istringstream values(line.substr(sizeof(label) - 1));
        uint64_t soft = 0;
        uint64_t hard = 0;
        return (values >> soft >> hard) &&
               soft == expected && hard == expected;
    }
    return false;
}

void flush_hello(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_result result = capsid_worker_flush(worker);
        if (result == CAPSID_OK) {
            return;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("HELLO flush failed: ") +
                 capsid_result_string(result));
        }
        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLOUT;
        poll(&descriptor, 1, 10);
    }
    fail("HELLO flush timeout");
}

}  // namespace

int main(int argc, char **argv) {
#if !defined(__linux__)
    (void)argc;
    (void)argv;
    return 77;
#else
    if (argc != 2) {
        fail("expected worker path");
    }

    char inherited_template[] = "/tmp/capsid-inherited-fd-XXXXXX";
    const int temporary = mkstemp(inherited_template);
    if (temporary < 0) {
        fail(std::string("cannot create inherited-fd probe: ") +
             std::strerror(errno));
    }
    const int inherited = fcntl(temporary, F_DUPFD, 100);
    close(temporary);
    unlink(inherited_template);
    if (inherited < 100 ||
        fcntl(inherited, F_SETFD, 0) != 0) {
        close(inherited);
        fail("cannot prepare inheritable high-numbered fd");
    }

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.strict_sandbox = 1;
    capsid_resource_limits limits;
    capsid_resource_limits_init(&limits);
    limits.enabled_fields = CAPSID_RESOURCE_LIMIT_FILE_DESCRIPTORS;
    limits.file_descriptors = 48;
    config.resource_limits = &limits;

    capsid_worker *worker = NULL;
    const capsid_result result = capsid_worker_spawn(&config, &worker);
    if (result != CAPSID_OK) {
        close(inherited);
        fail(std::string("worker spawn failed: ") +
             capsid_result_string(result));
    }

    // Binding v1 §4.3 startup order: the worker receives the App bundle
    // bytes (no JavaScript runs) before installing the sandbox. Sending the
    // bundle immediately after spawn keeps this test aligned with the
    // current startup contract while still proving fd/environment hygiene
    // and kernel sandbox activation before READY/JS execution.
    static const char kMinimalBundle[] =
        "export default { async fetch() { return new Response('ok'); } };";
    if (capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(kMinimalBundle),
            sizeof(kMinimalBundle) - 1) != CAPSID_OK) {
        capsid_worker_destroy(worker);
        close(inherited);
        fail("worker bundle load failed");
    }

    const int64_t worker_pid = capsid_worker_pid(worker);
    const std::string inherited_path =
        std::string("/proc/") + std::to_string(worker_pid) +
        "/fd/" + std::to_string(inherited);
    const std::string stdin_path =
        std::string("/proc/") + std::to_string(worker_pid) + "/fd/0";
    const std::string stdout_path =
        std::string("/proc/") + std::to_string(worker_pid) + "/fd/1";
    const std::string stderr_path =
        std::string("/proc/") + std::to_string(worker_pid) + "/fd/2";

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           path_exists(inherited_path)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool leaked = path_exists(inherited_path);
    const std::string stdin_target = symlink_target(stdin_path);
    const std::string stdout_target = symlink_target(stdout_path);
    const std::string stderr_target = symlink_target(stderr_path);
    const bool environment_empty =
        worker_environment_is_empty(worker_pid);

    flush_hello(worker);
    const std::chrono::steady_clock::time_point sandbox_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < sandbox_deadline &&
           !kernel_sandbox_is_active(worker_pid)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool sandbox_before_ready =
        kernel_sandbox_is_active(worker_pid);
    const bool open_file_limit_applied =
        worker_open_file_limit_is(worker_pid, 48);

    capsid_worker_destroy(worker);
    close(inherited);

    if (leaked) {
        fail("worker inherited an unrelated host file descriptor");
    }
    if (stdin_target != "/dev/null" ||
        stdout_target != "/dev/null" ||
        stderr_target != "/dev/null") {
        fail("strict worker stdio was not isolated through /dev/null");
    }
    if (!environment_empty) {
        fail("strict worker inherited the host environment");
    }
    if (!sandbox_before_ready) {
        fail("strict kernel sandbox was not active after bundle bytes/before READY");
    }
    if (!open_file_limit_applied) {
        fail("configured open-file rlimit was not applied");
    }
    return 0;
#endif
}
