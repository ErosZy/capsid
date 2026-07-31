#include "capsid/runtime.h"

#include <cstdlib>
#include <iostream>

#if defined(__linux__)
#include <sched.h>
#endif

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
#if !defined(__linux__)
    (void)argc;
    (void)argv;
    /*
     * CPU affinity is a Linux-only capability. Exit with the CTest skip code
     * rather than 0 so that an unsupported platform is reported as "not
     * covered" instead of contributing a green pass that asserted nothing.
     */
    std::cerr << "SKIP: CPU affinity requires Linux" << std::endl;
    return 77;
#else
    require(argc == 2, "worker path argument");
    const uint32_t available = capsid_available_cpu_count();
    const uint32_t recommended = capsid_recommended_worker_count();
    require(available > 0, "available CPU count");
    require(
        recommended > 0 && recommended <= available,
        "recommended worker count fits affinity");

    uint32_t cpu = 0;
    require(
        capsid_available_cpu_at(0, &cpu) == CAPSID_OK,
        "first available CPU");

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    capsid_worker *worker = NULL;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_OK,
        "worker spawn");
    require(
        capsid_worker_set_cpu_affinity(worker, cpu) == CAPSID_OK,
        "worker affinity");

    cpu_set_t assigned;
    CPU_ZERO(&assigned);
    require(
        sched_getaffinity(
            static_cast<pid_t>(capsid_worker_pid(worker)),
            sizeof(assigned),
            &assigned) == 0,
        "read worker affinity");
    require(CPU_COUNT(&assigned) == 1, "worker pinned to one CPU");
    require(CPU_ISSET(cpu, &assigned), "worker pinned to requested CPU");
    capsid_worker_destroy(worker);
    return 0;
#endif
}
