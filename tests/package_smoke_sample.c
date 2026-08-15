// Package smoke sample (remediation spec §12.4): a self-contained C program
// compiled against ONLY the extracted package — include/capsid/runtime.h and
// lib/libcapsid_runtime.a — that drives the frozen worker round trip:
//
//   spawn → READY → load bundle → begin request → RESPONSE_HEAD(200) →
//   RESPONSE_BODY("package smoke ok") → RESPONSE_END → graceful shutdown →
//   drain EXIT → destroy
//
// It never touches the build tree: worker path and bundle path are argv
// inputs pointing into the extracted package and the smoke work directory.
//
// Compiled with a strict -std=c11, where glibc hides poll()/clock_gettime()
// unless the POSIX feature macro is requested (macOS exposes them
// unconditionally). The macro must precede every include.

#define _POSIX_C_SOURCE 200112L

#include "capsid/runtime.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static double now_seconds(void) {
#if defined(_WIN32)
    /* GetTickCount64 is monotonic at millisecond resolution, which the
     * deadline pacing here only needs as a coarse clock. */
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fail("clock_gettime");
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

static char *read_file(const char *path, size_t *out_size) {
    FILE *handle = fopen(path, "rb");
    if (handle == NULL) {
        fail("cannot open bundle");
    }
    if (fseek(handle, 0, SEEK_END) != 0) {
        fail("cannot seek bundle");
    }
    long length = ftell(handle);
    if (length < 0) {
        fail("cannot tell bundle");
    }
    rewind(handle);
    char *data = (char *)malloc((size_t)length + 1);
    if (data == NULL) {
        fail("cannot allocate bundle buffer");
    }
    if (fread(data, 1, (size_t)length, handle) != (size_t)length) {
        fail("cannot read bundle");
    }
    fclose(handle);
    data[length] = '\0';
    *out_size = (size_t)length;
    return data;
}

static void drain_until_exit(capsid_worker *worker) {
    const double deadline = now_seconds() + 15.0;
    for (;;) {
        capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail("shutdown flush failed");
        }
        capsid_event event;
        memset(&event, 0, sizeof(event));
        event.struct_size = sizeof(event);
        capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_EXIT) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail("worker error during shutdown");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail("shutdown event error");
        }
        if (now_seconds() >= deadline) {
            fail("timed out draining EXIT after shutdown");
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

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <capsid-worker> <bundle.mjs>\n", argv[0]);
        return 2;
    }
    const char *worker_path = argv[1];
    const char *bundle_path = argv[2];

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;

    capsid_worker *worker = NULL;
    if (capsid_worker_spawn(&config, &worker) != CAPSID_OK) {
        fail("could not spawn worker");
    }

    // Contract (mirrors test_worker_integration): the bundle is loaded
    // BEFORE the worker reports READY — spawn only starts the process, the
    // HELLO/READY handshake completes once the module is loaded.
    size_t bundle_size = 0;
    char *bundle = read_file(bundle_path, &bundle_size);
    if (capsid_worker_load_bundle_named(worker,
                                        (const uint8_t *)bundle,
                                        bundle_size,
                                        "smoke.mjs") != CAPSID_OK) {
        fail("could not load bundle");
    }
    free(bundle);

    const double startup_deadline = now_seconds() + 15.0;
    int ready = 0;
    for (;;) {
        capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail("startup flush failed");
        }
        capsid_event event;
        memset(&event, 0, sizeof(event));
        event.struct_size = sizeof(event);
        capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                ready = 1;
                break;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail("worker startup error");
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail("startup event error");
        }
        if (now_seconds() >= startup_deadline) {
            fail("timed out waiting for READY");
        }
#if defined(_WIN32)
        Sleep(50);
#else
        struct pollfd descriptor;
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
#endif
    }
    if (!ready) {
        fail("worker never became READY");
    }

    if (capsid_worker_begin_bodyless_request(worker, 1, "GET",
                                             "https://example.test/smoke",
                                             NULL, 0) != CAPSID_OK) {
        fail("could not begin request");
    }

    const double request_deadline = now_seconds() + 15.0;
    int received_head = 0;
    int received_end = 0;
    size_t body_size = 0;
    for (;;) {
        capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail("request flush failed");
        }
        capsid_event event;
        memset(&event, 0, sizeof(event));
        event.struct_size = sizeof(event);
        capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                if (event.request_id != 1 || event.status != 200) {
                    fail("unexpected response head");
                }
                received_head = 1;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                if (!received_head || event.request_id != 1) {
                    fail("response body before its head");
                }
                body_size += event.payload.size;
                if (capsid_worker_grant_response_credit(
                        worker, event.request_id,
                        (uint32_t)event.payload.size) != CAPSID_OK) {
                    fail("could not replenish response credit");
                }
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                if (!received_head || event.request_id != 1) {
                    fail("unexpected response end");
                }
                received_end = 1;
                break;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail("worker request error");
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail("request event error");
        }
        if (now_seconds() >= request_deadline) {
            fail("timed out waiting for the response");
        }
#if defined(_WIN32)
        Sleep(50);
#else
        struct pollfd descriptor;
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
#endif
    }
    if (!received_end) {
        fail("no RESPONSE_END");
    }
    if (body_size != 16) {  /* "package smoke ok" */
        fprintf(stderr, "FAIL: unexpected body size %zu\n", body_size);
        exit(1);
    }

    if (capsid_worker_shutdown(worker) != CAPSID_OK) {
        fail("graceful shutdown failed");
    }
    drain_until_exit(worker);
    capsid_worker_destroy(worker);

    printf("PASS: package smoke C round trip\n");
    return 0;
}
