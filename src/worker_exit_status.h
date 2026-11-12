#ifndef CAPSID_WORKER_EXIT_STATUS_H
#define CAPSID_WORKER_EXIT_STATUS_H

#include <cstdint>

struct capsid_worker;

namespace capsid {

// Internal C++ teardown result. IPC EOF and CAPSID_EVENT_EXIT do not carry
// an OS status. An unknown result must never be interpreted as exit zero.
struct WorkerExitStatus {
    bool observed = false;
    bool destroy_forced = false;
    std::uint32_t exit_code = 0;
    int signal = 0;
    std::uint32_t wait_error = 0;

    bool clean() const {
        return observed && !destroy_forced && wait_error == 0 &&
               signal == 0 && exit_code == 0;
    }
};

// Same ownership, synchronous abortive cleanup and exactly-once reap as the
// public destroy API. The natural-exit grace is bounded; after forced exit,
// wait for the actual OS result before returning. Captures status in that
// reap, without a competing waiter or second PID lookup. No C ABI changes.
WorkerExitStatus destroy_worker_with_status(capsid_worker* worker) noexcept;

}  // namespace capsid

#endif
