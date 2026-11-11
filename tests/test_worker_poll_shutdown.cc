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
}

int main(int argc, char** argv) {
    try {
        check(argc == 4, "worker fixture before|after");
        const std::string order = argv[3];
        check(order == "before" || order == "after", "known shutdown ordering");
        std::ifstream input(argv[2], std::ios::binary);
        check(input.is_open(), "fixture opened");
        const std::string bundle((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = argv[1];
        capsid_build_info info;
        capsid_build_info_init(&info);
        ok(capsid_runtime_build_info(&info), "linked build identity");
        Worker worker;
        ok(capsid_worker_spawn(&config, &worker.value), "spawn worker");
        ok(capsid_worker_load_bundle(worker.value, reinterpret_cast<const uint8_t*>(bundle.data()),
                                    bundle.size()), "load real bundle");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto flushed = capsid_worker_flush(worker.value);
            check(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "startup flush");
            capsid_event event{};
            event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(worker.value, &event);
            if (result == CAPSID_OK) {
                check(event.type == CAPSID_EVENT_READY && info.compatibility_id &&
                      std::string(reinterpret_cast<const char*>(event.payload.data), event.payload.size) ==
                          info.compatibility_id, "actual READY identity");
                break;
            }
            check(result == CAPSID_WOULD_BLOCK && std::chrono::steady_clock::now() < deadline,
                  "READY deadline");
            capsid_pollfd descriptor{};
            descriptor.fd = capsid_worker_fd(worker.value);
            descriptor.events = POLLIN | (flushed == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&descriptor, 1, 10);
        }
        // Queue all three frames before the one flush in shutdown_and_wait.
        // The post-shutdown metrics frame reaches queue_output/update_poll
        // without any JS promise or pending response, isolating poll closure.
        if (order == "after") ok(capsid_worker_shutdown(worker.value), "queue initial shutdown");
        ok(capsid_worker_request_memory_metrics(worker.value), "queue actual metrics request");
        const auto exit = capsid_test::shutdown_and_wait(worker.value, 2500);
        const auto cleanup = capsid::destroy_worker_with_status(worker.value);
        worker.value = nullptr;
        std::printf("POLL_SHUTDOWN order=%s compatibility=%s clean=%d shutdown=%d flush=%d event=%d closed=%d reaped=%d status=%d wait=%d forced=%d errors=%u\n",
            order.c_str(), info.compatibility_id, exit.clean() ? 1 : 0,
            int(exit.shutdown_result), int(exit.flush_result), int(exit.event_result),
            exit.channel_closed ? 1 : 0, exit.reaped ? 1 : 0, exit.status, exit.wait_error,
            cleanup.destroy_forced ? 1 : 0, exit.error_events);
        check(exit.clean() && !cleanup.destroy_forced, "natural exit without restarting a closing poll");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
