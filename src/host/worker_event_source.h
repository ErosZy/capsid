#ifndef CAPSID_HOST_WORKER_EVENT_SOURCE_H
#define CAPSID_HOST_WORKER_EVENT_SOURCE_H

#include "capsid/runtime.h"

#include <chrono>
#include <optional>

namespace capsid::host {

// WorkerEventSource: the only Host component allowed to call
// capsid_worker_fd() (design review §4.3; enforced by the source audit in
// cmake/build_host.cmake). The worker thread blocks on the worker IPC
// descriptor plus a command-wake pipe; the io thread writes one byte per
// empty→non-empty command-queue transition, so command pickup latency is
// bounded by poll latency instead of a polling period. The pipe is
// non-blocking and close-on-exec: a full pipe can never block the io
// thread, and a dropped byte is harmless because a drop can only happen
// while the worker is already draining.
class WorkerEventSource {
public:
    // Outcome of a writable wait (see wait_writable_or_wake).
    enum class WakeResult { kWritable, kWoken, kTimedOut, kError };

    WorkerEventSource();
    ~WorkerEventSource();
    WorkerEventSource(const WorkerEventSource&) = delete;
    WorkerEventSource& operator=(const WorkerEventSource&) = delete;

    // Called once after the worker is spawned; the sole capsid_worker_fd()
    // call site in the Host.
    void set_worker(capsid_worker* worker);

    // io thread: wake the worker thread out of wait(). A full pipe is not
    // an error; see the class comment.
    void wake();

    // worker thread: consume all pending wake bytes.
    void drain_wake_bytes();

    // worker thread: block until the worker IPC descriptor is readable, a
    // wake byte arrives, or `until` passes. Returns false only on a poll
    // error, which the worker thread treats as fatal.
    bool wait(std::optional<std::chrono::steady_clock::time_point> until);

    // worker thread: block until the worker IPC descriptor is writable, a
    // wake byte arrives, or `until` passes. kWritable means the flush can
    // proceed; kWoken means a command (kShutdown most likely) is waiting and
    // the caller must return to the command loop so it can run.
    WakeResult wait_writable_or_wake(
        std::optional<std::chrono::steady_clock::time_point> until);

private:
    enum class PollResult { kWorker, kWake, kTimeout, kError };

    PollResult poll_worker(short worker_events, short wake_events,
                           std::optional<std::chrono::steady_clock::time_point>
                               until);

    capsid_worker* worker_ = nullptr;
    int worker_fd_ = -1;  // from capsid_worker_fd(); the sole call site
    int wake_pipe_[2] = {-1, -1};
};

}  // namespace capsid::host

#endif
