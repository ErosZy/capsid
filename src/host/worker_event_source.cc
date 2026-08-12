// WorkerEventSource implementation — see worker_event_source.h. This file
// is the only Host translation unit allowed to call capsid_worker_fd();
// cmake/build_host.cmake audits that boundary at configure time.

#include "host/worker_event_source.h"

#include "host/poll_limits.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <unistd.h>

namespace capsid::host {

namespace {

void write_stderr(std::string_view message) {
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
}

}  // namespace

WorkerEventSource::WorkerEventSource() {
    if (::pipe(wake_pipe_) != 0 ||
        fcntl(wake_pipe_[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(wake_pipe_[1], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK) != 0) {
        write_stderr("capsid-host: failed to create the command wake pipe");
        std::abort();
    }
}

WorkerEventSource::~WorkerEventSource() {
    if (wake_pipe_[0] != -1) {
        ::close(wake_pipe_[0]);
    }
    if (wake_pipe_[1] != -1) {
        ::close(wake_pipe_[1]);
    }
}

void WorkerEventSource::set_worker(capsid_worker* worker) {
    worker_ = worker;
    worker_fd_ = capsid_worker_fd(worker);
}

void WorkerEventSource::wake() {
    const char byte = 0;
    const ssize_t unused = ::write(wake_pipe_[1], &byte, 1);
    (void)unused;
}

void WorkerEventSource::drain_wake_bytes() {
    char buffer[64];
    while (::read(wake_pipe_[0], buffer, sizeof(buffer)) > 0) {
    }
}

bool WorkerEventSource::wait(
    std::optional<std::chrono::steady_clock::time_point> until) {
    return poll_worker(POLLIN, POLLIN, until) != PollResult::kError;
}

WorkerEventSource::WakeResult WorkerEventSource::wait_writable_or_wake(
    std::optional<std::chrono::steady_clock::time_point> until) {
    switch (poll_worker(POLLOUT, POLLIN, until)) {
    case PollResult::kWorker:
        return WakeResult::kWritable;
    case PollResult::kWake:
        return WakeResult::kWoken;
    case PollResult::kTimeout:
        return WakeResult::kTimedOut;
    case PollResult::kError:
        return WakeResult::kError;
    }
    return WakeResult::kError;
}

WorkerEventSource::PollResult WorkerEventSource::poll_worker(
    short worker_events,
    short wake_events,
    std::optional<std::chrono::steady_clock::time_point> until) {
    struct pollfd descriptors[2];
    descriptors[0].fd = worker_fd_;
    descriptors[0].events = worker_events;
    descriptors[0].revents = 0;
    descriptors[1].fd = wake_pipe_[0];
    descriptors[1].events = wake_events;
    descriptors[1].revents = 0;
    int timeout_ms = -1;
    if (until.has_value()) {
        const std::int64_t remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                *until - std::chrono::steady_clock::now())
                .count();
        timeout_ms = poll_timeout_ms(
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, remaining)));
    }
    for (;;) {
        const int result = ::poll(descriptors, 2, timeout_ms);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PollResult::kError;
        }
        if (result == 0) {
            return PollResult::kTimeout;
        }
        if ((descriptors[0].revents &
             (POLLIN | POLLOUT | POLLHUP | POLLERR)) != 0) {
            return PollResult::kWorker;
        }
        return PollResult::kWake;
    }
}

}  // namespace capsid::host
