#include "worker_runtime.h"

extern "C" {
#include "tjs.h"
}

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

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
#if defined(_WIN32)
    // Windows needs no descriptor sweep: the spawner passes an explicit
    // inheritable-handle list to CreateProcess, so the worker already owns
    // exactly the IPC handle (and stdio when --close-stdio is absent).
    (void)ipc_fd;
    (void)network_namespace_fd;
#else
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
#endif
}

bool redirect_stdio_to_dev_null() {
    /*
     * libuv requires every descriptor it owns to be greater than stderr.
     * Leaving 0/1/2 closed lets epoll_create1() reuse fd 0, and libuv then
     * aborts during loop teardown.  Replacing inherited stdio with /dev/null
     * keeps the worker silent without exposing host descriptors. On Windows
     * the same contract maps to NUL.
     */
#if defined(_WIN32)
    const int null_fd = _open("NUL", _O_RDWR);
#else
    const int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
#endif
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
    {
        FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
        if (dbg != NULL) {
            std::fprintf(dbg, "worker main entered argc=%d\n", argc);
            std::fclose(dbg);
        }
    }
    std::cerr << "capsid-worker: DEBUG main entered" << std::endl;
    int ipc_fd = -1;
    int network_namespace_fd = -1;
    bool close_stdio = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ipc-fd") == 0 && i + 1 < argc) {
#if defined(_WIN32)
            // The spawner passes the inherited OS handle value; convert it
            // back into a CRT descriptor. Handle values are preserved
            // verbatim across CreateProcess inheritance.
            ipc_fd = _open_osfhandle(
                static_cast<intptr_t>(
                    std::strtoull(argv[++i], NULL, 10)),
                _O_RDWR | _O_BINARY);
#else
            ipc_fd = std::atoi(argv[++i]);
#endif
        } else if (std::strcmp(
                       argv[i],
                       "--network-namespace-fd") == 0 &&
                   i + 1 < argc) {
            // Linux-only in practice (the spawner rejects namespace fds
            // outside Linux); parsed for CLI symmetry on every platform.
            network_namespace_fd = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--close-stdio") == 0) {
            close_stdio = true;
        }
    }
    if (ipc_fd < 0) {
        std::cerr << "capsid-worker: --ipc-fd is required" << std::endl;
        return 2;
    }

#if defined(_WIN32)
    // CRT defaults: binary file mode for every open()/fopen() in the
    // worker (IPC frames, JS bundle artifacts) and Winsock initialized
    // for the ioctlsocket/CRT-over-socket paths.
    capsid::win32::set_binary_file_defaults();
    (void)capsid::win32::ensure_winsock();
#endif

    close_unrelated_descriptors(ipc_fd, network_namespace_fd);
    if (close_stdio && !redirect_stdio_to_dev_null()) {
        return 2;
    }
    std::cerr << "capsid-worker: DEBUG started fd=" << ipc_fd << std::endl;
#if defined(_WIN32)
    {
        FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
        if (dbg != NULL) {
            const SOCKET before = static_cast<SOCKET>(
                _get_osfhandle(ipc_fd));
            int type = 0;
            int length = sizeof(type);
            std::fprintf(
                dbg,
                "before TJS: handle=%lld so_type=%d type_err=%d\n",
                static_cast<long long>(before),
                getsockopt(before, SOL_SOCKET, SO_TYPE,
                           reinterpret_cast<char *>(&type), &length) == 0
                    ? type
                    : -1,
                WSAGetLastError());
            std::fclose(dbg);
        }
    }
#endif
    TJS_Initialize(argc, argv);
#if defined(_WIN32)
    {
        FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
        if (dbg != NULL) {
            const SOCKET after = static_cast<SOCKET>(
                _get_osfhandle(ipc_fd));
            int type = 0;
            int length = sizeof(type);
            std::fprintf(
                dbg,
                "after TJS: handle=%lld so_type=%d type_err=%d\n",
                static_cast<long long>(after),
                getsockopt(after, SOL_SOCKET, SO_TYPE,
                           reinterpret_cast<char *>(&type), &length) == 0
                    ? type
                    : -1,
                WSAGetLastError());
            for (int probe_fd = 0; probe_fd <= 5; ++probe_fd) {
                std::fprintf(dbg, "after TJS: fd=%d handle=%lld\n",
                             probe_fd,
                             static_cast<long long>(
                                 _get_osfhandle(probe_fd)));
            }
            std::fprintf(dbg, "after TJS: stdout_fileno=%d stderr_fileno=%d\n",
                         _fileno(stdout), _fileno(stderr));
            std::fclose(dbg);
        }
    }
#endif
    return capsid_run_worker(ipc_fd, network_namespace_fd);
}
