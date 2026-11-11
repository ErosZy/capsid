/*
 * C11 embedding example: FetchRPC v4 carriage + the channel ABI probe
 * (GLM plan T07). Demonstrates the frozen surface an embedder uses:
 *
 *   - capsid_protocol_options / capsid_worker_spawn_ex (two-way size
 *     negotiation; v4 carriage with no feature bits)
 *   - capsid_worker_get_protocol_info (negotiated state via the typed
 *     getter, never by reading the READY tail by hand)
 *   - channel entry points answering CAPSID_PROTOCOL_ERROR on a
 *     connection that did not negotiate the channel capabilities
 *   - a real request: bodyless GET, status 200, exact body bytes,
 *     exactly one terminal event
 *
 * Build it from the repository root:
 *   cc -std=c11 -Iinclude -c examples/c11-embed/channel_probe.c \
 *      -o /tmp/channel_probe.o
 *   c++ /tmp/channel_probe.o build-v030-dev/libcapsid_runtime.a \
 *      -o /tmp/channel_probe
 */
#include "capsid/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static const char kBundle[] =
    "export default { async fetch(request) {"
    "  return new Response(\"ok\", { status: 200 });"
    "} };";

int main(int argc, char **argv) {
    const char *worker_path = argc > 1 ? argv[1] : "capsid-worker";
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.js_heap_limit = 64u << 20;
    config.max_queued_bytes = 4u << 20;
    config.max_header_bytes = 64u << 10;

    capsid_protocol_options options;
    check(capsid_protocol_options_init(&options, sizeof(options)) ==
              CAPSID_OK,
          "options init");
    options.wire_version = 4;

    capsid_worker *worker = NULL;
    const capsid_result spawned =
        capsid_worker_spawn_ex(&config, &options, &worker);
    check(spawned == CAPSID_OK, "v4 spawn");
    if (spawned != CAPSID_OK) {
        fprintf(stderr, "spawn: %s\n", capsid_result_string(spawned));
        return 1;
    }

    check(capsid_worker_load_bundle(
              worker, (const uint8_t *)kBundle, strlen(kBundle)) ==
              CAPSID_OK,
          "load bundle");

    /* pump: READY first, then the response stream */
    int saw_ready = 0;
    int saw_response_head = 0;
    int saw_terminal = 0;
    char body[2] = {0};
    size_t body_size = 0;
    for (;;) {
        const capsid_result flushed = capsid_worker_flush(worker);
        check(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK,
              "flush");
        capsid_event event;
        memset(&event, 0, sizeof(event));
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(worker, &event);
        if (result == CAPSID_WOULD_BLOCK) {
            continue;
        }
        check(result == CAPSID_OK, "next_event");
        if (event.type == CAPSID_EVENT_READY) {
            saw_ready = 1;
            /* negotiated state comes from the typed getter */
            capsid_protocol_info info;
            check(capsid_worker_get_protocol_info(
                      worker, &info, sizeof(info)) == CAPSID_OK,
                  "protocol info");
            check(info.wire_version == 4 &&
                      info.effective_capabilities == 0,
                  "carriage negotiation: wire 4, no capabilities");
            /* channel surface is gated on a carriage-only stream */
            const uint8_t byte = 'x';
            check(capsid_worker_channel_send(
                      worker, 1, &byte, 1,
                      CAPSID_CHANNEL_FLAG_START |
                          CAPSID_CHANNEL_FLAG_END |
                          CAPSID_CHANNEL_FLAG_TEXT) ==
                      CAPSID_PROTOCOL_ERROR,
                  "channel send gated without the capability bits");
            check(capsid_worker_dial_accept(worker, 1, 1,
                                            1024 * 1024,
                                            "chat") ==
                      CAPSID_PROTOCOL_ERROR,
                  "dial accept gated without the capability bits");
            /* the real request goes out once READY landed */
            check(capsid_worker_begin_bodyless_request(
                      worker, 1, "GET", "https://example.test/", NULL,
                      0) == CAPSID_OK,
                  "begin bodyless request");
        } else if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
            saw_response_head = 1;
            check(event.status == 200, "status is 200");
        } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
            /* Event storage is borrowed until the next pump call. */
            if (event.payload.size > sizeof(body) - body_size) {
                check(0, "body exceeds expected size");
                body_size = sizeof(body) + 1;
                break;
            }
            if (event.payload.size) memcpy(body + body_size, event.payload.data, event.payload.size);
            body_size += event.payload.size;
        } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
            saw_terminal = 1;
            break;
        } else if (event.type == CAPSID_EVENT_ERROR) {
            fprintf(stderr, "worker error: %.*s\n",
                    (int)event.payload.size,
                    (const char *)event.payload.data);
            break;
        }
    }

    check(saw_ready, "READY observed");
    check(saw_response_head, "RESPONSE_HEAD observed");
    check(saw_terminal, "exactly one terminal event");
    check(body_size == 2 && memcmp(body, "ok", 2) == 0,
          "body is byte-exactly \"ok\"");

    capsid_worker_destroy(worker);
    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("c11 channel probe: PASS\n");
    return 0;
}
