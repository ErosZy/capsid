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

// macOS does not define SOCK_CLOEXEC; these IPC pairs do not cross exec
// on the test paths, so a plain socket type is the portable fallback.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
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
    if (!capsid::apply_sandbox(config, &features, NULL, NULL, &error)) {
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
    if (!capsid::apply_sandbox(config, &features, NULL, NULL, &error)) {
        std::cerr << "namespace sandbox unavailable: " << error << std::endl;
        return 77;
    }
    return (features & namespace_features) == namespace_features ? 0 : 20;
}


// Binding v1 §7.9: profile conformance probes. Each probe runs in a fresh
// forked child with the binding profiles under test; the parent holds the
// listener and the authorized directory.
int run_binding_write_probe(const std::string &authorized_dir) {
    capsid::SandboxConfig config;
    config.address_space_limit = 0;
    config.file_descriptor_limit = 64;
    config.strict = true;
    config.required_features = CAPSID_SANDBOX_FEATURE_STRICT_BASE;
    config.preinstalled_features = 0;
    config.binding_profiles = {"filesystem-write"};
    config.binding_write_paths.push_back(authorized_dir);

    uint32_t features = 0;
    uint32_t landlock_abi = 0;
    uint32_t seccomp_mode = 0;
    std::string error;
    if (!capsid::apply_sandbox(
            config, &features, &landlock_abi, &seccomp_mode, &error)) {
        std::cerr << "binding sandbox unavailable: " << error << std::endl;
        return 77;
    }
    if (landlock_abi < 3) {
        // §4.2: an old kernel must fail the worker startup — the launcher
        // would have refused, so this build combination cannot run the
        // conformance probe.
        return 77;
    }
    if (seccomp_mode == 0) {
        return 30;
    }
    // Write inside the authorized directory succeeds.
    const std::string inside = authorized_dir + "/binding-probe.txt";
    const int fd = open(inside.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return 31;
    }
    if (write(fd, "ok", 2) != 2) {
        close(fd);
        return 32;
    }
    close(fd);
    if (unlink(inside.c_str()) != 0) {
        return 33;
    }
    // Writing outside the authorized directory fails (Landlock).
    errno = 0;
    const int escape_fd = open(
        "/tmp/capsid-binding-escape-probe",
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const int escape_errno = errno;
    if (escape_fd >= 0) {
        close(escape_fd);
        unlink("/tmp/capsid-binding-escape-probe");
        return 34;
    }
    if (escape_errno != EACCES && escape_errno != EPERM) {
        return 35;
    }
    return 0;
}

int run_binding_network_probe(int listener_fd) {
    capsid::SandboxConfig config;
    config.address_space_limit = 0;
    config.file_descriptor_limit = 64;
    config.strict = true;
    config.required_features = CAPSID_SANDBOX_FEATURE_STRICT_BASE;
    config.preinstalled_features = 0;
    config.binding_profiles = {"network-client"};

    uint32_t features = 0;
    std::string error;
    if (!capsid::apply_sandbox(
            config, &features, NULL, NULL, &error)) {
        std::cerr << "binding sandbox unavailable: " << error << std::endl;
        return 77;
    }
    // A client connect to the parent's listener succeeds.
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Learn the parent listener port via the inherited fd? The parent
    // binds port 0 and passes the port number via the probe's argv in the
    // fork wrapper — simpler: the probe reconnects to the port recorded in
    // the environment-free global below.
    (void)listener_fd;
    extern int binding_probe_listener_port;
    address.sin_port = htons(
        static_cast<uint16_t>(binding_probe_listener_port));
    const int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0) {
        return 40;
    }
    if (connect(client, reinterpret_cast<struct sockaddr *>(&address),
                sizeof(address)) != 0) {
        close(client);
        return 41;
    }
    close(client);
    // Server syscalls stay permanently denied even with network-client.
    errno = 0;
    const int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server >= 0) {
        int reuse = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(server, reinterpret_cast<struct sockaddr *>(&address),
                 sizeof(address)) == 0) {
            close(server);
            return 42;
        }
        close(server);
    }
    return 0;
}

int run_binding_wasi_probe() {
    capsid::SandboxConfig config;
    config.address_space_limit = 0;
    config.file_descriptor_limit = 64;
    config.strict = true;
    config.required_features = CAPSID_SANDBOX_FEATURE_STRICT_BASE;
    config.preinstalled_features = 0;
    config.binding_profiles = {"wasi"};
    uint32_t features = 0;
    std::string error;
    if (capsid::apply_sandbox(
            config, &features, NULL, NULL, &error)) {
        return 50;
    }
    if (error.find("not implemented") == std::string::npos) {
        return 51;
    }
    return 0;
}

int binding_probe_listener_port = 0;

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
    if (argc == 3 && std::string(argv[1]) == "--binding-write") {
        const pid_t pid = fork();
        require(pid >= 0, "binding write probe forked");
        if (pid == 0) {
            _exit(run_binding_write_probe(argv[2]));
        }
        int status = 0;
        require(waitpid(pid, &status, 0) == pid,
                "binding write probe reaped");
        return WIFEXITED(status) ? WEXITSTATUS(status) : 60;
    }
    if (argc == 2 && std::string(argv[1]) == "--binding-network") {
        // Parent holds a listener; the child connects to it.
        const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(listener >= 0, "binding listener created");
        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        require(bind(listener, reinterpret_cast<struct sockaddr *>(&address),
                     sizeof(address)) == 0,
                "binding listener bound");
        socklen_t length = sizeof(address);
        require(getsockname(listener,
                            reinterpret_cast<struct sockaddr *>(&address),
                            &length) == 0,
                "binding listener named");
        require(listen(listener, 4) == 0, "binding listener listening");
        binding_probe_listener_port = ntohs(address.sin_port);
        const pid_t pid = fork();
        require(pid >= 0, "binding network probe forked");
        if (pid == 0) {
            close(listener);
            _exit(run_binding_network_probe(-1));
        }
        int status = 0;
        require(waitpid(pid, &status, 0) == pid,
                "binding network probe reaped");
        close(listener);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 61;
    }
    if (argc == 2 && std::string(argv[1]) == "--binding-wasi") {
        const pid_t pid = fork();
        require(pid >= 0, "binding wasi probe forked");
        if (pid == 0) {
            _exit(run_binding_wasi_probe());
        }
        int status = 0;
        require(waitpid(pid, &status, 0) == pid,
                "binding wasi probe reaped");
        return WIFEXITED(status) ? WEXITSTATUS(status) : 62;
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
    // Binding v1 §7.9: profile conformance probes skip outside privileged
    // Linux; the Hosted Validity workflow treats skip 77 as a failure.
    if (argc == 2 && std::string(argv[1]) == "--binding-network") {
        return 77;
    }
    if (argc == 2 && std::string(argv[1]) == "--binding-wasi") {
        return 77;
    }
    if (argc == 3 && std::string(argv[1]) == "--binding-write") {
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
        !capsid::apply_sandbox(config, &features, NULL, NULL, &error),
        "strict sandbox remains fail-closed outside Linux");
    require(
        error.find("strict sandbox is unavailable") != std::string::npos,
        "unsupported platform error is explicit");
#endif
    return 0;
}
