// Long-lived M1D Admin service. See admin_service.h.

#include "host/admin_service.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace capsid::host {
namespace {

// Cross-platform stop pipe: BOTH ends get FD_CLOEXEC and O_NONBLOCK.
// Linux uses pipe2 for the atomic flags; other POSIX systems (macOS) use
// pipe + fcntl, and any failure closes every fd created so far.
bool create_stop_pipe(int pipe_fds[2]) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
#if defined(__linux__)
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0) {
        return true;
    }
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    return false;
#else
    if (pipe(pipe_fds) != 0) {
        return false;
    }
    for (int index = 0; index < 2; ++index) {
        const int fd_flags = fcntl(pipe_fds[index], F_GETFD);
        const int file_flags = fcntl(pipe_fds[index], F_GETFL);
        if (fd_flags < 0 || file_flags < 0 ||
            fcntl(pipe_fds[index], F_SETFD, fd_flags | FD_CLOEXEC) != 0 ||
            fcntl(pipe_fds[index], F_SETFL, file_flags | O_NONBLOCK) != 0) {
            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                close(pipe_fds[1]);
            }
            pipe_fds[0] = -1;
            pipe_fds[1] = -1;
            return false;
        }
    }
    return true;
#endif
}

}  // namespace

AdminService::AdminService(AdminServiceOptions options, AdminBackend* backend)
    : options_(std::move(options)), backend_(backend) {}

AdminService::~AdminService() {
    request_stop();
    std::string ignored;
    (void)wait(&ignored);
}

bool AdminService::start(std::string* error) {
    // Bind and listen BEFORE start returns (the caller must be able to
    // connect immediately).
    if (!open_admin_listener(options_.socket, &listener_, error)) {
        return false;
    }
    // Record the exact inode this service created; cleanup removes ONLY
    // that inode (a replaced pathname is left untouched).
    struct stat st = {};
    if (lstat(options_.socket.path.c_str(), &st) == 0) {
        socket_dev_ = st.st_dev;
        socket_ino_ = st.st_ino;
    }
    if (!create_stop_pipe(stop_pipe_)) {
        close(listener_);
        listener_ = -1;
        if (error != nullptr) {
            *error = "cannot create admin stop pipe";
        }
        return false;
    }
    stopping_.store(false);
    thread_ = std::thread(&AdminService::accept_loop, this);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void AdminService::request_stop() {
    stopping_.store(true);
    // Wake the idle accept loop exactly once: the nonblocking pipe and the
    // one-shot gate make repeated stops idempotent and never blocking.
    if (!stop_written_.exchange(true)) {
        // GCC's warn_unused_result ignores the (void) cast; assign and
        // ignore explicitly. The wake byte is best-effort by design.
        const ssize_t wake_bytes = write(stop_pipe_[1], "x", 1);
        (void)wake_bytes;
    }
    // Wake an accepted connection mid-header: shutdown makes its read
    // return promptly instead of waiting out the HTTP deadline. The fd
    // lifecycle is mutex-protected, so this can never hit a descriptor
    // number that was closed and reused by the system.
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_fd_ >= 0) {
        (void)shutdown(active_fd_, SHUT_RDWR);
    }
}

void AdminService::accept_loop() {
    for (;;) {
        struct pollfd descriptors[2];
        descriptors[0].fd = listener_;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = stop_pipe_[0];
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        int result = poll(descriptors, 2, -1);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (stopping_.load()) {
            break;
        }
        if (result < 0) {
            continue;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            // Stop requested; drain and exit.
            break;
        }
        if ((descriptors[0].revents & POLLIN) == 0) {
            continue;
        }
        const int fd = accept(listener_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (stopping_.load()) {
                break;
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_fd_ = fd;
        }
        std::string ignored;
        (void)serve_accepted_admin_http_connection(
            fd, options_.http, backend_, &ignored);
        {
            // Clear the active fd under the same lock request_stop uses,
            // THEN close: a concurrent stop either sees the live fd (and
            // shuts it down before this clear) or the cleared slot.
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_fd_ = -1;
            close(fd);
        }
        if (stopping_.load()) {
            break;
        }
    }
}

bool AdminService::wait(std::string* error) {
    if (thread_.joinable()) {
        thread_.join();
    }
    // Remove ONLY the socket inode this service created: a pathname that
    // was unlinked and replaced meanwhile is left untouched.
    if (socket_dev_ != 0 && socket_ino_ != 0) {
        struct stat st = {};
        if (lstat(options_.socket.path.c_str(), &st) == 0 &&
            st.st_dev == socket_dev_ && st.st_ino == socket_ino_) {
            (void)unlink(options_.socket.path.c_str());
        }
    }
    if (listener_ >= 0) {
        close(listener_);
        listener_ = -1;
    }
    if (stop_pipe_[0] >= 0) {
        close(stop_pipe_[0]);
        close(stop_pipe_[1]);
        stop_pipe_[0] = -1;
        stop_pipe_[1] = -1;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

}  // namespace capsid::host
