#include "capsid/runtime.h"
#include "response_credit_probe.h"
#include "graceful_worker_exit.h"
#include "win32_compat.h"
#include "worker_exit_status.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void ok(capsid_result result, const char* message) {
    check(result == CAPSID_OK, message);
}
struct Peer {
    capsid_worker* worker = nullptr;
    capsid_test::ResponseCreditProbe response_credit;
    ~Peer() { capsid_worker_destroy(worker); }

    capsid_event next() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto flushed = capsid_worker_flush(worker);
            check(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "flush");
            capsid_event event{};
            event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK) {
                std::printf("DEADLINE_EVENT type=%d request=%llu flags=%u status=%u credit=%u payload=",
                    int(event.type), static_cast<unsigned long long>(event.request_id),
                    event.flags, event.status, event.credit);
                for (size_t index = 0; index < event.payload.size; ++index)
                    std::printf("%02x", unsigned(event.payload.data[index]));
                std::printf("\n");
                std::fflush(stdout);
                check(event.type != CAPSID_EVENT_ERROR && event.type != CAPSID_EVENT_EXIT, "healthy worker");
                if (event.type == CAPSID_EVENT_AUDIT || event.type == CAPSID_EVENT_LOG ||
                    event.type == CAPSID_EVENT_MEMORY_METRICS) continue;
                return event;
            }
            check(result == CAPSID_WOULD_BLOCK && std::chrono::steady_clock::now() < deadline,
                  "event deadline");
            capsid_pollfd fd{};
            fd.fd = capsid_worker_fd(worker);
            fd.events = POLLIN | (flushed == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&fd, 1, 10);
        }
    }

    std::string response(uint64_t id, bool timeout_expected = false) {
        std::string text;
        bool head = false;
        for (;;) {
            const auto event = next();
            check(event.request_id == id, "HTTP owner");
            if (event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
                check(timeout_expected && !head && text.empty(), "configured host timeout without response");
                std::printf("DEADLINE_TIMEOUT request=%llu flags=%u status=%u payload=%.*s\n",
                    static_cast<unsigned long long>(event.request_id), event.flags, event.status,
                    int(event.payload.size), event.payload.data);
                std::fflush(stdout);
                return "\"HOST_TIMEOUT\"";
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                check(!head && event.status == 200, "HTTP status and single head");
                head = true;
            } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                check(head, "body follows head");
                text.append(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
                response_credit.consume(worker, event);
            } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
                check(head, "complete response");
                response_credit.complete(id, text);
                return text;
            } else if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                check(event.credit > 0, "positive request credit");
            } else check(false, "ordinary native HTTP event");
        }
    }
};
}

int main(int argc, char** argv) {
    try {
        check(argc == 7 || (argc == 8 && std::strcmp(argv[7], "--small-response-window") == 0),
              "worker, fixture, target, stage, trigger, link, optional small response window");
        const bool small_window = argc == 8;
        const std::string stage = argv[4], trigger = argv[5], link = argv[6];
        check(stage == "headless" || stage == "unread" || stage == "default" ||
              stage == "byob" || stage == "least", "known native stage");
        const bool host = trigger == "host";
        check((host && (link == "implicit" || link == "request")) ||
              (trigger == "signal" && link == "timeout"), "known deadline and ownership");
        std::ifstream input(argv[2], std::ios::binary);
        check(input.is_open(), "fixture opened");
        const std::string bundle((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = argv[1];
        config.request_timeout_ms = host ? 750 : 5000;
        if (small_window) config.initial_stream_window = 32;
        capsid_build_info info;
        capsid_build_info_init(&info);
        ok(capsid_runtime_build_info(&info), "linked runtime identity");
        check(info.compatibility_id && info.capability_manifest_sha256, "complete build identity");
#if defined(_WIN32)
        const char* platform = "windows";
        const uint32_t expected_features = config.process_memory_limit ? CAPSID_SANDBOX_FEATURE_RLIMITS : 0U;
#else
        const char* platform = "posix";
        const uint32_t expected_features = CAPSID_SANDBOX_FEATURE_RLIMITS;
#endif
        std::printf("DEADLINE_CONFIG timeout=%llu heap=%llu limit=%llu process=%llu platform=%s ready=%u compatibility=%s manifest=%s\n",
            static_cast<unsigned long long>(config.request_timeout_ms),
            static_cast<unsigned long long>(config.js_heap_limit),
            static_cast<unsigned long long>(config.max_fetch_response_body_bytes),
            static_cast<unsigned long long>(config.process_memory_limit), platform, expected_features,
            info.compatibility_id, info.capability_manifest_sha256);
        capsid_egress_rule rule;
        capsid_egress_rule_init(&rule);
        rule.action = CAPSID_EGRESS_ALLOW;
        rule.target = "localhost";
        capsid_egress_policy policy;
        capsid_egress_policy_init(&policy);
        policy.rules = &rule;
        policy.rule_count = 1;
        config.egress_policy = &policy;
        capsid_protocol_options options;
        capsid_protocol_options_init(&options, sizeof(options));
        options.wire_version = 4;
        options.capabilities = 123;
        options.max_channels = 1;
        Peer peer;
        peer.response_credit = capsid_test::ResponseCreditProbe(small_window ? 32 : 0);
        ok(capsid_worker_spawn_ex(&config, &options, &peer.worker), "configured native worker spawn");
        ok(capsid_worker_load_bundle(peer.worker, reinterpret_cast<const uint8_t*>(bundle.data()),
                                    bundle.size()), "bundle load");
        const auto ready = peer.next();
        check(ready.type == CAPSID_EVENT_READY && ready.flags == expected_features &&
              std::string(reinterpret_cast<const char*>(ready.payload.data), ready.payload.size) ==
                  info.compatibility_id, "actual worker agrees with linked runtime");
        const std::string url = std::string("http://app.test/probe?target=") + argv[3] +
            "&stage=" + stage + "&trigger=" + trigger + "&link=" + link;
        const auto began = std::chrono::steady_clock::now();
        ok(capsid_worker_begin_bodyless_request(peer.worker, 1, "GET", url.c_str(), nullptr, 0),
           "native probe request");
        const auto result = peer.response(1, host);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began).count();
        std::printf("DEADLINE_ELAPSED milliseconds=%lld\nLIMIT_PROBE %s\n",
                    static_cast<long long>(elapsed), result.c_str());
        std::fflush(stdout);
        std::string acknowledgment;
        check(static_cast<bool>(std::getline(std::cin, acknowledgment)) && acknowledgment == "finish",
              "source EOF observed before host shutdown");
        ok(capsid_worker_begin_bodyless_request(peer.worker, 2, "GET", "http://app.test/healthy", nullptr, 0),
           "healthy request");
        check(peer.response(2) == "healthy", "same worker healthy reuse");
        const auto exited = capsid_test::shutdown_and_wait(peer.worker, 2500);
        const auto cleanup = capsid::destroy_worker_with_status(peer.worker);
        peer.worker = nullptr;
        std::printf("LIMIT_EXIT {\"clean\":%s,\"processObserved\":%s,\"exitStatus\":%d,\"forced\":%s,\"errorEvents\":%u}\n",
            exited.clean() ? "true" : "false", exited.reaped ? "true" : "false", exited.status,
            cleanup.destroy_forced ? "true" : "false", exited.error_events);
        check(exited.clean() && !cleanup.destroy_forced, "natural OS exit");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
