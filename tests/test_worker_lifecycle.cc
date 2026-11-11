#include "capsid/runtime.h"
#include "graceful_worker_exit.h"
#include "worker_exit_assertions.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {

void fail(const char *message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("expected stubborn worker path");
    }
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.strict_sandbox = 0;

    capsid_worker *worker = NULL;
    if (capsid_worker_spawn(&config, &worker) != CAPSID_OK) {
        fail("could not spawn stubborn worker");
    }
    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    const auto graceful = capsid_test::shutdown_and_wait(worker, 80);
    const auto status = capsid::destroy_worker_with_status(worker);
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    if (elapsed.count() > 1500) {
        fail("worker destroy exceeded bounded deadline");
    }
    if (graceful.shutdown_result != CAPSID_OK ||
        graceful.flush_result != CAPSID_OK || graceful.wait_error != 0 ||
        graceful.event_result != CAPSID_OK || graceful.reaped ||
        graceful.channel_closed || graceful.clean() || elapsed.count() < 80) {
        fail("stubborn Worker was accepted as a graceful exit");
    }
    if (!status.observed || !status.destroy_forced || status.clean() || status.wait_error != 0) {
        fail("abortive destroy did not retain the actual forced exit");
    }
#if defined(_WIN32)
    if (status.exit_code != 1 || status.signal != 0) fail("unexpected forced OS exit");
#else
    if (status.signal != SIGKILL) fail("unexpected forced OS signal");
#endif
    std::cout << "WORKER_DESTROY_EXIT ";
    capsid_test::print_worker_exit(status);
    std::cout << std::endl;
    std::cout << "WORKER_EXIT_DEADLINE {\"observed\":false,\"clean\":false,"
                 "\"abortiveDestroyCompleted\":true,\"elapsedMs\":"
              << elapsed.count() << "}" << std::endl;
    return 0;
}
