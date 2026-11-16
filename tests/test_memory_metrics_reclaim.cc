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
struct Worker {
    capsid_worker* value = nullptr;
    ~Worker() { capsid_worker_destroy(value); }
};
using Clock = std::chrono::steady_clock;

capsid_event next(capsid_worker* worker, Clock::time_point deadline) {
    for (;;) {
        const auto flushed = capsid_worker_flush(worker);
        check(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "flush frames");
        capsid_event event{};
        event.struct_size = sizeof(event);
        const auto result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            check(event.type != CAPSID_EVENT_ERROR && event.type != CAPSID_EVENT_EXIT,
                  "worker remains healthy through snapshot");
            return event;
        }
        check(result == CAPSID_WOULD_BLOCK && Clock::now() < deadline,
              "absolute event deadline");
        capsid_pollfd descriptor{};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flushed == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        check(capsid::win32::capsid_poll(&descriptor, 1, 20) >= 0, "poll worker");
    }
}

capsid_memory_metrics snapshot(capsid_worker* worker, Clock::time_point deadline) {
    for (;;) {
        const auto event = next(worker, deadline);
        if (event.type != CAPSID_EVENT_MEMORY_METRICS) continue;
        capsid_memory_metrics metrics;
        capsid_memory_metrics_init(&metrics);
        ok(capsid_memory_metrics_decode(&event, &metrics), "decode actual snapshot");
        return metrics;
    }
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 4, "worker soak-fixture reclaimed|retained");
        const bool retained = std::string(argv[3]) == "retained";
        check(retained || std::string(argv[3]) == "reclaimed", "known test mode");
        std::ifstream input(argv[2], std::ios::binary);
        check(input.is_open(), "open actual soak fixture");
        std::string bundle((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
        if (retained) {
            // Reachable arrays are not garbage: the same snapshot path must
            // still expose accumulating live allocations, not hide a leak.
            const std::string marker = "const FILL_BYTE";
            const auto start = bundle.find(marker);
            check(start != std::string::npos, "negative fixture global marker");
            bundle.insert(start, "const retainedForNegative = [];\n");
            const std::string abort = "const onAbort = () => {";
            const auto position = bundle.find(abort);
            check(position != std::string::npos, "negative fixture abort marker");
            bundle.insert(position + abort.size(),
                          "\nretainedForNegative.push(new Array(4096).fill('retained'));\n");
        }
        const char* modules[] = {"capsid:env"};
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.action = CAPSID_PERMISSION_ALLOW;
        rule.permission = CAPSID_PERMISSION_ENV;
        rule.resource = "APP_*";
        rule.rule_id = 1;
        capsid_capability_policy policy;
        capsid_capability_policy_init(&policy);
        policy.allowed_modules = modules;
        policy.allowed_module_count = 1;
        policy.rules = &rule;
        policy.rule_count = 1;
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = argv[1];
        config.capability_policy = &policy;
        config.strict_sandbox = 0;
        config.request_timeout_ms = 5000;
        Worker worker;
        ok(capsid_worker_spawn(&config, &worker.value), "spawn real worker");
        ok(capsid_worker_load_bundle(worker.value,
             reinterpret_cast<const uint8_t*>(bundle.data()), bundle.size()), "load soak fixture");
        const auto ready_deadline = Clock::now() + std::chrono::seconds(20);
        while (next(worker.value, ready_deadline).type != CAPSID_EVENT_READY) {}
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        ok(capsid_worker_request_memory_metrics(worker.value), "baseline request");
        const auto baseline = snapshot(worker.value, deadline);
        auto previous = baseline;
        unsigned replies = 0;
        for (unsigned wave = 0; wave < 4; ++wave) {
            for (uint64_t i = 1; i <= 50; ++i) {
                ok(capsid_worker_begin_bodyless_request(worker.value, wave * 50 + i,
                     "GET", "https://soak.test/slow?ms=200", nullptr, 0), "begin slow request");
            }
            for (uint64_t i = 1; i <= 50; ++i) {
                ok(capsid_worker_cancel(worker.value, wave * 50 + i), "cancel request");
            }
            // The request heads, cancels and both diagnostic requests share
            // one flush. No pipe-idle heuristic, sleeps or forced GC can make
            // an immediate pre-reclaim snapshot accidentally look settled.
            ok(capsid_worker_request_memory_metrics(worker.value), "first queued snapshot");
            ok(capsid_worker_request_memory_metrics(worker.value), "second queued snapshot");
            const auto first = snapshot(worker.value, deadline);
            const auto second = snapshot(worker.value, deadline);
            replies += 2;
            check(first.object_count == second.object_count &&
                  first.memory_used_size == second.memory_used_size,
                  "one matching response per queued snapshot request");
            const bool above_baseline = first.object_count > baseline.object_count * 102 / 100;
            std::printf("MEMORY_RECLAIM mode=%s wave=%u objects=%llu used=%llu baselineObjects=%llu replies=%u\n",
                        argv[3], wave + 1,
                        static_cast<unsigned long long>(first.object_count),
                        static_cast<unsigned long long>(first.memory_used_size),
                        static_cast<unsigned long long>(baseline.object_count), replies);
            if (retained) {
                check(above_baseline && first.memory_used_size > previous.memory_used_size,
                      "reachable leak remains observable after ordinary reclaim");
            } else {
                check(!above_baseline, "cancel snapshot crosses scheduled token reclaim");
            }
            previous = first;
        }
        check(replies == 8, "all eight queued snapshots delivered");
        const auto exit = capsid_test::shutdown_and_wait(worker.value, 2500);
        const auto cleanup = capsid::destroy_worker_with_status(worker.value);
        worker.value = nullptr;
        check(exit.clean() && !cleanup.destroy_forced, "natural exit without token leak");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
