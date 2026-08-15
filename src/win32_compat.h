/*
 * win32_compat.h — portability shims for the POSIX APIs Capsid's runtime
 * sources use on fd-based IPC, sockets and process primitives.
 *
 * Windows rules for this header:
 *   - winsock2.h must precede windows.h, so include this header FIRST in
 *     any translation unit that also includes windows.h (directly or via
 *     libuv/txiki headers). WIN32_LEAN_AND_MEAN and NOMINMAX keep the
 *     windows.h surface from leaking min/max macros into std:: code.
 *   - CRT descriptors on Windows are NOT Winsock sockets: winsock calls
 *     (send/recv/accept/shutdown/ioctlsocket) take the raw SOCKET handle.
 *     The *_fd helpers below convert CRT fd <-> SOCKET, so callers must
 *     use them instead of calling winsock directly with an int fd.
 *   - This header is PRIVATE to src/ and tools/. It must never leak into
 *     include/capsid (the public ABI stays platform-neutral).
 *
 * On POSIX the helpers degrade to their standard equivalents, so shared
 * call sites can use them unconditionally when the header is included on
 * every platform (host sources do this; the runtime core includes it only
 * under _WIN32 to keep the Linux build surface unchanged).
 */
#ifndef CAPSID_SRC_WIN32_COMPAT_H
#define CAPSID_SRC_WIN32_COMPAT_H

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstring>
#include <vector>

typedef SSIZE_T ssize_t;
#ifndef socklen_t
typedef int socklen_t;
#endif
#ifndef pid_t
typedef DWORD pid_t;
#endif
#ifndef mode_t
typedef unsigned int mode_t;
#endif
#ifndef uid_t
typedef unsigned int uid_t;
#endif
#ifndef gid_t
typedef unsigned int gid_t;
#endif
/* off_t: provided by MSVC sys/types.h (pulled in by io.h). */

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// MSVC errno.h has no ELOOP (its symlink-detection value is EDEADLOCK);
// the safe-read path uses ELOOP as its reparse-point rejection marker.
#ifndef ELOOP
#define ELOOP EDEADLOCK
#endif

// waitpid-status accessors: Windows has no signal deaths, so the status
// value IS the exit code (see graceful_worker_exit.h, which stores the
// GetExitCodeProcess result there).
#ifndef WIFSIGNALED
#define WIFSIGNALED(status) 0
#endif
#ifndef WTERMSIG
#define WTERMSIG(status) 0
#endif
#ifndef WIFEXITED
#define WIFEXITED(status) 1
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (status)
#endif

// rmdir lives in the CRT as _rmdir only.
#ifndef rmdir
#define rmdir _rmdir
#endif

// mkdtemp for test scaffolding: replaces the XXXXXX suffix with unique
// characters and creates the directory (the POSIX contract).
inline char *mkdtemp(char *path_template) {
    const errno_t err =
        _mktemp_s(path_template, std::strlen(path_template) + 1);
    if (err != 0) {
        errno = err;
        return nullptr;
    }
    if (_mkdir(path_template) != 0) {
        return nullptr;
    }
    return path_template;
}

// access() constants: MSVC io.h defines only F_OK.
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif

namespace capsid {
namespace win32 {

// Refcounted WSAStartup; safe to call from every thread and process that
// touches Winsock sockets or CRT wrappers over sockets.
inline bool ensure_winsock() {
    static const bool initialized = []() -> bool {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

// CRT opens default to text mode on Windows; binary mode makes open()/
// fopen() byte-transparent so IPC frames and artifacts are never subject
// to CRLF translation.
inline void set_binary_file_defaults() {
    _set_fmode(_O_BINARY);
}

inline int getpid() {
    return static_cast<int>(_getpid());
}

inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

// _putenv with a bare NAME (no '=') removes the variable — the CRT's
// documented removal form.
inline int unsetenv(const char *name) {
    return _putenv(name) == 0 ? 0 : -1;
}

// usleep: millisecond-resolution sleep (test scaffolding; the ceiling
// round matches POSIX "sleep at least this long").
inline int usleep(unsigned long usec) {
    Sleep((usec + 999u) / 1000u);
    return 0;
}

// gettimeofday via the precision file-time clock (struct timeval comes
// from winsock2.h above). The timezone argument is ignored.
inline int gettimeofday(struct timeval *tv, void * /*timezone*/) {
    if (tv == NULL) {
        errno = EINVAL;
        return -1;
    }
    FILETIME file_time;
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    const unsigned long long unix_epoch_offset = 116444736000000000ull;
    const unsigned long long usecs =
        (value.QuadPart - unix_epoch_offset) / 10ull;
    tv->tv_sec = static_cast<long>(usecs / 1000000ull);
    tv->tv_usec = static_cast<long>(usecs % 1000000ull);
    return 0;
}

// Process-identity checks are uid-based on POSIX; Windows has no uid
// namespace for AF_UNIX peers and relies on the socket directory's NTFS
// ACLs instead (see docs/windows.md).
inline unsigned long long geteuid() {
    return 0ull;
}

// Non-blocking mode for a CRT fd backed by a socket handle.
inline bool set_socket_nonblocking(int fd) {
    if (!ensure_winsock()) {
        return false;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        return false;
    }
    u_long mode = 1;
    return ioctlsocket(socket_handle, FIONBIO, &mode) == 0;
}

// Maps a winsock failure to errno so callers can keep their
// errno == EINTR / EAGAIN checks shared with POSIX. WSAEWOULDBLOCK maps
// to EAGAIN (what a nonblocking read would report on POSIX).
inline void map_winsock_errno() {
    const int error = WSAGetLastError();
    switch (error) {
        case WSAEINTR: errno = EINTR; break;
        case WSAEWOULDBLOCK: errno = EAGAIN; break;
        case WSAENOTSOCK: errno = EBADF; break;
        case WSAEADDRINUSE: errno = EADDRINUSE; break;
        case WSAECONNREFUSED: errno = ECONNREFUSED; break;
        case WSAEACCES: errno = EACCES; break;
        case WSAEADDRNOTAVAIL: errno = EADDRNOTAVAIL; break;
        case WSAEINVAL: errno = EINVAL; break;
        default: errno = EIO; break;
    }
}

// send() on a CRT fd (the fd must wrap a socket).
inline ssize_t send_fd(int fd,
                       const void *data,
                       size_t size,
                       int flags) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int sent = send(
        socket_handle,
        static_cast<const char *>(data),
        static_cast<int>(size),
        flags);
    if (sent == SOCKET_ERROR) {
        map_winsock_errno();
        return -1;
    }
    return static_cast<ssize_t>(sent);
}

// write() on a CRT fd that may wrap EITHER a socket (the READY/wake
// channels are socketpairs) or a real pipe (a harness may pass a stdio
// pipe as the READY fd). Winsock send() rejects pipe handles with
// WSAENOTSOCK (mapped to EBADF); fall back to the CRT for those.
inline ssize_t write_any_fd(int fd, const void *data, size_t size) {
    const ssize_t via_socket = send_fd(fd, data, size, 0);
    if (via_socket >= 0 || errno != EBADF) {
        return via_socket;
    }
    return _write(fd, data, static_cast<unsigned int>(size));
}

// accept() on a CRT listener fd; the accepted socket becomes a CRT fd.
inline int accept_fd(int listener) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET listener_socket =
        static_cast<SOCKET>(_get_osfhandle(listener));
    if (listener_socket == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const SOCKET accepted = accept(listener_socket, NULL, NULL);
    if (accepted == INVALID_SOCKET) {
        map_winsock_errno();
        return -1;
    }
    // SOCKET is an integer handle type: integer-to-integer cast.
    return _open_osfhandle(
        static_cast<intptr_t>(accepted), _O_RDWR | _O_BINARY);
}

// shutdown(SHUT_RDWR) on a CRT fd.
inline int shutdown_fd(int fd) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int result = shutdown(socket_handle, SD_BOTH);
    if (result != 0) {
        map_winsock_errno();
    }
    return result;
}

// Loopback TCP socket pair replacing socketpair(AF_UNIX, SOCK_STREAM).
// fds[0] and fds[1] are CRT descriptors; neither handle is inheritable.
// Windows pipe() handles cannot be polled by WSAPoll, so stop/wake
// channels must use this pair instead of pipe().
inline bool create_socket_pair(int fds[2]) {
    if (!ensure_winsock()) {
        return false;
    }
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return false;
    }
    SOCKET child = INVALID_SOCKET;
    SOCKET parent = INVALID_SOCKET;
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t address_size = sizeof(address);
    const bool ok =
        bind(listener,
             reinterpret_cast<const struct sockaddr *>(&address),
             sizeof(address)) == 0 &&
        listen(listener, 1) == 0 &&
        getsockname(listener,
                    reinterpret_cast<struct sockaddr *>(&address),
                    &address_size) == 0 &&
        (child = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) !=
            INVALID_SOCKET &&
        connect(child,
                reinterpret_cast<const struct sockaddr *>(&address),
                sizeof(address)) == 0 &&
        (parent = accept(listener, NULL, NULL)) != INVALID_SOCKET;
    // The accepted socket must be re-bound to the listener's context
    // before the listener closes: AFD otherwise recycles the listener's
    // endpoint name and the accepted socket's I/O collides with it
    // (ERROR_ALREADY_EXISTS) once the teardown lands.
    setsockopt(parent, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
               reinterpret_cast<const char *>(&listener),
               sizeof(listener));
    closesocket(listener);
    if (!ok) {
        if (child != INVALID_SOCKET) {
            closesocket(child);
        }
        if (parent != INVALID_SOCKET) {
            closesocket(parent);
        }
        return false;
    }
    fds[0] = _open_osfhandle(
        static_cast<intptr_t>(parent), _O_RDWR | _O_BINARY);
    fds[1] = _open_osfhandle(
        static_cast<intptr_t>(child), _O_RDWR | _O_BINARY);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) {
            close(fds[0]);
        }
        if (fds[1] >= 0) {
            close(fds[1]);
        }
        return false;
    }
    return true;
}

}  // namespace win32
}  // namespace capsid

#endif  // _WIN32

// ---- socket-fd wrappers (both platforms) ---------------------------------
// Tests and host sources drive sockets through CRT fds; these wrappers
// translate to raw SOCKET handles on Windows and pass through on POSIX,
// so call sites stay platform-neutral.

#if defined(_WIN32)

namespace capsid {
namespace win32 {

// socket(family, SOCK_STREAM, 0) as a CRT fd.
inline int create_socket_fd(int family) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET handle = socket(family, SOCK_STREAM, 0);
    if (handle == INVALID_SOCKET) {
        map_winsock_errno();
        return -1;
    }
    return _open_osfhandle(
        static_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
}

// socket(AF_INET, SOCK_STREAM, 0) as a CRT fd.
inline int create_tcp_socket_fd() {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET handle = socket(AF_INET, SOCK_STREAM, 0);
    if (handle == INVALID_SOCKET) {
        map_winsock_errno();
        return -1;
    }
    return _open_osfhandle(
        static_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
}

inline int connect_fd(int fd, const struct sockaddr *address,
                      socklen_t address_size) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int result = connect(socket_handle, address, address_size);
    if (result != 0) {
        map_winsock_errno();
    }
    return result;
}

inline int bind_fd(int fd, const struct sockaddr *address,
                   socklen_t address_size) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int result = bind(socket_handle, address, address_size);
    if (result != 0) {
        map_winsock_errno();
    }
    return result;
}

inline int listen_fd(int fd, int backlog) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int result = listen(socket_handle, backlog);
    if (result != 0) {
        map_winsock_errno();
    }
    return result;
}

inline int getsockname_fd(int fd, struct sockaddr *address,
                          socklen_t *address_size) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int result = getsockname(socket_handle, address, address_size);
    if (result != 0) {
        map_winsock_errno();
    }
    return result;
}

inline ssize_t recv_fd(int fd, void *buffer, size_t size, int flags) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int received = recv(
        socket_handle,
        static_cast<char *>(buffer),
        static_cast<int>(size),
        flags);
    if (received == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        // A hard peer death surfaces as RST on Windows (POSIX read()
        // reports EOF for the same event); the IPC channel and the HTTP
        // test clients treat it as a closed stream.
        if (error == WSAECONNRESET || error == WSAENOTCONN ||
            error == WSAESHUTDOWN) {
            return 0;
        }
        map_winsock_errno();
        return -1;
    }
    return static_cast<ssize_t>(received);
}

// read() on a CRT fd: Winsock sockets must go through recv — the CRT's
// own read() does not handle _open_osfhandle socket fds (EINVAL).
inline ssize_t read_fd(int fd, void *buffer, size_t size) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const intptr_t osfhandle = _get_osfhandle(fd);
    if (osfhandle < 0) {
        errno = EBADF;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(osfhandle);
    const int received = recv(
        socket_handle,
        static_cast<char *>(buffer),
        static_cast<int>(size),
        0);
    if (received == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        // A hard peer death surfaces as RST on Windows (POSIX read()
        // reports EOF for the same event); the IPC channel and the HTTP
        // test clients treat it as a closed stream.
        if (error == WSAECONNRESET || error == WSAENOTCONN ||
            error == WSAESHUTDOWN) {
            return 0;
        }
        map_winsock_errno();
        return -1;
    }
    return static_cast<ssize_t>(received);
}

// SO_REUSEADDR on a CRT fd (Windows takes the option value as a char
// pointer; POSIX as an int pointer).
inline int setsockopt_reuseaddr_fd(int fd) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const int reuse = 1;
    const int result = setsockopt(
        socket_handle, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    if (result != 0) {
        map_winsock_errno();
    }
    return result;
}

// SO_RCVTIMEO as a millisecond budget (Windows takes DWORD ms; POSIX
// takes struct timeval).
inline int setsockopt_recv_timeout_fd(int fd, unsigned timeout_ms) {
    if (!ensure_winsock()) {
        errno = EIO;
        return -1;
    }
    const SOCKET socket_handle = static_cast<SOCKET>(_get_osfhandle(fd));
    if (socket_handle == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    const DWORD ms = static_cast<DWORD>(timeout_ms);
    const int result = setsockopt(
        socket_handle, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char *>(&ms), sizeof(ms));
    if (result != 0) {
        map_winsock_errno();
    }
    return result;
}

}  // namespace win32
}  // namespace capsid

#else  // POSIX passthroughs

// The passthroughs below call the socket/process APIs directly, and this
// header is included first by tests and host sources on every platform;
// bring in the POSIX declarations the call sites rely on.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace capsid {
namespace win32 {

inline bool create_socket_pair(int fds[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
}

inline int create_socket_fd(int family) {
    return socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
}

inline int create_tcp_socket_fd() {
    return socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
}

inline int connect_fd(int fd, const struct sockaddr *address,
                      socklen_t address_size) {
    return connect(fd, address, address_size);
}

inline int bind_fd(int fd, const struct sockaddr *address,
                   socklen_t address_size) {
    return bind(fd, address, address_size);
}

inline int listen_fd(int fd, int backlog) {
    return listen(fd, backlog);
}

inline int getsockname_fd(int fd, struct sockaddr *address,
                          socklen_t *address_size) {
    return getsockname(fd, address, address_size);
}

inline ssize_t recv_fd(int fd, void *buffer, size_t size, int flags) {
    return recv(fd, buffer, size, flags);
}

inline ssize_t read_fd(int fd, void *buffer, size_t size) {
    return read(fd, buffer, size);
}

inline int setsockopt_reuseaddr_fd(int fd) {
    const int reuse = 1;
    return setsockopt(
        fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

inline int setsockopt_recv_timeout_fd(int fd, unsigned timeout_ms) {
    struct timeval timeout = {};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000u);
    timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000u) * 1000u);
    return setsockopt(
        fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

}  // namespace win32
}  // namespace capsid

#endif  // _WIN32

// ---- poll() compatibility ------------------------------------------------
// Shared call sites use capsid_poll/capsid_pollfd. On Windows the shim is
// WSAPoll over CRT socket fds (pipe handles are not supported — use
// capsid::win32::create_socket_pair for stop channels). On POSIX it is
// plain poll(). POLLIN/POLLOUT bits are value-identical across the two
// implementations for the flags both define.
#if defined(_WIN32)
struct capsid_pollfd {
    intptr_t fd;
    short events;
    short revents;
};
namespace capsid {
namespace win32 {
inline int capsid_poll(capsid_pollfd *fds,
                       size_t count,
                       int timeout_ms) {
    std::vector<WSAPOLLFD> descriptors(count);
    for (size_t index = 0; index < count; ++index) {
        descriptors[index].fd = static_cast<SOCKET>(
            _get_osfhandle(static_cast<int>(fds[index].fd)));
        descriptors[index].events = fds[index].events;
        descriptors[index].revents = 0;
    }
    const int result = WSAPoll(
        descriptors.empty() ? NULL : &descriptors[0],
        static_cast<ULONG>(count),
        timeout_ms);
    if (result == SOCKET_ERROR) {
        map_winsock_errno();
        return -1;
    }
    for (size_t index = 0; index < count; ++index) {
        fds[index].revents = descriptors[index].revents;
    }
    return result;
}
}  // namespace win32
}  // namespace capsid
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
typedef struct pollfd capsid_pollfd;
namespace capsid {
namespace win32 {
inline int capsid_poll(capsid_pollfd *fds,
                       size_t count,
                       int timeout_ms) {
    return poll(fds, static_cast<nfds_t>(count), timeout_ms);
}
}  // namespace win32
}  // namespace capsid
#endif

#endif  // CAPSID_SRC_WIN32_COMPAT_H
