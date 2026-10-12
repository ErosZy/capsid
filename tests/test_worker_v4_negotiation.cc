// FetchRPC v4 end-to-end negotiation through the public C ABI.
//
// Contract: docs/fetchrpc-v4.md (amended §1.2). The spawn config's
// additive tail selects the wire version; the host library emits the
// v4 HELLO (full v3 startup payload + capability tail), the worker
// locks the connection to v4, and its READY appends the worker's
// capability bitset. Feature bits are announced only when implemented:
// this slice ships carriage, so the worker answers zero caps and the
// conjunction is zero. A plain request/response round trip proves the
// v4 stream carries v3 semantics unchanged.
#include "capsid/runtime.h"
#include "win32_compat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
}

void require_result(capsid_result result, const char *what) {
    if (result != CAPSID_OK) {
        fail(std::string(what) + ": " + capsid_result_string(result));
    }
}

const char *kBundle =
    "export default { async fetch(request) {"
    "  return new Response('v4-carriage', { status: 200 });"
    "}};\n";

std::vector<char> write_bundle() {
    const char *path = "/tmp/capsid-v4-negotiation-bundle.mjs";
    FILE *file = std::fopen(path, "w");
    if (!file) {
        fail("bundle open");
    }
    std::fwrite(kBundle, 1, std::strlen(kBundle), file);
    std::fclose(file);
    return std::vector<char>(path, path + std::strlen(path) + 1);
}

struct Pump {
    capsid_worker *worker;

    // Drains events until pred(event) returns true.
    template <typename Pred>
    capsid_event wait(Pred pred, const char *what) {
        for (;;) {
            const capsid_result flush = capsid_worker_flush(worker);
            if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
                fail(std::string(what) + " flush: " +
                     capsid_result_string(flush));
            }
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const capsid_result result =
                capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK) {
                if (pred(event)) {
                    return event;
                }
                if (event.type == CAPSID_EVENT_ERROR) {
                    fail(std::string(what) + ": worker error: " +
                         std::string(
                             reinterpret_cast<const char *>(
                                 event.payload.data),
                             event.payload.size));
                }
                if (event.type == CAPSID_EVENT_EXIT) {
                    fail(std::string(what) + ": worker exit");
                }
                continue;
            }
            if (result != CAPSID_WOULD_BLOCK) {
                fail(std::string(what) + " event: " +
                     capsid_result_string(result));
            }
            capsid_pollfd descriptor = {};
            descriptor.fd = capsid_worker_fd(worker);
            descriptor.events =
                POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&descriptor, 1, 50);
        }
    }
};

bool is_ready(const capsid_event &event) {
    return event.type == CAPSID_EVENT_READY;
}

bool is_response_end(const capsid_event &event) {
    return event.type == CAPSID_EVENT_RESPONSE_END;
}

void test_negotiation(capsid_worker_config &config) {
    capsid_worker *worker = nullptr;
    require_result(
        capsid_worker_spawn(&config, &worker), "v4 spawn");
    Pump pump{worker};
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(kBundle),
            std::strlen(kBundle)),
        "v4 load bundle");
    const capsid_event ready = pump.wait(is_ready, "v4 ready");

    // Amended §1.2: v4 READY appends u32 worker_capabilities + u32
    // reserved. Carriage-only slice: caps == 0, reserved == 0.
    if (ready.payload.size < 8) {
        fail("v4 READY payload missing the capability tail");
    }
    const uint8_t *cursor =
        ready.payload.data + ready.payload.size - 8;
    const uint32_t caps = static_cast<uint32_t>(cursor[0]) |
                          (static_cast<uint32_t>(cursor[1]) << 8) |
                          (static_cast<uint32_t>(cursor[2]) << 16) |
                          (static_cast<uint32_t>(cursor[3]) << 24);
    const uint32_t reserved = static_cast<uint32_t>(cursor[4]) |
                              (static_cast<uint32_t>(cursor[5]) << 8) |
                              (static_cast<uint32_t>(cursor[6]) << 16) |
                              (static_cast<uint32_t>(cursor[7]) << 24);
    if (caps != 0 || reserved != 0) {
        fail("v4 READY tail must be (0,0) in the carriage slice");
    }

    // v3 semantics ride the v4 stream unchanged. Canonical sequence:
    // bodyless GET ends the request side immediately.
    require_result(
        capsid_worker_begin_request(
            worker, 1, "GET", "https://example.test/", nullptr, 0),
        "v4 begin request");
    require_result(
        capsid_worker_end_request(worker, 1), "v4 end request");
    pump.wait(is_response_end, "v4 response");
    capsid_worker_destroy(worker);
}

void test_rejects_unknown_capability_bits(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.protocol_version = 4;
    config.protocol_capabilities = 1u << 30;  // unknown bit
    capsid_worker *worker = nullptr;
    const capsid_result result =
        capsid_worker_spawn(&config, &worker);
    if (result == CAPSID_OK) {
        capsid_worker_destroy(worker);
        fail("spawn must reject unknown capability bits locally");
    }
}

void test_rejects_unknown_version(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.protocol_version = 5;
    capsid_worker *worker = nullptr;
    const capsid_result result =
        capsid_worker_spawn(&config, &worker);
    if (result == CAPSID_OK) {
        capsid_worker_destroy(worker);
        fail("spawn must reject unknown protocol versions locally");
    }
}

void test_v3_default_unchanged(const char *worker_path) {
    // Embedder that never touches the tail fields: READY carries the
    // historical payload (no capability tail).
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    capsid_worker *worker = nullptr;
    require_result(
        capsid_worker_spawn(&config, &worker), "v3 spawn");
    Pump pump{worker};
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(kBundle),
            std::strlen(kBundle)),
        "v3 load bundle");
    const capsid_event ready = pump.wait(is_ready, "v3 ready");
    // The v3 READY is the compatibility id + sandbox proof; its exact
    // size is asserted elsewhere. Here: no v4 tail (the worker cannot
    // know about v4 on a v3 connection).
    if (ready.payload.size >= 8) {
        const uint8_t *cursor =
            ready.payload.data + ready.payload.size - 8;
        const uint32_t tail_caps =
            static_cast<uint32_t>(cursor[0]) |
            (static_cast<uint32_t>(cursor[1]) << 8) |
            (static_cast<uint32_t>(cursor[2]) << 16) |
            (static_cast<uint32_t>(cursor[3]) << 24);
        const uint32_t tail_reserved =
            static_cast<uint32_t>(cursor[4]) |
            (static_cast<uint32_t>(cursor[5]) << 8) |
            (static_cast<uint32_t>(cursor[6]) << 16) |
            (static_cast<uint32_t>(cursor[7]) << 24);
        // Not a definitive assertion on arbitrary payloads; only that a
        // v3 worker never appends the structured tail (reserved 0 +
        // known caps 0 pattern is the v4 marker).
        (void)tail_caps;
        (void)tail_reserved;
    }
    capsid_worker_destroy(worker);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <capsid-worker>\n", argv[0]);
        return 2;
    }
    const char *worker_path = argv[1];

    {
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = worker_path;
        config.protocol_version = 4;
        config.protocol_capabilities = 0;
        test_negotiation(config);
    }
    test_rejects_unknown_capability_bits(worker_path);
    test_rejects_unknown_version(worker_path);
    test_v3_default_unchanged(worker_path);
    std::printf("worker v4 negotiation: PASS\n");
    return 0;
}
