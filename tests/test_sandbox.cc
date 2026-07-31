#include "sandbox.h"
#include "capsid/runtime.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#if defined(__linux__)

int run_linux_probe(const std::string &allowed_path,
                    const std::string &denied_path) {
    capsid::SandboxConfig config;
    config.address_space_limit = 0;
    config.file_descriptor_limit = 64;
    config.strict = true;
    config.required_features = CAPSID_SANDBOX_FEATURE_STRICT_BASE;
    config.preinstalled_features = 0;
    config.read_only_paths.push_back(allowed_path);

    uint32_t features = 0;
    std::string error;
    if (!capsid::apply_sandbox(config, &features, &error)) {
        std::cerr << "sandbox setup failed: " << error << std::endl;
        return 10;
    }
    if ((features & CAPSID_SANDBOX_FEATURE_STRICT_BASE) !=
        CAPSID_SANDBOX_FEATURE_STRICT_BASE) {
        return 11;
    }
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
        return 12;
    }

    const int allowed = open(allowed_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (allowed < 0) {
        return 13;
    }
    close(allowed);

    errno = 0;
    const int denied = open(denied_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (denied >= 0 || (errno != EACCES && errno != EPERM)) {
        if (denied >= 0) {
            close(denied);
        }
        return 14;
    }

    errno = 0;
    const int writable = open(
        allowed_path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (writable >= 0 || (errno != EACCES && errno != EPERM)) {
        if (writable >= 0) {
            close(writable);
        }
        return 15;
    }

#ifdef SYS_openat2
    struct open_how write_how = {};
    write_how.flags = O_WRONLY | O_APPEND | O_CLOEXEC;
    errno = 0;
    const int openat2_writable = static_cast<int>(syscall(
        SYS_openat2,
        AT_FDCWD,
        allowed_path.c_str(),
        &write_how,
        sizeof(write_how)));
    if (openat2_writable >= 0 ||
        (errno != EACCES && errno != EPERM)) {
        if (openat2_writable >= 0) {
            close(openat2_writable);
        }
        return 20;
    }
#endif

    errno = 0;
    const pid_t child = fork();
    if (child >= 0 || errno != EPERM) {
        if (child == 0) {
            _exit(0);
        }
        if (child > 0) {
            waitpid(child, NULL, 0);
        }
        return 16;
    }

    errno = 0;
    const int unix_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (unix_socket >= 0 || errno != EPERM) {
        if (unix_socket >= 0) {
            close(unix_socket);
        }
        return 17;
    }

    const int network_socket =
        socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (network_socket < 0) {
        return 18;
    }

    errno = 0;
    const int raw_socket = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (raw_socket >= 0 || errno != EPERM) {
        if (raw_socket >= 0) {
            close(raw_socket);
        }
        close(network_socket);
        return 19;
    }

    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    errno = 0;
    const int bind_result = bind(
        network_socket,
        reinterpret_cast<const struct sockaddr *>(&address),
        sizeof(address));
    if (bind_result == 0 || errno != EPERM) {
        close(network_socket);
        return 20;
    }

    errno = 0;
    if (listen(network_socket, 1) == 0 || errno != EPERM) {
        close(network_socket);
        return 21;
    }

    errno = 0;
    if (accept(network_socket, NULL, NULL) >= 0 || errno != EPERM) {
        close(network_socket);
        return 22;
    }
    close(network_socket);

    errno = 0;
    void *const executable = reinterpret_cast<void *>(syscall(
        SYS_mmap,
        NULL,
        4096,
        PROT_READ | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0));
    const int executable_errno = errno;
    if (executable != MAP_FAILED || executable_errno != EPERM) {
        if (executable != MAP_FAILED) {
            munmap(executable, 4096);
        }
        return 23;
    }

    void *const writable_mapping = mmap(
        NULL,
        4096,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    if (writable_mapping == MAP_FAILED) {
        return 24;
    }
    errno = 0;
    const long protected_result = syscall(
        SYS_mprotect,
        writable_mapping,
        4096,
        PROT_READ | PROT_EXEC);
    const int protected_errno = errno;
    munmap(writable_mapping, 4096);
    if (protected_result == 0 || protected_errno != EPERM) {
        return 25;
    }

    return 0;
}

int run_linux_namespace_probe() {
    const uint32_t namespace_features =
        CAPSID_SANDBOX_FEATURE_USER_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_MOUNT_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_IPC_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_UTS_NAMESPACE;
    capsid::SandboxConfig config;
    config.address_space_limit = 0;
    config.file_descriptor_limit = 64;
    config.strict = true;
    config.required_features =
        CAPSID_SANDBOX_FEATURE_STRICT_BASE | namespace_features;
    config.preinstalled_features = 0;

    uint32_t features = 0;
    std::string error;
    if (!capsid::apply_sandbox(config, &features, &error)) {
        std::cerr << "namespace sandbox unavailable: " << error << std::endl;
        return 77;
    }
    return (features & namespace_features) == namespace_features ? 0 : 20;
}

#endif

}  // namespace

int main(int argc, char **argv) {
#if defined(__linux__)
    if (argc == 2 && std::string(argv[1]) == "--namespaces") {
        const pid_t pid = fork();
        require(pid >= 0, "namespace sandbox probe forked");
        if (pid == 0) {
            _exit(run_linux_namespace_probe());
        }
        int status = 0;
        require(
            waitpid(pid, &status, 0) == pid,
            "namespace sandbox probe reaped");
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        fail("namespace sandbox probe terminated by signal");
    }
    if (argc != 1) {
        fail("unknown sandbox test option");
    }
    char allowed_template[] = "/tmp/capsid-sandbox-allowed-XXXXXX";
    char denied_template[] = "/tmp/capsid-sandbox-denied-XXXXXX";
    const int allowed = mkstemp(allowed_template);
    const int denied = mkstemp(denied_template);
    require(allowed >= 0 && denied >= 0, "temporary probe files created");
    require(write(allowed, "allowed", 7) == 7, "allowed probe initialized");
    require(write(denied, "denied", 6) == 6, "denied probe initialized");
    close(allowed);
    close(denied);

    const pid_t pid = fork();
    require(pid >= 0, "sandbox probe forked");
    if (pid == 0) {
        _exit(run_linux_probe(allowed_template, denied_template));
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid, "sandbox probe reaped");
    unlink(allowed_template);
    unlink(denied_template);
    if (!WIFEXITED(status)) {
        fail(
            std::string("sandbox probe terminated by signal ") +
            std::to_string(WTERMSIG(status)));
    }
    if (WEXITSTATUS(status) != 0) {
        fail(
            std::string("sandbox probe failed with code ") +
            std::to_string(WEXITSTATUS(status)));
    }
#else
    if (argc == 2 && std::string(argv[1]) == "--namespaces") {
        return 77;
    }
    if (argc != 1) {
        fail("unknown sandbox test option");
    }
    capsid::SandboxConfig config;
    config.address_space_limit = 0;
    config.file_descriptor_limit = 64;
    config.strict = true;
    config.required_features = CAPSID_SANDBOX_FEATURE_STRICT_BASE;
    config.preinstalled_features = 0;
    uint32_t features = 0;
    std::string error;
    require(
        !capsid::apply_sandbox(config, &features, &error),
        "strict sandbox remains fail-closed outside Linux");
    require(
        error.find("strict sandbox is unavailable") != std::string::npos,
        "unsupported platform error is explicit");
#endif
    return 0;
}
