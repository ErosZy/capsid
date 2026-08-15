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
/* off_t: provided by MSVC sys/types.h (pulled in by io.h). */

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// MSVC errno.h has no ELOOP (its symlink-detection value is EDEADLOCK);
// the safe-read path uses ELOOP as its reparse-point rejection marker.
#ifndef ELOOP
#define ELOOP EDEADLOCK
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

// gettimeofday via the precision file-time clock (struct timeval comes
// from winsock2.h above). The timezone argument is ignored.
inline int gettimeofday(struct timeval *tv, void * /*timezone*/) {
    if (tv == NULL) {
        errno = EINVAL;
        return -1;
    }
    FILETIME file_time;
    GetSystemTimePreciseAsFileTime(&file_time);
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

// accept() on a CRT listener fd; the accepted socket becomes a CRT fd.
inline int accept_fd(int listener) {
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
