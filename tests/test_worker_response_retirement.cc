#include "capsid/runtime.h"
#include "graceful_worker_exit.h"
#include "win32_compat.h"
#include "worker_exit_status.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void ok(capsid_result result, const char* message) {
    check(result == CAPSID_OK, message);
}
struct Peer {
    capsid_worker* worker = nullptr;
    ~Peer() { capsid_worker_destroy(worker); }

    capsid_event next() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto flushed = capsid_worker_flush(worker);
            check(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "flush queued IPC");
            capsid_event event{};
            event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK) {
                std::printf("RETIREMENT_EVENT type=%d request=%llu flags=%u status=%u credit=%u payload=",
                    int(event.type), static_cast<unsigned long long>(event.request_id),
                    event.flags, event.status, event.credit);
                for (size_t index = 0; index < event.payload.size; ++index)
                    std::printf("%02x", unsigned(event.payload.data[index]));
                std::puts("");
                std::fflush(stdout);
                check(event.type != CAPSID_EVENT_ERROR && event.type != CAPSID_EVENT_EXIT,
                      "worker remains healthy");
                return event;
            }
            check(result == CAPSID_WOULD_BLOCK && std::chrono::steady_clock::now() < deadline,
                  "worker event deadline");
            capsid_pollfd descriptor{};
            descriptor.fd = capsid_worker_fd(worker);
            descriptor.events = POLLIN | (flushed == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&descriptor, 1, 10);
        }
    }

    void grant(uint64_t id, uint32_t bytes) {
        ok(capsid_worker_grant_response_credit(worker, id, bytes), "return consumed response credit");
        std::printf("RETIREMENT_CREDIT request=%llu bytes=%u\n",
            static_cast<unsigned long long>(id), bytes);
        std::fflush(stdout);
    }

    std::string body(uint64_t id) {
        const auto event = next();
        check(event.type == CAPSID_EVENT_RESPONSE_BODY && event.request_id == id &&
              event.flags == 0 && event.status == 0 && event.credit == 0, "owned body piece");
        return std::string(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
    }
};

void run(const char* worker, const char* fixture, const std::string& kind,
         const std::string& action, size_t bytes) {
    std::ifstream input(fixture, std::ios::binary);
    check(input.is_open(), "fixture opened");
    const std::string bundle((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker;
    config.initial_stream_window = 32;
    config.request_timeout_ms = 750;
    capsid_build_info info;
    capsid_build_info_init(&info);
    ok(capsid_runtime_build_info(&info), "linked runtime identity");
    check(info.compatibility_id && info.capability_manifest_sha256, "complete build identity");
#if defined(_WIN32)
    const uint32_t features = config.process_memory_limit ? CAPSID_SANDBOX_FEATURE_RLIMITS : 0U;
#else
    const uint32_t features = CAPSID_SANDBOX_FEATURE_RLIMITS;
#endif
    std::printf("RETIREMENT_CONFIG timeout=%llu window=%u heap=%llu process=%llu limit=%llu ready=%u compatibility=%s manifest=%s\n",
        static_cast<unsigned long long>(config.request_timeout_ms), config.initial_stream_window,
        static_cast<unsigned long long>(config.js_heap_limit),
        static_cast<unsigned long long>(config.process_memory_limit),
        static_cast<unsigned long long>(config.max_fetch_response_body_bytes), features,
        info.compatibility_id, info.capability_manifest_sha256);
    std::fflush(stdout);
    capsid_protocol_options options;
    capsid_protocol_options_init(&options, sizeof(options));
    options.wire_version = 4;
    options.capabilities = 123;
    options.max_channels = 1;
    Peer peer;
    ok(capsid_worker_spawn_ex(&config, &options, &peer.worker), "spawn worker");
    ok(capsid_worker_load_bundle(peer.worker, reinterpret_cast<const uint8_t*>(bundle.data()),
                                bundle.size()), "load actual bundle");
    const auto ready = peer.next();
    check(ready.type == CAPSID_EVENT_READY && ready.flags == features &&
          std::string(reinterpret_cast<const char*>(ready.payload.data), ready.payload.size) ==
              info.compatibility_id, "worker agrees with linked runtime");
    const auto started = std::chrono::steady_clock::now();
    const auto url = "http://app.test/" + kind + "?bytes=" + std::to_string(bytes);
    ok(capsid_worker_begin_bodyless_request(peer.worker, 1, "GET", url.c_str(), nullptr, 0),
       "start owned response");
    const auto head = peer.next();
    check(head.type == CAPSID_EVENT_RESPONSE_HEAD && head.request_id == 1 &&
          head.status == 200 && head.flags == 0 && head.credit == 0, "streaming response head");
    const std::string alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string expected;
    for (size_t index = 0; index < bytes; ++index) expected += alphabet[index % alphabet.size()];
    std::string text = peer.body(1);
    check(text == expected.substr(0, 32), "exact initial window before action");
    if (action == "timeout") {
        const auto terminal = peer.next();
        check(terminal.type == CAPSID_EVENT_REQUEST_TIMEOUT && terminal.request_id == 1 &&
              terminal.flags == 1 && terminal.status == 0 && terminal.credit == 0 &&
              std::string(reinterpret_cast<const char*>(terminal.payload.data), terminal.payload.size) ==
                  "request deadline exceeded", "single timeout after partial response");
    } else if (action == "cancel") {
        ok(capsid_worker_cancel(peer.worker, 1), "cancel held writer");
    } else if (action == "grant") {
        uint32_t consumed = 32;
        for (;;) {
            peer.grant(1, consumed);
            const auto event = peer.next();
            check(event.request_id == 1, "original response ownership");
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                check(text == expected, "complete credited response bytes");
                break;
            }
            check(event.type == CAPSID_EVENT_RESPONSE_BODY && event.payload.size > 0 &&
                  event.payload.size <= 32, "bounded continued body");
            text.append(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
            consumed = static_cast<uint32_t>(event.payload.size);
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    check(action != "timeout" || (elapsed >= 750 && elapsed < 5000), "configured deadline observed");
    std::printf("RETIREMENT_ACTION kind=%s action=%s bytes=%zu received=%zu milliseconds=%lld\n",
        kind.c_str(), action.c_str(), bytes, text.size(), static_cast<long long>(elapsed));
    std::fflush(stdout);
    if (action != "shutdown") {
        ok(capsid_worker_begin_bodyless_request(peer.worker, 2, "GET", "http://app.test/healthy", nullptr, 0),
           "same worker healthy reuse");
        const auto healthy = peer.next();
        check(healthy.type == CAPSID_EVENT_RESPONSE_HEAD && healthy.request_id == 2 &&
              healthy.status == 200 && healthy.flags == 1 && healthy.credit == 7, "healthy fixed head");
        check(peer.body(2) == "healthy", "exact healthy bytes");
        peer.grant(2, 7);
        const auto end = peer.next();
        check(end.type == CAPSID_EVENT_RESPONSE_END && end.request_id == 2, "healthy end without stale events");
    }
    const auto exit = capsid_test::shutdown_and_wait(peer.worker, 2500);
    const auto cleanup = capsid::destroy_worker_with_status(peer.worker);
    peer.worker = nullptr;
    std::printf("RETIREMENT_EXIT clean=%d shutdown=%d flush=%d event=%d closed=%d reaped=%d status=%d wait=%d forced=%d errors=%u\n",
        exit.clean() ? 1 : 0, int(exit.shutdown_result), int(exit.flush_result), int(exit.event_result),
        exit.channel_closed ? 1 : 0, exit.reaped ? 1 : 0, exit.status, exit.wait_error,
        cleanup.destroy_forced ? 1 : 0, exit.error_events);
    std::fflush(stdout);
    check(exit.clean() && !cleanup.destroy_forced, "natural OS exit without runtime diagnostics");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 6, "worker fixture kind action bytes");
        const std::string kind = argv[3], action = argv[4];
        const size_t bytes = static_cast<size_t>(std::stoul(argv[5]));
        check((kind == "snapshot" || kind == "stream") && (bytes == 33 || bytes == 101) &&
              (action == "timeout" || action == "cancel" || action == "grant" || action == "shutdown"),
              "known deterministic case");
        run(argv[1], argv[2], kind, action, bytes);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
