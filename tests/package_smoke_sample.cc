// Package smoke sample (remediation spec §12.4): the C++ companion to
// package_smoke_sample.c. Compiled against ONLY the extracted package's
// public headers (include/capsid/runtime.hpp) and static runtime library.
//
// Exercises the C++ surface: recommended_worker_count(), available_cpus(),
// CapabilityPolicyBuilder (application identity + module allowlist), then
// the same worker round trip through the C API with the built policy wired
// into the config — proving a consumer can build a policy and hand it to
// capsid_worker_spawn() without the build tree.
//
// Compiled with a strict -std=c++17, where glibc hides poll()/clock_gettime()
// unless the POSIX feature macro is requested (macOS exposes them
// unconditionally). The macro must precede every include.

#define _POSIX_C_SOURCE 200112L

#include "capsid/runtime.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

double now_seconds() {
#if defined(_WIN32)
    /* GetTickCount64 is monotonic at millisecond resolution, which the
     * deadline pacing here only needs as a coarse clock. */
    return static_cast<double>(GetTickCount64()) / 1000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fail("clock_gettime");
    }
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) / 1000000000.0;
#endif
}

std::string read_file(const char *path) {
    FILE *handle = std::fopen(path, "rb");
    if (handle == nullptr) {
        fail("cannot open bundle");
    }
    std::string content;
    char buffer[4096];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
        content.append(buffer, count);
    }
    std::fclose(handle);
    return content;
}

// Blocks until the next event is available and returns CAPSID_OK with the
// event filled; the event payload stays valid until the next call, so
// callers must handle the payload before calling again.
capsid_result wait_for_event(capsid_worker *worker, double deadline,
                             capsid_event *event) {
    for (;;) {
        capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail("flush failed");
        }
        memset(event, 0, sizeof(*event));
        event->struct_size = sizeof(*event);
        capsid_result result = capsid_worker_next_event(worker, event);
        if (result == CAPSID_OK) {
            return CAPSID_OK;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail("event error");
        }
        if (now_seconds() >= deadline) {
            fail("timed out waiting for event");
        }
#if defined(_WIN32)
        /* next_event above polls the channel; the sleep only paces the
         * loop, so a plain Sleep stands in for poll(). */
        Sleep(50);
#else
        struct pollfd descriptor;
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
#endif
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <capsid-worker> <bundle.mjs>\n",
                     argv[0]);
        return 2;
    }
    const char *worker_path = argv[1];
    const char *bundle_path = argv[2];

    // C++ topology surface. available_cpus() is Linux-only (sched_getaffinity);
    // on other platforms it is an empty set whose out-of-range access is
    // CAPSID_INVALID_ARGUMENT, while recommended_worker_count() is bounded
    // to >= 1 everywhere (frozen fallback, spec §10.2).
    require(capsid::recommended_worker_count() >= 1,
            "recommended_worker_count() must be at least 1");
    const std::vector<uint32_t> cpus = capsid::available_cpus();
    if (cpus.empty()) {
        uint32_t cpu = 0;
        require(capsid_available_cpu_at(0, &cpu) == CAPSID_INVALID_ARGUMENT,
                "empty CPU set must report CAPSID_INVALID_ARGUMENT");
    } else {
        uint32_t cpu = 0;
        require(capsid_available_cpu_at(0, &cpu) == CAPSID_OK,
                "first available CPU must be readable");
    }

    // C++ policy surface: the module allowlist only accepts names from the
    // frozen known-module registry (capsid:env here); the entry bundle name
    // is not part of the allowlist (matching test_worker_integration's
    // bodyless-end-failure policy).
    capsid::CapabilityPolicyBuilder policy;
    policy.application_identity("package-smoke");
    policy.allow_module("capsid:env");

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.capability_policy = &policy.descriptor();

    capsid_worker *worker = nullptr;
    require(capsid_worker_spawn(&config, &worker) == CAPSID_OK,
            "could not spawn worker");

    // Contract: load the bundle BEFORE waiting for READY (mirrors
    // test_worker_integration — the HELLO/READY handshake completes once
    // the module is loaded).
    const std::string bundle = read_file(bundle_path);
    require(capsid_worker_load_bundle_named(
                worker,
                reinterpret_cast<const uint8_t *>(bundle.data()),
                bundle.size(),
                "smoke.mjs") == CAPSID_OK,
            "could not load bundle");

    const double startup_deadline = now_seconds() + 15.0;
    for (;;) {
        capsid_event event;
        require(wait_for_event(worker, startup_deadline, &event) == CAPSID_OK,
                "startup event");
        if (event.type == CAPSID_EVENT_READY) {
            break;
        }
        if (event.type == CAPSID_EVENT_EXIT || event.type == CAPSID_EVENT_ERROR) {
            fail("worker failed before READY");
        }
    }

    require(capsid_worker_begin_bodyless_request(
                worker, 1, "GET", "https://example.test/smoke", nullptr, 0) ==
                CAPSID_OK,
            "could not begin request");

    const double request_deadline = now_seconds() + 15.0;
    bool received_head = false;
    bool received_end = false;
    size_t body_size = 0;
    for (;;) {
        capsid_event event;
        require(wait_for_event(worker, request_deadline, &event) == CAPSID_OK,
                "request event");
        if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
            continue;
        }
        if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
            require(event.request_id == 1 && event.status == 200,
                    "unexpected response head");
            received_head = true;
            continue;
        }
        if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
            require(event.request_id == 1 && received_head,
                    "unexpected response body");
            body_size += event.payload.size;
            require(capsid_worker_grant_response_credit(
                        worker, event.request_id,
                        static_cast<uint32_t>(event.payload.size)) == CAPSID_OK,
                    "could not replenish response credit");
            continue;
        }
        if (event.type == CAPSID_EVENT_RESPONSE_END) {
            require(event.request_id == 1, "unexpected response end");
            received_end = true;
            break;
        }
        if (event.type == CAPSID_EVENT_ERROR || event.type == CAPSID_EVENT_EXIT) {
            fail("worker failed during request");
        }
    }
    require(received_head && received_end, "incomplete response");
    require(body_size == 16, "unexpected response body size");

    require(capsid_worker_shutdown(worker) == CAPSID_OK,
            "graceful shutdown failed");
    for (;;) {
        capsid_event event;
        require(wait_for_event(worker, now_seconds() + 15.0, &event) ==
                    CAPSID_OK,
                "shutdown event");
        if (event.type == CAPSID_EVENT_EXIT) {
            break;
        }
        if (event.type == CAPSID_EVENT_ERROR) {
            fail("worker error during shutdown");
        }
    }
    capsid_worker_destroy(worker);

    std::printf("PASS: package smoke C++ round trip\n");
    return 0;
}
