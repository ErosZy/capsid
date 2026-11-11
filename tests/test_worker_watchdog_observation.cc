#include "capsid/runtime.h"
#include "win32_compat.h"
#include "worker_exit_status.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(bool value, const char* message) {if (!value) throw std::runtime_error(message);}
void ok(capsid_result value, const char* message) {check(value == CAPSID_OK, message);}
struct Worker {
    capsid_worker* value = nullptr;
    ~Worker() {capsid_worker_destroy(value);}
};
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "stubborn worker and active/late");
        const bool late = std::string(argv[2]) == "late";
        check(late || std::string(argv[2]) == "active", "known control mode");
        Worker worker;
        capsid_worker_config config; capsid_worker_config_init(&config);
        config.worker_path = argv[1]; config.strict_sandbox = 0; config.request_timeout_ms = 200;
        ok(capsid_worker_spawn(&config, &worker.value), "spawn original stubborn native process");
        const auto start = std::chrono::steady_clock::now();
        ok(capsid_worker_begin_bodyless_request(worker.value, 1, "GET", "http://app.test/hang", nullptr, 0), "begin original timeout");
        ok(capsid_worker_flush(worker.value), "flush original request");
        if (late) std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const auto resumed = std::chrono::steady_clock::now();
        bool timed_out = false;
        for (;;) {
            capsid_event event{}; event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(worker.value, &event);
            if (result == CAPSID_OK) {
                check(event.type == CAPSID_EVENT_REQUEST_TIMEOUT && event.request_id == 1 &&
                    event.flags == 1 && event.status == 0 && event.credit == 0 && event.payload.size == 0,
                    "exact host hard-timeout terminal");
                timed_out = true; break;
            }
            check(result == CAPSID_WOULD_BLOCK, "no other terminal or protocol error");
            const auto elapsed = std::chrono::steady_clock::now() - start;
            check(elapsed < std::chrono::seconds(2), "stubborn process must not survive a refreshed watchdog");
            capsid_pollfd fd{}; fd.fd = capsid_worker_fd(worker.value); fd.events = POLLIN;
            capsid::win32::capsid_poll(&fd, 1, 5);
        }
        check(timed_out, "actual hard watchdog fired");
        const auto stopped = std::chrono::steady_clock::now();
        const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(stopped - start).count();
        const auto after_resume = std::chrono::duration_cast<std::chrono::milliseconds>(stopped - resumed).count();
        if (late) check(after_resume >= 250 && after_resume < 350, "one250ms drain grace after late resume");
        else check(total >= 450 && total < 550, "original200ms plus250ms active hard deadline");
        capsid_event end{}; end.struct_size = sizeof(end);
        check(capsid_worker_next_event(worker.value, &end) == CAPSID_CLOSED, "hard watchdog retires exactly one request");
        const auto cleanup = capsid::destroy_worker_with_status(worker.value); worker.value = nullptr;
        check(cleanup.observed && cleanup.wait_error == 0 && (cleanup.signal != 0 || cleanup.exit_code != 0),
            "actual OS process reaped after hard watchdog");
        std::printf("WATCHDOG_CLOCK {\"mode\":\"%s\",\"totalMs\":%lld,\"afterResumeMs\":%lld,\"nativeOsReaped\":true,\"exitCode\":%u,\"signal\":%d}\n",
            late ? "late" : "active", static_cast<long long>(total), static_cast<long long>(after_resume),
            cleanup.exit_code, cleanup.signal);
        return 0;
    } catch (const std::exception& error) {std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;}
}
