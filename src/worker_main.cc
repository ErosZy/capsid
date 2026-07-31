#include "worker_runtime.h"

extern "C" {
#include "tjs.h"
}

#include <cstdlib>
#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace {

void close_unrelated_descriptors(int ipc_fd,
                                 int network_namespace_fd) {
#if defined(__linux__) && defined(__NR_close_range)
    // Use close_range to close everything above the highest reserved fd.
    // IPC and network-namespace fds are placed above the range the dynamic
    // linker may reuse during startup (musl reopens fd 3).
    const unsigned int max_reserved =
        static_cast<unsigned int>(
            network_namespace_fd > ipc_fd
                ? network_namespace_fd
                : ipc_fd);
    if (max_reserved >= 3 &&
        syscall(__NR_close_range,
                max_reserved + 1,
                ~static_cast<unsigned int>(0),
                0) == 0) {
        return;
    }
#endif

    struct rlimit limit;
    rlim_t maximum = 65536;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
        limit.rlim_max != RLIM_INFINITY) {
        maximum = limit.rlim_max;
    }
    for (rlim_t fd = 3; fd < maximum; ++fd) {
        if (fd != static_cast<rlim_t>(ipc_fd) &&
            fd != static_cast<rlim_t>(
                      network_namespace_fd)) {
            close(static_cast<int>(fd));
        }
    }
}

bool redirect_stdio_to_dev_null() {
    /*
     * libuv requires every descriptor it owns to be greater than stderr.
     * Leaving 0/1/2 closed lets epoll_create1() reuse fd 0, and libuv then
     * aborts during loop teardown.  Replacing inherited stdio with /dev/null
     * keeps the worker silent without exposing host descriptors.
     */
    const int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd < 0) {
        return false;
    }
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (dup2(null_fd, fd) < 0) {
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
            return false;
        }
    }
    if (null_fd > STDERR_FILENO) {
        close(null_fd);
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    int ipc_fd = -1;
    int network_namespace_fd = -1;
    bool close_stdio = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ipc-fd") == 0 && i + 1 < argc) {
            ipc_fd = std::atoi(argv[++i]);
        } else if (std::strcmp(
                       argv[i],
                       "--network-namespace-fd") == 0 &&
                   i + 1 < argc) {
            network_namespace_fd = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--close-stdio") == 0) {
            close_stdio = true;
        }
    }
    if (ipc_fd < 0) {
        std::cerr << "capsid-worker: --ipc-fd is required" << std::endl;
        return 2;
    }

    close_unrelated_descriptors(ipc_fd, network_namespace_fd);
    if (close_stdio && !redirect_stdio_to_dev_null()) {
        return 2;
    }
    TJS_Initialize(argc, argv);
    return capsid_run_worker(ipc_fd, network_namespace_fd);
}
