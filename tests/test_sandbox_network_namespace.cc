#include "capsid/runtime.h"

#if defined(__linux__)
#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void fail(const std::string &message) {
    std::cerr << "network-namespace-test: " << message << std::endl;
    std::exit(1);
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot read fixture: ") + path);
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

#if defined(__linux__)

bool send_descriptor(int socket_fd, int descriptor) {
    char byte = 'N';
    struct iovec iov = {};
    iov.iov_base = &byte;
    iov.iov_len = 1;
    char control[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr message = {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
    return sendmsg(socket_fd, &message, 0) == 1;
}

int receive_descriptor(int socket_fd) {
    char byte = 0;
    struct iovec iov = {};
    iov.iov_base = &byte;
    iov.iov_len = 1;
    char control[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr message = {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(socket_fd, &message, 0) != 1) {
        return -1;
    }
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message);
         header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET &&
            header->cmsg_type == SCM_RIGHTS &&
            header->cmsg_len == CMSG_LEN(sizeof(int))) {
            int descriptor = -1;
            std::memcpy(
                &descriptor, CMSG_DATA(header), sizeof(descriptor));
            return descriptor;
        }
    }
    return -1;
}

uint32_t wait_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail("worker flush failed");
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return event.flags;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker startup error: ") +
                     std::string(
                         reinterpret_cast<const char *>(
                             event.payload.data),
                         event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK ||
            std::chrono::steady_clock::now() >= deadline) {
            fail("worker READY timeout");
        }
        usleep(1000);
    }
}

int run(const char *worker_path, const char *fixture_path) {
    // Regression: pin a descriptor at fd 4 before any other descriptor is
    // allocated, so the parent-side fd layout (pin=4, target namespace=5,
    // IPC socketpair=6/7) pushes the spawn's namespace-fd copy onto the
    // child IPC fd target (8). A spawn that copies the namespace fd into
    // the F_DUPFD range instead of strictly above every child fd clobbers
    // the IPC socket during the pre-exec file actions and the worker dies
    // on its first IPC read.
    const int layout_pin = open("/dev/null", O_RDWR);
    if (layout_pin < 0) {
        fail("layout pin open failed");
    }
    if (dup2(layout_pin, 4) < 0) {
        fail("layout pin dup2 failed");
    }
    close(layout_pin);

    int control[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, control) != 0) {
        fail("control socketpair failed");
    }
    const pid_t owner = fork();
    if (owner < 0) {
        fail("namespace owner fork failed");
    }
    if (owner == 0) {
        close(control[0]);
        if (unshare(CLONE_NEWNET) != 0) {
            _exit(77);
        }
        const int descriptor =
            open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
        if (descriptor < 0 ||
            !send_descriptor(control[1], descriptor)) {
            _exit(78);
        }
        close(descriptor);
        char byte = 0;
        while (read(control[1], &byte, 1) > 0) {
        }
        _exit(0);
    }

    close(control[1]);
    const int target_namespace = receive_descriptor(control[0]);
    if (target_namespace < 0) {
        int status = 0;
        waitpid(owner, &status, 0);
        close(control[0]);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
            std::cerr
                << "network namespace creation unavailable"
                << std::endl;
            return 77;
        }
        fail("namespace owner did not provide a descriptor");
    }

    struct stat host_namespace = {};
    struct stat target_stat = {};
    if (stat("/proc/self/ns/net", &host_namespace) != 0 ||
        fstat(target_namespace, &target_stat) != 0 ||
        (host_namespace.st_dev == target_stat.st_dev &&
         host_namespace.st_ino == target_stat.st_ino)) {
        fail("target network namespace was not isolated");
    }

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.sandbox_network_namespace_fd = target_namespace;
    config.sandbox_required_features =
        CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE;
    capsid_worker *worker = NULL;
    const capsid_result spawn = capsid_worker_spawn(&config, &worker);
    if (spawn != CAPSID_OK) {
        fail(std::string("worker spawn failed: ") +
             capsid_result_string(spawn));
    }
    const std::string bundle = read_file(fixture_path);
    if (capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()) != CAPSID_OK) {
        fail("load bundle failed");
    }
    const uint32_t features = wait_ready(worker);
    if ((features & CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE) == 0) {
        fail("READY omitted network namespace feature");
    }

    const std::string worker_namespace_path =
        std::string("/proc/") +
        std::to_string(capsid_worker_pid(worker)) +
        "/ns/net";
    struct stat worker_stat = {};
    if (stat(worker_namespace_path.c_str(), &worker_stat) != 0 ||
        worker_stat.st_dev != target_stat.st_dev ||
        worker_stat.st_ino != target_stat.st_ino) {
        fail("worker did not enter the supplied network namespace");
    }

    struct stat caller_owned_fd = {};
    if (fstat(target_namespace, &caller_owned_fd) != 0) {
        fail("spawn closed the caller-owned namespace descriptor");
    }

    capsid_worker_destroy(worker);
    close(target_namespace);
    close(control[0]);
    int status = 0;
    waitpid(owner, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("namespace owner cleanup failed");
    }
    return 0;
}

#endif

}  // namespace

int main(int argc, char **argv) {
#if !defined(__linux__)
    (void) argc;
    (void) argv;
    return 77;
#else
    if (argc != 3) {
        fail("expected worker path and fixture path");
    }
    return run(argv[1], argv[2]);
#endif
}
