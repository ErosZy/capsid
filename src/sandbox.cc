#include "sandbox.h"

#include <errno.h>
#include <sys/resource.h>

#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <vector>

#ifndef SYS_landlock_create_ruleset
#ifdef __NR_landlock_create_ruleset
#define SYS_landlock_create_ruleset __NR_landlock_create_ruleset
#endif
#endif
#ifndef SYS_landlock_add_rule
#ifdef __NR_landlock_add_rule
#define SYS_landlock_add_rule __NR_landlock_add_rule
#endif
#endif
#ifndef SYS_landlock_restrict_self
#ifdef __NR_landlock_restrict_self
#define SYS_landlock_restrict_self __NR_landlock_restrict_self
#endif
#endif
#endif

namespace capsid {

namespace {

bool set_limit(const char *name,
               int resource,
               rlim_t current,
               rlim_t maximum,
               std::string *error) {
    struct rlimit limit;
    limit.rlim_cur = current;
    limit.rlim_max = maximum;
    if (setrlimit(resource, &limit) == 0) {
        return true;
    }
    if (error) {
        *error = std::string(name) + ": " + std::strerror(errno);
    }
    return false;
}

#if defined(__linux__)

bool fail_errno(const char *operation, std::string *error) {
    if (error) {
        *error = std::string(operation) + ": " + std::strerror(errno);
    }
    return false;
}

bool write_all(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool write_control_file(const char *path,
                        const std::string &value,
                        bool optional,
                        std::string *error) {
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        if (optional && errno == ENOENT) {
            return true;
        }
        return fail_errno(path, error);
    }
    const bool ok = write_all(fd, value.data(), value.size());
    const int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return ok ? true : fail_errno(path, error);
}

bool install_namespaces(uint32_t required,
                        uint32_t *features,
                        std::string *error) {
    const uint32_t namespace_mask =
        CAPSID_SANDBOX_FEATURE_USER_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_MOUNT_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_IPC_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_UTS_NAMESPACE;
    if ((required & namespace_mask) == 0) {
        return true;
    }

    const uid_t uid = getuid();
    const gid_t gid = getgid();
    if (unshare(CLONE_NEWUSER) != 0) {
        return fail_errno("unshare(CLONE_NEWUSER)", error);
    }
    if (!write_control_file(
            "/proc/self/setgroups", "deny\n", true, error) ||
        !write_control_file(
            "/proc/self/uid_map",
            std::string("0 ") + std::to_string(uid) + " 1\n",
            false,
            error) ||
        !write_control_file(
            "/proc/self/gid_map",
            std::string("0 ") + std::to_string(gid) + " 1\n",
            false,
            error)) {
        return false;
    }
    if (setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0) {
        return fail_errno("enter user namespace identity", error);
    }
    *features |= CAPSID_SANDBOX_FEATURE_USER_NAMESPACE;

    int flags = 0;
    if ((required & CAPSID_SANDBOX_FEATURE_MOUNT_NAMESPACE) != 0) {
        flags |= CLONE_NEWNS;
    }
    if ((required & CAPSID_SANDBOX_FEATURE_IPC_NAMESPACE) != 0) {
        flags |= CLONE_NEWIPC;
    }
    if ((required & CAPSID_SANDBOX_FEATURE_UTS_NAMESPACE) != 0) {
        flags |= CLONE_NEWUTS;
    }
    if (flags != 0 && unshare(flags) != 0) {
        return fail_errno("unshare(additional namespaces)", error);
    }
    if ((flags & CLONE_NEWNS) != 0) {
        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
            return fail_errno("make mount namespace private", error);
        }
        *features |= CAPSID_SANDBOX_FEATURE_MOUNT_NAMESPACE;
    }
    if ((flags & CLONE_NEWIPC) != 0) {
        *features |= CAPSID_SANDBOX_FEATURE_IPC_NAMESPACE;
    }
    if ((flags & CLONE_NEWUTS) != 0) {
        *features |= CAPSID_SANDBOX_FEATURE_UTS_NAMESPACE;
    }
    return true;
}

bool install_network_namespace(int descriptor,
                               uint32_t required,
                               uint32_t *features,
                               std::string *error) {
    const bool expected =
        (required & CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE) != 0;
    if (!expected && descriptor < 0) {
        return true;
    }
    if (!expected || descriptor < 0) {
        if (error) {
            *error =
                "network namespace descriptor/feature mismatch";
        }
        if (descriptor >= 0) {
            close(descriptor);
        }
        return false;
    }
    const int result = setns(descriptor, CLONE_NEWNET);
    const int saved_errno = errno;
    close(descriptor);
    errno = saved_errno;
    if (result != 0) {
        return fail_errno("setns(CLONE_NEWNET)", error);
    }
    *features |= CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE;
    return true;
}

uint64_t landlock_access_for_abi(int abi) {
    uint64_t access =
        LANDLOCK_ACCESS_FS_EXECUTE |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) {
        access |= LANDLOCK_ACCESS_FS_REFER;
    }
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) {
        access |= LANDLOCK_ACCESS_FS_TRUNCATE;
    }
#endif
    return access;
}

bool add_landlock_path(int ruleset_fd,
                       const std::string &path,
                       bool optional,
                       std::string *error) {
    int path_fd =
        open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (path_fd < 0) {
        if (optional && (errno == ENOENT || errno == ENOTDIR)) {
            return true;
        }
        return fail_errno(
            (std::string("open Landlock path ") + path).c_str(), error);
    }
    struct stat info;
    if (fstat(path_fd, &info) != 0) {
        const int saved_errno = errno;
        close(path_fd);
        errno = saved_errno;
        return fail_errno(
            (std::string("stat Landlock path ") + path).c_str(), error);
    }
    if (S_ISLNK(info.st_mode)) {
        close(path_fd);
        if (!optional) {
            if (error) {
                *error =
                    std::string(
                        "Landlock path must not be a symlink: ") +
                    path;
            }
            return false;
        }
        path_fd = open(path.c_str(), O_PATH | O_CLOEXEC);
        if (path_fd < 0 || fstat(path_fd, &info) != 0) {
            const int saved_errno = errno;
            if (path_fd >= 0) {
                close(path_fd);
            }
            errno = saved_errno;
            return fail_errno(
                (std::string("open resolved Landlock path ") +
                 path).c_str(),
                error);
        }
    }
    struct landlock_path_beneath_attr rule = {};
    rule.parent_fd = path_fd;
    rule.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE;
    if (S_ISDIR(info.st_mode)) {
        rule.allowed_access |= LANDLOCK_ACCESS_FS_READ_DIR;
    }
    const int result = static_cast<int>(syscall(
        SYS_landlock_add_rule,
        ruleset_fd,
        LANDLOCK_RULE_PATH_BENEATH,
        &rule,
        0));
    const int saved_errno = errno;
    close(path_fd);
    errno = saved_errno;
    return result == 0
        ? true
        : fail_errno(
              (std::string("add Landlock rule ") + path).c_str(), error);
}

bool install_landlock(const std::vector<std::string> &required_paths,
                      std::string *error) {
#if defined(SYS_landlock_create_ruleset) && \
    defined(SYS_landlock_add_rule) && \
    defined(SYS_landlock_restrict_self)
    const int abi = static_cast<int>(syscall(
        SYS_landlock_create_ruleset,
        NULL,
        0,
        LANDLOCK_CREATE_RULESET_VERSION));
    if (abi < 1) {
        return fail_errno("query Landlock ABI", error);
    }

    struct landlock_ruleset_attr attr = {};
    attr.handled_access_fs = landlock_access_for_abi(abi);
    const int ruleset_fd = static_cast<int>(syscall(
        SYS_landlock_create_ruleset, &attr, sizeof(attr), 0));
    if (ruleset_fd < 0) {
        return fail_errno("create Landlock ruleset", error);
    }

    static const char *const optional_paths[] = {
        "/etc/resolv.conf",
        "/etc/hosts",
        "/etc/nsswitch.conf",
        "/etc/gai.conf",
        "/etc/localtime",
        "/etc/ssl/certs",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ca-certificates/extracted/tls-ca-bundle.pem",
        "/dev/urandom",
        "/dev/random",
    };
    for (size_t i = 0;
         i < sizeof(optional_paths) / sizeof(optional_paths[0]);
         ++i) {
        if (!add_landlock_path(
                ruleset_fd, optional_paths[i], true, error)) {
            close(ruleset_fd);
            return false;
        }
    }
    for (size_t i = 0; i < required_paths.size(); ++i) {
        if (!add_landlock_path(
                ruleset_fd, required_paths[i], false, error)) {
            close(ruleset_fd);
            return false;
        }
    }
    if (syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0) {
        const int saved_errno = errno;
        close(ruleset_fd);
        errno = saved_errno;
        return fail_errno("enforce Landlock ruleset", error);
    }
    close(ruleset_fd);
    return true;
#else
    (void)required_paths;
    if (error) {
        *error = "Landlock syscall numbers are unavailable in Linux headers";
    }
    return false;
#endif
}

void bpf_statement(std::vector<struct sock_filter> *filter,
                   uint16_t code,
                   uint32_t value) {
    struct sock_filter instruction = {};
    instruction.code = code;
    instruction.k = value;
    filter->push_back(instruction);
}

void bpf_jump(std::vector<struct sock_filter> *filter,
              uint16_t code,
              uint32_t value,
              uint8_t jump_true,
              uint8_t jump_false) {
    struct sock_filter instruction = {};
    instruction.code = code;
    instruction.jt = jump_true;
    instruction.jf = jump_false;
    instruction.k = value;
    filter->push_back(instruction);
}

uint32_t seccomp_errno_action() {
    return SECCOMP_RET_ERRNO |
        (static_cast<uint32_t>(EPERM) & SECCOMP_RET_DATA);
}

void deny_syscall(std::vector<struct sock_filter> *filter, int number) {
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K,
             static_cast<uint32_t>(number), 0, 1);
    bpf_statement(filter, BPF_RET | BPF_K, seccomp_errno_action());
}

void allow_syscall(std::vector<struct sock_filter> *filter, int number) {
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K,
             static_cast<uint32_t>(number), 0, 1);
    bpf_statement(filter, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
}

void filter_open_syscall(std::vector<struct sock_filter> *filter,
                         int number,
                         size_t flags_argument) {
    uint32_t forbidden =
        O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
#ifdef O_TMPFILE
    forbidden |= O_TMPFILE;
#endif
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K,
             static_cast<uint32_t>(number), 0, 4);
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        static_cast<uint32_t>(
            offsetof(struct seccomp_data, args) +
            flags_argument * sizeof(uint64_t)));
    bpf_jump(filter, BPF_JMP | BPF_JSET | BPF_K, forbidden, 0, 1);
    bpf_statement(filter, BPF_RET | BPF_K, seccomp_errno_action());
    bpf_statement(filter, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));
}

void filter_executable_memory(std::vector<struct sock_filter> *filter,
                              int number) {
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K,
             static_cast<uint32_t>(number), 0, 4);
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        static_cast<uint32_t>(offsetof(struct seccomp_data, args[2])));
    bpf_jump(filter, BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1);
    bpf_statement(filter, BPF_RET | BPF_K, seccomp_errno_action());
    bpf_statement(filter, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));
}

void filter_socket_syscall(std::vector<struct sock_filter> *filter) {
#ifdef __NR_socket
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K,
             static_cast<uint32_t>(__NR_socket), 0, 10);
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        static_cast<uint32_t>(offsetof(struct seccomp_data, args[0])));
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 2, 0);
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 1, 0);
    bpf_statement(filter, BPF_RET | BPF_K, seccomp_errno_action());
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        static_cast<uint32_t>(offsetof(struct seccomp_data, args[1])));
    bpf_statement(
        filter,
        BPF_ALU | BPF_AND | BPF_K,
        static_cast<uint32_t>(~(SOCK_NONBLOCK | SOCK_CLOEXEC)));
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 1, 0);
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K, SOCK_DGRAM, 0, 1);
    bpf_statement(filter, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    bpf_statement(filter, BPF_RET | BPF_K, seccomp_errno_action());
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));
#else
    (void)filter;
#endif
}

void filter_prctl_syscall(std::vector<struct sock_filter> *filter) {
#ifdef __NR_prctl
    bpf_jump(filter, BPF_JMP | BPF_JEQ | BPF_K,
             static_cast<uint32_t>(__NR_prctl), 0, 4);
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        static_cast<uint32_t>(offsetof(struct seccomp_data, args[0])));
    bpf_jump(
        filter,
        BPF_JMP | BPF_JEQ | BPF_K,
        PR_GET_NO_NEW_PRIVS,
        0,
        1);
    bpf_statement(filter, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    bpf_statement(filter, BPF_RET | BPF_K, seccomp_errno_action());
    bpf_statement(
        filter,
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));
#else
    (void)filter;
#endif
}

bool install_seccomp(std::string *error) {
    std::vector<struct sock_filter> filter;
    bpf_statement(
        &filter, BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, arch));
#if defined(__x86_64__)
    bpf_jump(&filter, BPF_JMP | BPF_JEQ | BPF_K,
             AUDIT_ARCH_X86_64, 1, 0);
#elif defined(__aarch64__)
    bpf_jump(&filter, BPF_JMP | BPF_JEQ | BPF_K,
             AUDIT_ARCH_AARCH64, 1, 0);
#else
    if (error) {
        *error = "seccomp policy supports Linux x86_64 and aarch64 only";
    }
    return false;
#endif
    bpf_statement(&filter, BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    bpf_statement(
        &filter, BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));

#define CAPSID_DENY_SYSCALL(name) \
    do { \
        /* NOLINTNEXTLINE(bugprone-macro-parentheses) */ \
        deny_syscall(&filter, __NR_##name); \
    } while (0)
#ifdef __NR_accept
    CAPSID_DENY_SYSCALL(accept);
#endif
#ifdef __NR_accept4
    CAPSID_DENY_SYSCALL(accept4);
#endif
#ifdef __NR_bind
    CAPSID_DENY_SYSCALL(bind);
#endif
#ifdef __NR_listen
    CAPSID_DENY_SYSCALL(listen);
#endif
#ifdef __NR_clone
    CAPSID_DENY_SYSCALL(clone);
#endif
#ifdef __NR_clone3
    CAPSID_DENY_SYSCALL(clone3);
#endif
#ifdef __NR_fork
    CAPSID_DENY_SYSCALL(fork);
#endif
#ifdef __NR_vfork
    CAPSID_DENY_SYSCALL(vfork);
#endif
#ifdef __NR_execve
    CAPSID_DENY_SYSCALL(execve);
#endif
#ifdef __NR_execveat
    CAPSID_DENY_SYSCALL(execveat);
#endif
#ifdef __NR_ptrace
    CAPSID_DENY_SYSCALL(ptrace);
#endif
#ifdef __NR_process_vm_readv
    CAPSID_DENY_SYSCALL(process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
    CAPSID_DENY_SYSCALL(process_vm_writev);
#endif
#ifdef __NR_kill
    CAPSID_DENY_SYSCALL(kill);
#endif
#ifdef __NR_tkill
    CAPSID_DENY_SYSCALL(tkill);
#endif
#ifdef __NR_tgkill
    CAPSID_DENY_SYSCALL(tgkill);
#endif
#ifdef __NR_bpf
    CAPSID_DENY_SYSCALL(bpf);
#endif
#ifdef __NR_perf_event_open
    CAPSID_DENY_SYSCALL(perf_event_open);
#endif
#ifdef __NR_userfaultfd
    CAPSID_DENY_SYSCALL(userfaultfd);
#endif
#ifdef __NR_mount
    CAPSID_DENY_SYSCALL(mount);
#endif
#ifdef __NR_umount2
    CAPSID_DENY_SYSCALL(umount2);
#endif
#ifdef __NR_pivot_root
    CAPSID_DENY_SYSCALL(pivot_root);
#endif
#ifdef __NR_chroot
    CAPSID_DENY_SYSCALL(chroot);
#endif
#ifdef __NR_setns
    CAPSID_DENY_SYSCALL(setns);
#endif
#ifdef __NR_unshare
    CAPSID_DENY_SYSCALL(unshare);
#endif
#ifdef __NR_keyctl
    CAPSID_DENY_SYSCALL(keyctl);
#endif
#ifdef __NR_add_key
    CAPSID_DENY_SYSCALL(add_key);
#endif
#ifdef __NR_request_key
    CAPSID_DENY_SYSCALL(request_key);
#endif
#ifdef __NR_memfd_create
    CAPSID_DENY_SYSCALL(memfd_create);
#endif
#ifdef __NR_io_uring_setup
    CAPSID_DENY_SYSCALL(io_uring_setup);
#endif
#ifdef __NR_io_uring_enter
    CAPSID_DENY_SYSCALL(io_uring_enter);
#endif
#ifdef __NR_io_uring_register
    CAPSID_DENY_SYSCALL(io_uring_register);
#endif
#ifdef __NR_creat
    CAPSID_DENY_SYSCALL(creat);
#endif
#ifdef __NR_unlink
    CAPSID_DENY_SYSCALL(unlink);
#endif
#ifdef __NR_unlinkat
    CAPSID_DENY_SYSCALL(unlinkat);
#endif
#ifdef __NR_rename
    CAPSID_DENY_SYSCALL(rename);
#endif
#ifdef __NR_renameat
    CAPSID_DENY_SYSCALL(renameat);
#endif
#ifdef __NR_renameat2
    CAPSID_DENY_SYSCALL(renameat2);
#endif
#ifdef __NR_mkdir
    CAPSID_DENY_SYSCALL(mkdir);
#endif
#ifdef __NR_mkdirat
    CAPSID_DENY_SYSCALL(mkdirat);
#endif
#ifdef __NR_rmdir
    CAPSID_DENY_SYSCALL(rmdir);
#endif
#undef CAPSID_DENY_SYSCALL

#ifdef __NR_open
    filter_open_syscall(&filter, __NR_open, 1);
#endif
#ifdef __NR_openat
    filter_open_syscall(&filter, __NR_openat, 2);
#endif
#ifdef __NR_openat2
    /*
     * Capsid's read-only fs surface uses openat2 with
     * RESOLVE_NO_SYMLINKS. Landlock remains the path authority and denies
     * every write even though seccomp cannot inspect the pointed-to
     * open_how structure.
     */
    allow_syscall(&filter, __NR_openat2);
#endif
#ifdef __NR_mmap
    filter_executable_memory(&filter, __NR_mmap);
#endif
#ifdef __NR_mprotect
    filter_executable_memory(&filter, __NR_mprotect);
#endif
    filter_socket_syscall(&filter);
    filter_prctl_syscall(&filter);

#define CAPSID_ALLOW_SYSCALL(name) \
    do { allow_syscall(&filter, __NR_##name); } while (0)
#ifdef __NR_read
    CAPSID_ALLOW_SYSCALL(read);
#endif
#ifdef __NR_write
    CAPSID_ALLOW_SYSCALL(write);
#endif
#ifdef __NR_readv
    CAPSID_ALLOW_SYSCALL(readv);
#endif
#ifdef __NR_writev
    CAPSID_ALLOW_SYSCALL(writev);
#endif
#ifdef __NR_pread64
    CAPSID_ALLOW_SYSCALL(pread64);
#endif
#ifdef __NR_preadv
    CAPSID_ALLOW_SYSCALL(preadv);
#endif
#ifdef __NR_preadv2
    CAPSID_ALLOW_SYSCALL(preadv2);
#endif
#ifdef __NR_close
    CAPSID_ALLOW_SYSCALL(close);
#endif
#ifdef __NR_close_range
    CAPSID_ALLOW_SYSCALL(close_range);
#endif
#ifdef __NR_lseek
    CAPSID_ALLOW_SYSCALL(lseek);
#endif
#ifdef __NR_fstat
    CAPSID_ALLOW_SYSCALL(fstat);
#endif
#ifdef __NR_newfstatat
    CAPSID_ALLOW_SYSCALL(newfstatat);
#endif
#ifdef __NR_stat
    CAPSID_ALLOW_SYSCALL(stat);
#endif
#ifdef __NR_lstat
    CAPSID_ALLOW_SYSCALL(lstat);
#endif
#ifdef __NR_statx
    CAPSID_ALLOW_SYSCALL(statx);
#endif
#ifdef __NR_access
    CAPSID_ALLOW_SYSCALL(access);
#endif
#ifdef __NR_faccessat
    CAPSID_ALLOW_SYSCALL(faccessat);
#endif
#ifdef __NR_faccessat2
    CAPSID_ALLOW_SYSCALL(faccessat2);
#endif
#ifdef __NR_getdents64
    CAPSID_ALLOW_SYSCALL(getdents64);
#endif
#ifdef __NR_readlink
    CAPSID_ALLOW_SYSCALL(readlink);
#endif
#ifdef __NR_readlinkat
    CAPSID_ALLOW_SYSCALL(readlinkat);
#endif
#ifdef __NR_getcwd
    CAPSID_ALLOW_SYSCALL(getcwd);
#endif
#ifdef __NR_fcntl
    CAPSID_ALLOW_SYSCALL(fcntl);
#endif
#ifdef __NR_ioctl
    CAPSID_ALLOW_SYSCALL(ioctl);
#endif
#ifdef __NR_dup
    CAPSID_ALLOW_SYSCALL(dup);
#endif
#ifdef __NR_dup2
    CAPSID_ALLOW_SYSCALL(dup2);
#endif
#ifdef __NR_dup3
    CAPSID_ALLOW_SYSCALL(dup3);
#endif
#ifdef __NR_pipe
    CAPSID_ALLOW_SYSCALL(pipe);
#endif
#ifdef __NR_pipe2
    CAPSID_ALLOW_SYSCALL(pipe2);
#endif
#ifdef __NR_eventfd
    CAPSID_ALLOW_SYSCALL(eventfd);
#endif
#ifdef __NR_eventfd2
    CAPSID_ALLOW_SYSCALL(eventfd2);
#endif
#ifdef __NR_poll
    CAPSID_ALLOW_SYSCALL(poll);
#endif
#ifdef __NR_ppoll
    CAPSID_ALLOW_SYSCALL(ppoll);
#endif
#ifdef __NR_select
    CAPSID_ALLOW_SYSCALL(select);
#endif
#ifdef __NR_pselect6
    CAPSID_ALLOW_SYSCALL(pselect6);
#endif
#ifdef __NR_epoll_create
    CAPSID_ALLOW_SYSCALL(epoll_create);
#endif
#ifdef __NR_epoll_create1
    CAPSID_ALLOW_SYSCALL(epoll_create1);
#endif
#ifdef __NR_epoll_ctl
    CAPSID_ALLOW_SYSCALL(epoll_ctl);
#endif
#ifdef __NR_epoll_wait
    CAPSID_ALLOW_SYSCALL(epoll_wait);
#endif
#ifdef __NR_epoll_pwait
    CAPSID_ALLOW_SYSCALL(epoll_pwait);
#endif
#ifdef __NR_epoll_pwait2
    CAPSID_ALLOW_SYSCALL(epoll_pwait2);
#endif
#ifdef __NR_connect
    CAPSID_ALLOW_SYSCALL(connect);
#endif
#ifdef __NR_shutdown
    CAPSID_ALLOW_SYSCALL(shutdown);
#endif
#ifdef __NR_getsockname
    CAPSID_ALLOW_SYSCALL(getsockname);
#endif
#ifdef __NR_getpeername
    CAPSID_ALLOW_SYSCALL(getpeername);
#endif
#ifdef __NR_getsockopt
    CAPSID_ALLOW_SYSCALL(getsockopt);
#endif
#ifdef __NR_setsockopt
    CAPSID_ALLOW_SYSCALL(setsockopt);
#endif
#ifdef __NR_sendto
    CAPSID_ALLOW_SYSCALL(sendto);
#endif
#ifdef __NR_recvfrom
    CAPSID_ALLOW_SYSCALL(recvfrom);
#endif
#ifdef __NR_sendmsg
    CAPSID_ALLOW_SYSCALL(sendmsg);
#endif
#ifdef __NR_recvmsg
    CAPSID_ALLOW_SYSCALL(recvmsg);
#endif
#ifdef __NR_sendmmsg
    CAPSID_ALLOW_SYSCALL(sendmmsg);
#endif
#ifdef __NR_recvmmsg
    CAPSID_ALLOW_SYSCALL(recvmmsg);
#endif
#ifdef __NR_brk
    CAPSID_ALLOW_SYSCALL(brk);
#endif
#ifdef __NR_munmap
    CAPSID_ALLOW_SYSCALL(munmap);
#endif
#ifdef __NR_mremap
    CAPSID_ALLOW_SYSCALL(mremap);
#endif
#ifdef __NR_madvise
    CAPSID_ALLOW_SYSCALL(madvise);
#endif
#ifdef __NR_mincore
    CAPSID_ALLOW_SYSCALL(mincore);
#endif
#ifdef __NR_futex
    CAPSID_ALLOW_SYSCALL(futex);
#endif
#ifdef __NR_futex_waitv
    CAPSID_ALLOW_SYSCALL(futex_waitv);
#endif
#ifdef __NR_sched_yield
    CAPSID_ALLOW_SYSCALL(sched_yield);
#endif
#ifdef __NR_sched_getaffinity
    CAPSID_ALLOW_SYSCALL(sched_getaffinity);
#endif
#ifdef __NR_clock_gettime
    CAPSID_ALLOW_SYSCALL(clock_gettime);
#endif
#ifdef __NR_clock_getres
    CAPSID_ALLOW_SYSCALL(clock_getres);
#endif
#ifdef __NR_clock_nanosleep
    CAPSID_ALLOW_SYSCALL(clock_nanosleep);
#endif
#ifdef __NR_nanosleep
    CAPSID_ALLOW_SYSCALL(nanosleep);
#endif
#ifdef __NR_gettimeofday
    CAPSID_ALLOW_SYSCALL(gettimeofday);
#endif
#ifdef __NR_time
    CAPSID_ALLOW_SYSCALL(time);
#endif
#ifdef __NR_getrandom
    CAPSID_ALLOW_SYSCALL(getrandom);
#endif
#ifdef __NR_uname
    CAPSID_ALLOW_SYSCALL(uname);
#endif
#ifdef __NR_sysinfo
    CAPSID_ALLOW_SYSCALL(sysinfo);
#endif
#ifdef __NR_getpid
    CAPSID_ALLOW_SYSCALL(getpid);
#endif
#ifdef __NR_gettid
    CAPSID_ALLOW_SYSCALL(gettid);
#endif
#ifdef __NR_getuid
    CAPSID_ALLOW_SYSCALL(getuid);
#endif
#ifdef __NR_geteuid
    CAPSID_ALLOW_SYSCALL(geteuid);
#endif
#ifdef __NR_getgid
    CAPSID_ALLOW_SYSCALL(getgid);
#endif
#ifdef __NR_getegid
    CAPSID_ALLOW_SYSCALL(getegid);
#endif
#ifdef __NR_getppid
    CAPSID_ALLOW_SYSCALL(getppid);
#endif
#ifdef __NR_getrlimit
    CAPSID_ALLOW_SYSCALL(getrlimit);
#endif
#ifdef __NR_prlimit64
    CAPSID_ALLOW_SYSCALL(prlimit64);
#endif
#ifdef __NR_getrusage
    CAPSID_ALLOW_SYSCALL(getrusage);
#endif
#ifdef __NR_rt_sigaction
    CAPSID_ALLOW_SYSCALL(rt_sigaction);
#endif
#ifdef __NR_rt_sigprocmask
    CAPSID_ALLOW_SYSCALL(rt_sigprocmask);
#endif
#ifdef __NR_rt_sigreturn
    CAPSID_ALLOW_SYSCALL(rt_sigreturn);
#endif
#ifdef __NR_sigaltstack
    CAPSID_ALLOW_SYSCALL(sigaltstack);
#endif
#ifdef __NR_restart_syscall
    CAPSID_ALLOW_SYSCALL(restart_syscall);
#endif
#ifdef __NR_arch_prctl
    CAPSID_ALLOW_SYSCALL(arch_prctl);
#endif
#ifdef __NR_set_tid_address
    CAPSID_ALLOW_SYSCALL(set_tid_address);
#endif
#ifdef __NR_set_robust_list
    CAPSID_ALLOW_SYSCALL(set_robust_list);
#endif
#ifdef __NR_rseq
    CAPSID_ALLOW_SYSCALL(rseq);
#endif
#ifdef __NR_getcpu
    CAPSID_ALLOW_SYSCALL(getcpu);
#endif
#ifdef __NR_membarrier
    CAPSID_ALLOW_SYSCALL(membarrier);
#endif
#ifdef __NR_exit
    CAPSID_ALLOW_SYSCALL(exit);
#endif
#ifdef __NR_exit_group
    CAPSID_ALLOW_SYSCALL(exit_group);
#endif
#undef CAPSID_ALLOW_SYSCALL

    bpf_statement(&filter, BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    struct sock_fprog program;
    program.len = static_cast<unsigned short>(filter.size());
    program.filter = &filter[0];
    if (syscall(
            SYS_seccomp,
            SECCOMP_SET_MODE_FILTER,
            SECCOMP_FILTER_FLAG_TSYNC,
            &program) != 0) {
        return fail_errno("install seccomp filter", error);
    }
    return true;
}

#endif

}  // namespace

bool apply_sandbox(const SandboxConfig &config,
                   uint32_t *applied_features,
                   std::string *error) {
    uint32_t features = config.preinstalled_features;
    if (!set_limit("RLIMIT_CORE", RLIMIT_CORE, 0, 0, error)) {
        return false;
    }
    if (!set_limit("RLIMIT_NOFILE",
                   RLIMIT_NOFILE,
                   static_cast<rlim_t>(config.file_descriptor_limit),
                   static_cast<rlim_t>(config.file_descriptor_limit),
                   error)) {
        return false;
    }
#if defined(__APPLE__)
    if (config.address_space_limit != 0) {
        if (error) {
            *error =
                "process memory limit is unsupported on Darwin; use 0 or "
                "enforce a host-side resident-memory limit";
        }
        return false;
    }
#elif defined(RLIMIT_AS)
    if (config.address_space_limit &&
        !set_limit("RLIMIT_AS",
                   RLIMIT_AS,
                   static_cast<rlim_t>(config.address_space_limit),
                   static_cast<rlim_t>(config.address_space_limit),
                   error)) {
        return false;
    }
#endif
    features |= CAPSID_SANDBOX_FEATURE_RLIMITS;

    if (!config.strict) {
#if defined(__linux__)
        if (config.network_namespace_fd >= 0) {
            close(config.network_namespace_fd);
            if (error) {
                *error =
                    "network namespace requires strict sandbox mode";
            }
            return false;
        }
#endif
        if (applied_features) {
            *applied_features = features;
        }
        return true;
    }

#if defined(__linux__)
    if (!install_network_namespace(
            config.network_namespace_fd,
            config.required_features,
            &features,
            error)) {
        return false;
    }
    if (!install_namespaces(config.required_features, &features, error)) {
        return false;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return fail_errno("PR_SET_NO_NEW_PRIVS", error);
    }
    features |= CAPSID_SANDBOX_FEATURE_NO_NEW_PRIVS;
    if (!install_landlock(config.read_only_paths, error)) {
        return false;
    }
    features |= CAPSID_SANDBOX_FEATURE_LANDLOCK;
    if (!install_seccomp(error)) {
        return false;
    }
    features |= CAPSID_SANDBOX_FEATURE_SECCOMP;
    const uint32_t missing = config.required_features & ~features;
    if (missing != 0) {
        if (error) {
            char value[11];
            std::snprintf(value, sizeof(value), "0x%08x", missing);
            *error = std::string(
                "required sandbox features unavailable: ") + value;
        }
        return false;
    }
    if (applied_features) {
        *applied_features = features;
    }
    return true;
#else
    (void)config;
    if (error) {
        *error =
            "strict sandbox is unavailable on this platform/build";
    }
    return false;
#endif
}

}  // namespace capsid
