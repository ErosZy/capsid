// RED test for the request-end-after-response IPC interleaving.
//
// The Host may flush the request head and the request-end in separate IPC
// writes; the IPC is a SOCK_STREAM, so no ordering is guaranteed between
// those writes and the Runtime's reads. A synchronous handler response can
// therefore complete (and the Runtime erase the request) before the
// request-end frame arrives. The Runtime must treat that late frame — and
// any late request-body frame — as an idempotent no-op for a bounded set of
// terminal request ids, while frames for ids that never existed still fail
// closed and kill the worker.
//
// This test deliberately splits head and end across flushes and waits for
// RESPONSE_END in between, then verifies the worker survives and serves a
// second request.

#include "capsid/runtime.h"

#include "win32_compat.h"
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
#include <thread>

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

// Sends the request head and the request-end in separate flushes with a
// sleep in between: the Runtime completes the synchronous handler response
// (and erases the request) while the end frame is not yet on the wire. The
// end frame therefore always arrives after the Runtime forgot the id — the
// exact interleaving the terminal tombstone must tolerate. The client-side
// request state is only erased when next_event reads RESPONSE_END, so the
// end can still be queued here. Returns the response body; fails if the
// worker errors out or exits.
std::string run_split_request(capsid_worker *worker, uint64_t id) {
    require_result(
        capsid_worker_begin_request(worker, id, "GET",
                                    "https://example.test/sync", NULL, 0),
        "begin request");
    require_result(capsid_worker_flush(worker), "flush request head");

    // Give the Runtime time to process the head, run the synchronous
    // fixture and complete (and erase) the response without consuming any
    // event: consuming RESPONSE_END would erase the client-side state and
    // make the client ABI refuse the late end before it ever reaches the
    // worker.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    require_result(capsid_worker_end_request(worker, id), "end request");
    require_result(capsid_worker_flush(worker), "flush request end");

    bool received_head = false;
    bool received_end = false;
    std::string body;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!received_end) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("response flush: ") + capsid_result_string(flush));
        }

        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                if (event.request_id != id || event.status != 200) {
                    fail("unexpected response head");
                }
                received_head = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                if (!received_head || event.request_id != id) {
                    fail("response body arrived before its head");
                }
                body.append(reinterpret_cast<const char *>(event.payload.data),
                            event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, event.request_id,
                        static_cast<uint32_t>(event.payload.size)),
                    "replenish response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                if (!received_head || event.request_id != id) {
                    fail("unexpected response end");
                }
                received_end = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker error: ") +
                     std::string(reinterpret_cast<const char *>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited while the request was in flight");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("response event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for the response");
        }

        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
    return body;
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

    const std::string body = run_split_request(worker, 1);
    if (body != "sync-ok") {
        fail("unexpected first response body: " + body);
    }

    // The worker must survive the late request-end and serve a second
    // request (with a fresh id).
    const std::string second = run_split_request(worker, 2);
    if (second != "sync-ok") {
        fail("unexpected second response body: " + second);
    }

    // A frame for an id that never existed must still fail closed: the
    // client ABI refuses to queue it (CAPSID_INVALID_ARGUMENT) before any
    // frame reaches the worker. The Runtime-side rejection of truly unknown
    // ids is unchanged by the terminal tombstone.
    if (capsid_worker_end_request(worker, 999) != CAPSID_INVALID_ARGUMENT) {
        fail("end for a never-begun id was not rejected by the ABI");
    }

    capsid_worker_destroy(worker);
    std::cout << "PASS" << std::endl;
    return 0;
}
