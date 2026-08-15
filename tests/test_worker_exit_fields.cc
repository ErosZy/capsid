// RED test for the EOF/EXIT event construction (§13.1).
//
// capsid_worker_next_event writes only the fields a given event needs;
// a caller that reuses one capsid_event across events (the documented
// polling pattern) will see stale flags/status/credit on events whose
// construction forgets to clear them. The REQUEST_TIMEOUT construction
// zeroes all three; the EOF/EXIT construction must do the same — the
// event the worker's death produces must never leak a prior event's
// error flags, status or credit to the owner.
//
// This test primes all three fields with sentinel values, kills the
// worker (closing the IPC socket with the worker, so the client's read
// loop reaches EOF) and asserts the EXIT event reports zeros.

#include "capsid/runtime.h"

#include "win32_compat.h"
#include <signal.h>
#include <sys/types.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open fixture: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void wait_for_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup flush: ") + capsid_result_string(flush));
        }

        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker startup error: ") +
                     std::string(reinterpret_cast<const char *>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for READY");
        }

        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        fail("expected worker path and JavaScript fixture path");
    }
    const std::string bundle = read_file(argv[2]);

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load bundle");
    wait_for_ready(worker);

    // A realistic reuser polls events into one struct; prime the fields the
    // EXIT construction must clear, as a prior event (e.g. an ERROR or a
    // REQUEST_TIMEOUT for another request) would have left them.
    capsid_event event = {};
    event.struct_size = sizeof(event);
    event.flags = 0x13579BDFu;
    event.status = 7;
    event.credit = 11;

    // Closing the worker kills the IPC socket peer; the client's read loop
    // sees EOF and constructs the EXIT event under test.
    const pid_t worker_pid = static_cast<pid_t>(capsid_worker_pid(worker));
    if (worker_pid <= 0) {
        fail("worker pid unavailable");
    }
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE,
                                 static_cast<DWORD>(worker_pid));
    if (process == NULL || !TerminateProcess(process, 1)) {
        if (process != NULL) {
            CloseHandle(process);
        }
        fail("kill worker");
    }
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
#else
    if (kill(worker_pid, SIGKILL) != 0) {
        fail("kill worker");
    }
#endif

    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("post-kill flush: ") +
                 capsid_result_string(flush));
        }
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type != CAPSID_EVENT_EXIT) {
                fail("expected EXIT after worker SIGKILL");
            }
            if (event.flags != 0) {
                fail("EXIT event leaked stale flags");
            }
            if (event.status != 0) {
                fail("EXIT event leaked stale status");
            }
            if (event.credit != 0) {
                fail("EXIT event leaked stale credit");
            }
            break;
        }
        if (result == CAPSID_WOULD_BLOCK) {
            capsid_pollfd descriptor = {};
            descriptor.fd = capsid_worker_fd(worker);
            descriptor.events = POLLIN;
            capsid::win32::capsid_poll(&descriptor, 1, 50);
            continue;
        }
        fail(std::string("post-kill event: ") + capsid_result_string(result));
    }

    capsid_worker_destroy(worker);
    std::cout << "PASS" << std::endl;
    return 0;
}
