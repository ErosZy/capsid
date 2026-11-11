#include "host/parent_stop_monitor.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace capsid::host {
namespace {
#if defined(_MSC_VER)
void invalid_fd(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                std::uintptr_t) {}

#if defined(_DEBUG)
thread_local bool validating_stop_descriptor = false;
std::mutex stop_descriptor_probe_mutex;
int invalid_fd_report(int report_type, char*, int* return_value) {
    if (!validating_stop_descriptor || report_type != _CRT_ASSERT) return FALSE;
    if (return_value) *return_value = 0;
    return TRUE;
}
int invalid_fd_report_wide(int report_type, wchar_t*, int* return_value) {
    return invalid_fd_report(report_type, nullptr, return_value);
}
#endif

intptr_t stop_descriptor_handle(int fd, bool* inspected) {
    *inspected = false;
#if defined(_DEBUG)
    // The CRT treats overlapping assertion reports as recursive failure,
    // even when they originate on different threads. Serialize our probes.
    std::lock_guard<std::mutex> lock(stop_descriptor_probe_mutex);
#endif
    const auto previous = _set_thread_local_invalid_parameter_handler(invalid_fd);
#if defined(_DEBUG)
    // The Debug CRT reports its parameter assertion before invoking the
    // invalid-parameter handler. Limit that report to this descriptor probe
    // on this thread, so a bad CLI descriptor returns its normal error.
    if (_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, invalid_fd_report) < 0) {
        _set_thread_local_invalid_parameter_handler(previous);
        return -1;
    }
    if (_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, invalid_fd_report_wide) < 0) {
        _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, invalid_fd_report);
        _set_thread_local_invalid_parameter_handler(previous);
        return -1;
    }
    const bool was_validating = validating_stop_descriptor;
    validating_stop_descriptor = true;
#endif
    const intptr_t handle = _get_osfhandle(fd);
#if defined(_DEBUG)
    validating_stop_descriptor = was_validating;
    _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE, invalid_fd_report_wide);
    _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, invalid_fd_report);
#endif
    _set_thread_local_invalid_parameter_handler(previous);
    *inspected = true;
    return handle;
}
#endif
}

std::unique_ptr<ParentStopMonitor> ParentStopMonitor::create(
    int fd, std::string* error) {
    int owned = -1;
#if defined(_WIN32)
#if defined(_MSC_VER)
    bool inspected = false;
    const intptr_t handle = stop_descriptor_handle(fd, &inspected);
    if (!inspected) {
        if (error) *error = "cannot inspect stop descriptor";
        return nullptr;
    }
#else
    const intptr_t handle = _get_osfhandle(fd);
#endif
    DWORD available = 0;
    const bool pipe = handle != -1 && handle != -2 &&
        GetFileType(reinterpret_cast<HANDLE>(handle)) == FILE_TYPE_PIPE;
    const BOOL peeked = pipe ? PeekNamedPipe(reinterpret_cast<HANDLE>(handle),
        nullptr, 0, nullptr, &available, nullptr) : FALSE;
    const DWORD code = peeked ? ERROR_SUCCESS : GetLastError();
    if (!pipe || (!peeked && code != ERROR_BROKEN_PIPE && code != ERROR_PIPE_NOT_CONNECTED)) {
        if (error) *error = "descriptor must be a readable pipe";
        return nullptr;
    }
    owned = _dup(fd);
    if (owned >= 0 && !SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(owned)), HANDLE_FLAG_INHERIT, 0)) {
        _close(owned); owned = -1;
    }
#else
    struct stat status {};
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY || fstat(fd, &status) != 0 ||
        (!S_ISFIFO(status.st_mode) && !S_ISSOCK(status.st_mode))) {
        if (error) *error = "descriptor must be a readable pipe or socket";
        return nullptr;
    }
    owned = fcntl(fd, F_DUPFD_CLOEXEC, 3);
#endif
    if (owned < 0) {
        if (error) *error = "cannot duplicate stop descriptor";
        return nullptr;
    }
    return std::unique_ptr<ParentStopMonitor>(new ParentStopMonitor(owned));
}

ParentStopMonitor::~ParentStopMonitor() {
    cancel();
#if defined(_WIN32)
    _close(fd_);
#else
    close(fd_);
#endif
}

bool ParentStopMonitor::start(std::function<void()> request_stop, std::string* error) {
    if (thread_.joinable() || cancelled_.load()) {
        if (error) *error = "parent stop monitor already used";
        return false;
    }
#if !defined(_WIN32)
    // The standalone pool owns SIGTERM/SIGINT. The watcher must inherit a
    // blocked mask even when it starts before the pool's run() method.
    sigset_t blocked, previous;
    sigemptyset(&blocked); sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
        if (error) *error = "cannot prepare parent stop signal mask";
        return false;
    }
#endif
    try {
        thread_ = std::thread([this, callback = std::move(request_stop)]() mutable {
            watch(std::move(callback));
        });
    } catch (const std::system_error& exception) {
#if !defined(_WIN32)
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#endif
        if (error) *error = exception.what();
        return false;
    }
#if !defined(_WIN32)
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#endif
    return true;
}

void ParentStopMonitor::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ParentStopMonitor::watch(std::function<void()> request_stop) {
    while (!cancelled_.load(std::memory_order_acquire)) {
        bool requested = false;
#if defined(_WIN32)
        DWORD available = 0;
        if (!PeekNamedPipe(reinterpret_cast<HANDLE>(_get_osfhandle(fd_)),
                nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD code = GetLastError();
            if (code != ERROR_BROKEN_PIPE && code != ERROR_PIPE_NOT_CONNECTED)
                error_ = "parent stop pipe failed: " + std::to_string(code);
            requested = true;
        } else requested = available != 0;
#else
        struct pollfd watched {fd_, POLLIN, 0};
        const int result = poll(&watched, 1, 50);
        if (result < 0) {
            if (errno == EINTR) continue;
            error_ = "parent stop poll failed: " + std::string(std::strerror(errno));
            requested = true;
        } else if (result > 0) {
            if (watched.revents & POLLNVAL) error_ = "parent stop descriptor became invalid";
            requested = true;
        }
#endif
        if (requested && !cancelled_.load(std::memory_order_acquire)) { request_stop(); return; }
#if defined(_WIN32)
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return cancelled_.load(); });
#endif
    }
}
}  // namespace capsid::host
