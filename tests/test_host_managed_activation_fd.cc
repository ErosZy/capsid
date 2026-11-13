// Deterministic regression for the App-directory FD borrowed by activation.
// GNU ld --wrap=close lets the owning test reuse that exact number after its
// first close, as a concurrent Admin accept can. The product must never close
// the replacement socket. No scheduling, retry or sanitizer suppression is
// involved; deploy/replay/persist-failure all use the real worker pipeline.
#include "host/managed_host.h"
#include "build_identity.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

namespace {
struct CloseProbe {
    std::mutex mutex;
    bool enabled = false;
    dev_t device = 0;
    ino_t inode = 0;
    int source = -1;
    int reused = -1;
    unsigned closes = 0;
    bool duplicated = false;
} probe;

void require(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string temporary_directory() {
    std::string pattern = "/tmp/capsid-activation-fd-XXXXXX";
    require(mkdtemp(pattern.data()) != nullptr, "create private fixture root");
    return pattern;
}

void make_directory(const std::string& path) {
    require(mkdir(path.c_str(), 0700) == 0, "create fixture directory " + path);
}

void write_file(const std::string& path, const std::string& bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "open fixture file");
    require(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(),
            "write fixture file");
    require(std::fclose(file) == 0, "close fixture file");
}

void destroy_workers(const capsid::host::DeployOutcome& outcome) {
    for (auto* worker : outcome.workers) {
        capsid_worker_destroy(worker);
    }
}
}

extern "C" int __real_close(int fd);
extern "C" int __wrap_close(int fd) {
    std::lock_guard<std::mutex> guard(probe.mutex);
    if (probe.enabled) {
        struct stat st = {};
        const bool app_directory = fstat(fd, &st) == 0 &&
            st.st_dev == probe.device && st.st_ino == probe.inode;
        if (app_directory || fd == probe.reused) {
            const int result = __real_close(fd);
            if (result == 0) {
                ++probe.closes;
                if (probe.closes == 1) {
                    probe.reused = fd;
                    probe.duplicated = dup2(probe.source, fd) == fd;
                }
            }
            return result;
        }
    }
    return __real_close(fd);
}

int main(int argc, char** argv) {
    require(argc == 3, "expected activation FD mode and worker path");
    const std::string mode = argv[1];
    const bool replay = mode == "host_managed_activation_fd_replay" ||
        mode == "host_managed_activation_fd_replay_persist_failure";
    const bool persist_failure = mode == "host_managed_activation_fd_persist_failure" ||
        mode == "host_managed_activation_fd_replay_persist_failure";
    require(replay || persist_failure || mode == "host_managed_activation_fd_fresh",
            "unknown activation FD test mode");
    const std::string root = temporary_directory();
    for (const auto& suffix : {"/apps", "/apps/orders", "/apps/orders/v1",
                               "/state", "/state/apps", "/state/apps/orders",
                               "/secrets", "/secrets/orders"}) {
        make_directory(root + suffix);
    }
    const std::string app_state = root + "/state/apps/orders";
    write_file(root + "/apps/orders/v1/capsid.json",
        R"({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":[]},"pool":{"minReady":1,"maxWorkers":1}})");
    write_file(root + "/apps/orders/v1/bundle.mjs",
        "export default { fetch: () => new Response('fd-ownership-ok') };");
    const int apps = open((root + "/apps").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int secrets = open((root + "/secrets").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(apps >= 0 && secrets >= 0, "open fixture roots");
    capsid::host::ManagedHostOptions options;
    options.applications_root_fd = apps;
    options.secret_root_template_fd = secrets;
    options.state_root = root + "/state";
    options.application = "orders";
    options.worker_path = argv[2];
    options.runtime_compatibility_id = CAPSID_BUILD_COMPATIBILITY_ID;
    options.host_policy.max_workers = 1;
    options.host_policy.min_ready = 1;
    options.host_policy.max_requests_per_worker = 10000;
    options.host_policy.max_worker_memory_bytes = 256U * 1024U * 1024U;
    capsid::host::OperationStatus status;
    if (replay) {
        const auto initial = capsid::host::managed_deploy(&options, "v1", &status);
        destroy_workers(initial);
        require(initial.ok, "prepare committed generation for replay: " + initial.error);
    }
    if (persist_failure) {
        if (replay) {
            require(unlink((app_state + "/active.json").c_str()) == 0,
                    "replace only fixture-owned active state");
        }
        make_directory(app_state + "/active.json");
    }
    struct stat directory = {};
    require(stat(app_state.c_str(), &directory) == 0, "identify App state directory");
    int sockets[2] = {-1, -1};
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "create replacement stream");
    {
        std::lock_guard<std::mutex> guard(probe.mutex);
        probe.device = directory.st_dev;
        probe.inode = directory.st_ino;
        probe.source = sockets[0];
        probe.enabled = true;
    }
    const auto outcome = capsid::host::managed_deploy(&options, "v1", &status);
    unsigned closes;
    int replacement;
    bool duplicated;
    {
        std::lock_guard<std::mutex> guard(probe.mutex);
        probe.enabled = false;
        closes = probe.closes;
        replacement = probe.reused;
        duplicated = probe.duplicated;
    }
    destroy_workers(outcome);
    require(outcome.ok != persist_failure, "activation result: " + outcome.error);
    if (persist_failure) {
        require(outcome.error == "cannot persist active state", "exercise real persist failure");
    }
    require(duplicated && replacement >= 0, "actually reuse the released directory FD");
    const bool still_open = fcntl(replacement, F_GETFD) >= 0;
    std::cout << "ACTIVATION_FD closes=" << closes << " replacement=" << replacement
              << " still_open=" << still_open << '\n';
    require(closes == 1 && still_open,
            "activation closed its caller-owned App FD twice and destroyed a replacement stream");
    const char byte = 'x';
    require(send(sockets[1], &byte, 1, MSG_NOSIGNAL) == 1, "send on replacement peer");
    char received = 0;
    require(recv(replacement, &received, 1, MSG_DONTWAIT) == 1 && received == byte,
            "replacement stream remains usable after activation returns");
    close(replacement);
    close(sockets[0]);
    close(sockets[1]);
    close(apps);
    close(secrets);
    std::filesystem::remove_all(root);
    std::cout << "PASS\n";
}
