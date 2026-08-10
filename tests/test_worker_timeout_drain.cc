// RED test for the host hard-timeout terminal reason (§13.2).
//
// When the Host's hard timeout fires, the worker is SIGKILLed because it
// stopped answering: every request still inflight is gone with it and must
// receive a stable terminal reason — not just the first id in the request
// map. The client polls one event per call, so the remaining inflight ids
// must drain as successive CAPSID_EVENT_REQUEST_TIMEOUT events before the
// channel reports CAPSID_CLOSED.
//
// This test uses the stubborn native worker: it never reads the IPC socket
// and never answers, so every request reaches its host-side deadline and
// the client's hard-timeout path fires.

#include "capsid/runtime.h"

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
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

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("expected stubborn worker path");
    }
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.strict_sandbox = 0;
    // Small so the deadline (timeout + 250 ms grace) fires within the
    // test budget; the stubborn worker never answers either way.
    config.request_timeout_ms = 200;

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    require_result(
        capsid_worker_begin_bodyless_request(
            worker, 1, "GET", "https://example.test/hang", NULL, 0),
        "begin request 1");
    require_result(
        capsid_worker_begin_bodyless_request(
            worker, 2, "GET", "https://example.test/hang", NULL, 0),
        "begin request 2");
    require_result(capsid_worker_flush(worker), "flush requests");

    std::set<uint64_t> timed_out;
    std::set<uint64_t> seen;
    for (;;) {
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
                if (!seen.insert(event.request_id).second) {
                    fail("duplicate terminal event for request " +
                         std::to_string(event.request_id));
                }
                timed_out.insert(event.request_id);
                continue;
            }
            if (event.type == CAPSID_EVENT_EXIT ||
                event.type == CAPSID_EVENT_ERROR) {
                fail("unexpected event before the timeout drain");
            }
            continue;
        }
        if (result == CAPSID_WOULD_BLOCK) {
            struct pollfd descriptor = {};
            descriptor.fd = capsid_worker_fd(worker);
            descriptor.events = POLLIN;
            if (descriptor.fd >= 0) {
                poll(&descriptor, 1, 50);
            } else {
                usleep(50 * 1000);
            }
            continue;
        }
        if (result == CAPSID_CLOSED) {
            break;
        }
        fail(std::string("unexpected event result: ") +
             capsid_result_string(result));
    }

    if (timed_out != std::set<uint64_t>({1, 2})) {
        std::string actual;
        for (const uint64_t id : timed_out) {
            actual += std::to_string(id) + " ";
        }
        fail("hard timeout did not drain every inflight request; got: " +
             actual);
    }

    capsid_worker_destroy(worker);
    std::cout << "PASS" << std::endl;
    return 0;
}
