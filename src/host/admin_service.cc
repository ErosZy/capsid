// Long-lived M1D Admin service. See admin_service.h.

#include "host/admin_service.h"

#include "win32_compat.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <condition_variable>
#include <cstring>

namespace capsid::host {
namespace {

// Cross-platform stop pipe: BOTH ends get FD_CLOEXEC and O_NONBLOCK.
// Linux uses pipe2 for the atomic flags; other POSIX systems (macOS) use
// pipe + fcntl; Windows uses a loopback socket pair (WSAPoll cannot watch
// pipe handles), and any failure closes every fd created so far.
bool create_stop_pipe(int pipe_fds[2]) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
#if defined(_WIN32)
    if (capsid::win32::create_socket_pair(pipe_fds)) {
        return true;
    }
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    return false;
#elif defined(__linux__)
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
    if (start_gate_.exchange(true)) {
        if (error != nullptr) {
            *error = "admin service already started";
        }
        return false;
    }
    struct StartCompletion {
        AdminService* service;
        ~StartCompletion() { service->finish_start(); }
    } completion{this};
    if (stopping_.load()) {
        if (error != nullptr) {
            *error = "admin service stop was requested before start";
        }
        return false;
    }
    // Bind and listen BEFORE start returns (the caller must be able to
    // connect immediately).
    if (!open_admin_listener(options_.socket, &listener_, error)) {
        return false;
    }
    // Record the exact inode this service created; cleanup removes ONLY
    // that inode (a replaced pathname is left untouched). Windows cannot
    // identity-check socket inodes, so cleanup relies on listener_
    // ownership there (see wait()).
#if !defined(_WIN32)
    struct stat st = {};
    if (lstat(options_.socket.path.c_str(), &st) == 0) {
        socket_dev_ = st.st_dev;
        socket_ino_ = st.st_ino;
    }
#endif
    int created_stop_pipe[2] = {-1, -1};
    if (!create_stop_pipe(created_stop_pipe)) {
        close(listener_);
        listener_ = -1;
        if (error != nullptr) {
            *error = "cannot create admin stop pipe";
        }
        return false;
    }
    // A concurrent stop can race the pipe creation. Publish the wake after
    // the pipe exists so the accept loop cannot sleep forever on a stop that
    // arrived during startup.
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        stop_pipe_[0] = created_stop_pipe[0];
        stop_pipe_[1] = created_stop_pipe[1];
        if (stopping_.load() && !stop_written_.exchange(true)) {
            const ssize_t wake_bytes = write(stop_pipe_[1], "x", 1);
            (void)wake_bytes;
        }
    }
    if (stopping_.load(std::memory_order_acquire)) {
        close(listener_);
        listener_ = -1;
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        close(stop_pipe_[0]);
        close(stop_pipe_[1]);
        stop_pipe_[0] = -1;
        stop_pipe_[1] = -1;
        if (error != nullptr) {
            *error = "admin service stop was requested during start";
        }
        return false;
    }
    try {
        thread_ = std::thread(&AdminService::accept_loop, this);
    } catch (...) {
        close(listener_);
        listener_ = -1;
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        close(stop_pipe_[0]);
        close(stop_pipe_[1]);
        stop_pipe_[0] = -1;
        stop_pipe_[1] = -1;
        if (error != nullptr) {
            *error = "cannot start admin service thread";
        }
        return false;
    }
    if (stopping_.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (stop_pipe_[1] >= 0 && !stop_written_.exchange(true)) {
                const ssize_t wake_bytes = write(stop_pipe_[1], "x", 1);
                (void)wake_bytes;
            }
        }
        thread_.join();
        close(listener_);
        listener_ = -1;
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        close(stop_pipe_[0]);
        close(stop_pipe_[1]);
        stop_pipe_[0] = -1;
        stop_pipe_[1] = -1;
        if (error != nullptr) {
            *error = "admin service stop was requested during start";
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void AdminService::request_stop() {
    stopping_.store(true);
    // Wake the idle accept loop exactly once: the nonblocking pipe and the
    // one-shot gate make repeated stops idempotent and never blocking.
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (stop_pipe_[1] >= 0 && !stop_written_.exchange(true)) {
            // GCC's warn_unused_result ignores the (void) cast; assign and
            // ignore explicitly. The wake byte is best-effort by design.
            const ssize_t wake_bytes = write(stop_pipe_[1], "x", 1);
            (void)wake_bytes;
        }
    }
    // Wake an accepted connection mid-header: shutdown makes its read
    // return promptly instead of waiting out the HTTP deadline. The fd
    // lifecycle is mutex-protected, so this can never hit a descriptor
    // number that was closed and reused by the system.
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_fd_ >= 0) {
#if defined(_WIN32)
        (void)capsid::win32::shutdown_fd(active_fd_);
#else
        (void)shutdown(active_fd_, SHUT_RDWR);
#endif
    }
}

void AdminService::accept_loop() {
    for (;;) {
        capsid_pollfd descriptors[2];
        descriptors[0].fd = listener_;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = stop_pipe_[0];
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        int result = capsid::win32::capsid_poll(descriptors, 2, -1);
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
#if defined(_WIN32)
        const int fd = capsid::win32::accept_fd(listener_);
#else
        const int fd = accept(listener_, nullptr, nullptr);
#endif
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
    {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        if (!start_gate_.load(std::memory_order_acquire)) {
            if (error != nullptr) {
                *error = "admin service was not started";
            }
            return false;
        }
        lifecycle_cv_.wait(lock, [this] { return start_finished_; });
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    // Remove ONLY the socket inode this service created: a pathname that
    // was unlinked and replaced meanwhile is left untouched. Windows
    // cannot identity-check socket inodes; the listener was bound by this
    // service, so the unlink runs under listener_ ownership instead.
#if defined(_WIN32)
    if (listener_ >= 0) {
        (void)unlink(options_.socket.path.c_str());
    }
#else
    if (socket_dev_ != 0 && socket_ino_ != 0) {
        struct stat st = {};
        if (lstat(options_.socket.path.c_str(), &st) == 0 &&
            st.st_dev == socket_dev_ && st.st_ino == socket_ino_) {
            (void)unlink(options_.socket.path.c_str());
        }
    }
#endif
    if (listener_ >= 0) {
        close(listener_);
        listener_ = -1;
    }
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (stop_pipe_[0] >= 0) {
            close(stop_pipe_[0]);
            close(stop_pipe_[1]);
            stop_pipe_[0] = -1;
            stop_pipe_[1] = -1;
        }
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

}  // namespace capsid::host
