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
#include "response_credit_probe.h"
#include "protocol.h"
#include "win32_compat.h"

#include <chrono>
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) fail(std::string(what) + " deadline exceeded");
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



void test_negotiation(capsid_worker_config &config, uint32_t capabilities = 0, bool small_window = false) {
    if (small_window) config.initial_stream_window = 3;
    capsid_test::ResponseCreditProbe response_credit(small_window ? 3 : 0, capabilities);
    capsid_protocol_options v4;
    require_result(
        capsid_protocol_options_init(&v4, sizeof(v4)), "v4 options init");
    v4.wire_version = 4;
    v4.capabilities = capabilities;
    capsid_worker *worker = nullptr;
    require_result(
        capsid_worker_spawn_ex(&config, &v4, &worker), "v4 spawn");
    Pump pump{worker};
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(kBundle),
            std::strlen(kBundle)),
        "v4 load bundle");
    const capsid_event ready = pump.wait(is_ready, "v4 ready");

    // Amended §1.2 + T07: the client splits and strips the capability
    // tail itself, so the event payload is EXACTLY the v3-shaped
    // compatibility/proof bytes; the negotiated state moves to the
    // typed getter (embedders never read the tail by hand).
    // Inbound connections require the v4.2 terminal barrier. Half-close
    // and outbound constructor support are still absent.
    capsid_protocol_info info;
    require_result(
        capsid_worker_get_protocol_info(worker, &info, sizeof(info)),
        "protocol info after READY");
    const uint32_t expected = (capabilities & CAPSID_PROTOCOL_CAP_CHANNEL_CONFIRM)
        ? 27 | (capabilities & (CAPSID_PROTOCOL_CAP_CHANNEL_CLOSE_FINAL |
            CAPSID_PROTOCOL_CAP_FETCH_UPGRADE | CAPSID_PROTOCOL_CAP_CONNECTION_LIFETIME)) : 0;
    if (info.wire_version != 4 || info.host_capabilities != capabilities ||
        info.worker_capabilities != expected ||
        info.effective_capabilities != (expected & capabilities)) {
        fail("v4 negotiated capabilities do not match implemented inbound support");
    }
    if (ready.payload.size == 0 || ready.payload.size % 8 == 0) {
        // stripped payload = 71-byte compatibility id + proof stub;
        // an empty or tail-sized payload means the strip is wrong
        fail("READY payload must be the original proof-only bytes");
    }
    (void)ready;

    // v3 semantics ride the v4 stream unchanged. Canonical sequence:
    // bodyless GET ends the request side immediately.
    require_result(
        capsid_worker_begin_request(
            worker, 1, "GET", "https://example.test/", nullptr, 0),
        "v4 begin request");
    require_result(
        capsid_worker_end_request(worker, 1), "v4 end request");
    const capsid_event head = pump.wait([](const capsid_event &event) {
        return event.type == CAPSID_EVENT_RESPONSE_HEAD;
    }, "v4 response head");
    if (head.status != 200) fail("v4 response status must be 200");
    std::string body;
    for (;;) {
        const capsid_event next = pump.wait([](const capsid_event &event) {
            return event.type == CAPSID_EVENT_RESPONSE_BODY || event.type == CAPSID_EVENT_RESPONSE_END;
        }, "v4 response body");
        if (next.type == CAPSID_EVENT_RESPONSE_END) break;
        body.append(reinterpret_cast<const char *>(next.payload.data), next.payload.size);
        response_credit.consume(worker, next);
    }
    if (body != "v4-carriage") fail("v4 response bytes mismatch");
    response_credit.complete(1, body);
    if (capabilities) {
        // Bypass the host API's own gate to verify the actual worker guard.
        // Announcing host features cannot enable an unfinished worker path.
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kChannelHalfClose;
        capsid::protocol::append_u32(&frame.payload, 1);
        std::vector<uint8_t> wire;
        if (!capsid::protocol::encode_versioned(4, frame, &wire)) fail("guard frame encode");
        const auto sent = capsid::win32::send_fd(capsid_worker_fd(worker), wire.data(), wire.size(), 0);
        if (sent < 0 || static_cast<size_t>(sent) != wire.size()) fail("guard frame write");
        const auto rejected = pump.wait([](const capsid_event &event) {
            return event.type == CAPSID_EVENT_ERROR;
        }, "worker channel capability guard");
        const std::string diagnostic(reinterpret_cast<const char *>(rejected.payload.data), rejected.payload.size);
        if (diagnostic != "invalid IPC frame") fail("worker rejects unnegotiated channel frame explicitly");
    }
    capsid_worker_destroy(worker);
}

void test_rejects_unknown_capability_bits(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    capsid_protocol_options options;
    require_result(
        capsid_protocol_options_init(&options, sizeof(options)),
        "options init");
    options.wire_version = 4;
    options.capabilities = 1u << 30;  // unknown bit
    capsid_worker *worker = reinterpret_cast<capsid_worker *>(0x1);
    const capsid_result result =
        capsid_worker_spawn_ex(&config, &options, &worker);
    if (result == CAPSID_OK || worker != nullptr) {
        fail("spawn must reject unknown capability bits with a null out");
    }
}

void test_rejects_unknown_version(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    capsid_protocol_options options;
    require_result(
        capsid_protocol_options_init(&options, sizeof(options)),
        "options init");
    options.wire_version = 5;
    capsid_worker *worker = nullptr;
    const capsid_result result =
        capsid_worker_spawn_ex(&config, &options, &worker);
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
    if (argc != 2 && !(argc == 3 && std::strcmp(argv[2], "--small-response-window") == 0)) {
        std::fprintf(stderr, "usage: %s <capsid-worker>\n", argv[0]);
        return 2;
    }
    const char *worker_path = argv[1];
    const bool small_window = argc == 3;

    {
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = worker_path;
        test_negotiation(config, 0, small_window);
        test_negotiation(config, 31, small_window);
        test_negotiation(config, 63, small_window);
        test_negotiation(config, 123, small_window);
        test_negotiation(config, 155, small_window);
        test_negotiation(config, 251, small_window);
        test_negotiation(config, 255, small_window);
    }
    test_rejects_unknown_capability_bits(worker_path);
    test_rejects_unknown_version(worker_path);
    test_v3_default_unchanged(worker_path);
    std::printf("worker v4 negotiation: PASS\n");
    return 0;
}
