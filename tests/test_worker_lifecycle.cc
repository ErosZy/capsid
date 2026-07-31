#include "capsid/runtime.h"

#include <chrono>
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
    capsid_worker_destroy(worker);
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    if (elapsed.count() > 1500) {
        fail("worker destroy exceeded bounded deadline");
    }
    return 0;
}
