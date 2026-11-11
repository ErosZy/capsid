#ifndef CAPSID_TESTS_GRACEFUL_WORKER_EXIT_H
#define CAPSID_TESTS_GRACEFUL_WORKER_EXIT_H

#include "capsid/runtime.h"

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <thread>

namespace capsid_test {

struct GracefulWorkerExit {
    GracefulWorkerExit()
        : shutdown_result(CAPSID_OK),
          flush_result(CAPSID_OK),
          event_result(CAPSID_OK),
          channel_closed(false),
          error_events(0),
          reaped(false),
          wait_error(0),
          status(0) {}

    capsid_result shutdown_result;
    capsid_result flush_result;
    capsid_result event_result;
    bool channel_closed;
    uint32_t error_events;
    // Legacy name: on Windows this means the process was observed exited;
    // destroy still owns its process handle. POSIX waitpid reaps the child.
    bool reaped;
    int wait_error;
    int status;

    bool clean() const {
        return shutdown_result == CAPSID_OK && flush_result == CAPSID_OK &&
               event_result == CAPSID_OK && channel_closed && error_events == 0 &&
               reaped && wait_error == 0 && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0;
    }
};

inline GracefulWorkerExit wait_for_worker_exit(
    capsid_worker *worker,
    uint32_t timeout_ms,
    bool request_shutdown) {
    GracefulWorkerExit result;
    const int64_t worker_pid = capsid_worker_pid(worker);
    if (worker_pid <= 0) {
        result.wait_error = EINVAL;
        return result;
    }

    // Pin process identity before requesting exit. Reopening a numeric PID
    // while polling can observe a different process after PID reuse.
#if defined(_WIN32)
    HANDLE process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(worker_pid));
    if (process == NULL) {
        result.wait_error = EINVAL;
        return result;
    }
    struct ProcessHandle {
        HANDLE value;
        ~ProcessHandle() { CloseHandle(value); }
    } process_handle{process};
#endif

    result.shutdown_result = request_shutdown ? capsid_worker_shutdown(worker) : CAPSID_OK;
    if (result.shutdown_result != CAPSID_OK) {
        return result;
    }

    bool flushed = !request_shutdown;
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

        // Drain output while waiting: a Worker must not deadlock behind a
        // full IPC send buffer. Bound each batch so a noisy peer cannot
        // prevent checking the absolute deadline or actual process status.
        for (unsigned i = 0; i < 64 && !result.channel_closed; ++i) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const capsid_result next = capsid_worker_next_event(worker, &event);
            if (next == CAPSID_WOULD_BLOCK) break;
            if (next == CAPSID_CLOSED ||
                (next == CAPSID_OK && event.type == CAPSID_EVENT_EXIT)) {
                result.channel_closed = true;
                break;
            }
            if (next != CAPSID_OK) {
                result.event_result = next;
                return result;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_REQUEST_TIMEOUT) ++result.error_events;
        }

        if (!result.reaped) {
#if defined(_WIN32)
            const DWORD waited = WaitForSingleObject(process, 0);
            if (waited == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                if (!GetExitCodeProcess(process, &exit_code)) {
                    result.wait_error = EIO;
                    return result;
                }
                result.reaped = true;
                result.status = static_cast<int>(exit_code);
            } else if (waited == WAIT_FAILED) {
                result.wait_error = EIO;
                return result;
            }
#else
            int status = 0;
            const pid_t waited =
                waitpid(static_cast<pid_t>(worker_pid), &status, WNOHANG);
            if (waited == static_cast<pid_t>(worker_pid)) {
                result.reaped = true;
                result.status = status;
            }
            if (waited < 0 && errno != EINTR) {
                result.wait_error = errno;
                return result;
            }
#endif
        }
        if (result.reaped && result.channel_closed) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return result;
}

inline GracefulWorkerExit shutdown_and_wait(capsid_worker *worker,
                                           uint32_t timeout_ms) {
    return wait_for_worker_exit(worker, timeout_ms, true);
}

}  // namespace capsid_test

#endif
