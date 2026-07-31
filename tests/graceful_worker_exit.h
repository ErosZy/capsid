#ifndef CAPSID_TESTS_GRACEFUL_WORKER_EXIT_H
#define CAPSID_TESTS_GRACEFUL_WORKER_EXIT_H

#include "capsid/runtime.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <thread>

namespace capsid_test {

struct GracefulWorkerExit {
    GracefulWorkerExit()
        : shutdown_result(CAPSID_OK),
          flush_result(CAPSID_OK),
          reaped(false),
          wait_error(0),
          status(0) {}

    capsid_result shutdown_result;
    capsid_result flush_result;
    bool reaped;
    int wait_error;
    int status;
};

inline GracefulWorkerExit shutdown_and_wait(
    capsid_worker *worker,
    uint32_t timeout_ms) {
    GracefulWorkerExit result;
    const int64_t worker_pid = capsid_worker_pid(worker);
    if (worker_pid <= 0) {
        result.wait_error = EINVAL;
        return result;
    }

    result.shutdown_result = capsid_worker_shutdown(worker);
    if (result.shutdown_result != CAPSID_OK) {
        return result;
    }

    bool flushed = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!flushed) {
            result.flush_result = capsid_worker_flush(worker);
            if (result.flush_result == CAPSID_OK) {
                flushed = true;
            } else if (result.flush_result != CAPSID_WOULD_BLOCK) {
                return result;
            }
        }

        int status = 0;
        const pid_t waited =
            waitpid(static_cast<pid_t>(worker_pid), &status, WNOHANG);
        if (waited == static_cast<pid_t>(worker_pid)) {
            result.reaped = true;
            result.status = status;
            return result;
        }
        if (waited < 0 && errno != EINTR) {
            result.wait_error = errno;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return result;
}

}  // namespace capsid_test

#endif
