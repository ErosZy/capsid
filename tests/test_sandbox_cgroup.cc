#include "capsid/runtime.h"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open fixture: ") + path);
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string read_control(const std::string &cgroup, const char *name) {
    std::ifstream input((cgroup + "/" + name).c_str(), std::ios::in);
    if (!input) {
        fail(std::string("cannot read cgroup control: ") + name);
    }
    std::string value;
    std::getline(input, value);
    return value;
}

bool control_exists(const std::string &cgroup, const char *name) {
    struct stat info;
    return stat((cgroup + "/" + name).c_str(), &info) == 0;
}

uint32_t wait_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail("cgroup worker flush failed");
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return event.flags;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(
                    std::string("cgroup worker startup error: ") +
                    std::string(
                        reinterpret_cast<const char *>(
                            event.payload.data),
                        event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("cgroup worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail("cgroup worker event read failed");
        }
        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events =
            POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 20);
    }
    fail("cgroup worker READY timeout");
    return 0;
}

bool cgroup_contains_pid(const std::string &cgroup, int64_t pid) {
    std::ifstream input(
        (cgroup + "/cgroup.procs").c_str(),
        std::ios::in);
    int64_t candidate = -1;
    while (input >> candidate) {
        if (candidate == pid) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
#if !defined(__linux__)
    (void)argc;
    (void)argv;
    return 77;
#else
    if (argc != 3) {
        fail("expected worker path and fixture path");
    }
    const std::string cgroup =
        std::string("/sys/fs/cgroup/capsid-runtime-test-") +
        std::to_string(getpid());
    if (mkdir(cgroup.c_str(), 0755) != 0) {
        if (errno == EACCES || errno == EPERM || errno == EROFS) {
            std::cerr << "cgroup v2 delegation unavailable: "
                      << std::strerror(errno) << std::endl;
            return 77;
        }
        fail(std::string("cannot create cgroup: ") + std::strerror(errno));
    }

    const char *controls[] = {
        "cpu.max",
        "cpu.weight",
        "memory.high",
        "memory.max",
        "memory.swap.max",
        "pids.max",
    };
    for (size_t index = 0;
         index < sizeof(controls) / sizeof(controls[0]);
         ++index) {
        if (!control_exists(cgroup, controls[index])) {
            std::cerr << "cgroup v2 controller unavailable: "
                      << controls[index] << std::endl;
            rmdir(cgroup.c_str());
            return 77;
        }
    }

    const std::string original_cpu_max =
        read_control(cgroup, "cpu.max");
    const std::string original_memory_max =
        read_control(cgroup, "memory.max");
    capsid_resource_limits rollback_probe;
    capsid_resource_limits_init(&rollback_probe);
    rollback_probe.enabled_fields =
        CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_MAX;
    rollback_probe.cgroup_cpu_quota_us = 40000;
    rollback_probe.cgroup_cpu_period_us = 100000;
    /*
     * The kernel canonicalizes a one-byte memory limit to its page
     * granularity. The runtime must detect that readback mismatch, fail
     * startup, and restore the earlier cpu.max write.
     */
    rollback_probe.cgroup_memory_max_bytes = 1;
    capsid_worker_config rollback_config;
    capsid_worker_config_init(&rollback_config);
    rollback_config.worker_path = argv[1];
    rollback_config.strict_sandbox = 1;
    rollback_config.sandbox_cgroup_path = cgroup.c_str();
    rollback_config.resource_limits = &rollback_probe;
    capsid_worker *rejected_worker = NULL;
    if (capsid_worker_spawn(&rollback_config, &rejected_worker) !=
            CAPSID_SYSTEM_ERROR ||
        rejected_worker != NULL ||
        read_control(cgroup, "cpu.max") != original_cpu_max ||
        read_control(cgroup, "memory.max") != original_memory_max) {
        rmdir(cgroup.c_str());
        fail("failed cgroup transaction did not roll back controller values");
    }

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.strict_sandbox = 1;
    config.sandbox_cgroup_path = cgroup.c_str();
    capsid_resource_limits limits;
    capsid_resource_limits_init(&limits);
    limits.enabled_fields =
        CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_CPU_WEIGHT |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_HIGH |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_SWAP_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_PIDS_MAX;
    limits.cgroup_cpu_quota_us = 50000;
    limits.cgroup_cpu_period_us = 100000;
    limits.cgroup_cpu_weight = 123;
    limits.cgroup_memory_high_bytes = 128u * 1024u * 1024u;
    limits.cgroup_memory_max_bytes = 256u * 1024u * 1024u;
    limits.cgroup_memory_swap_max_bytes = 0;
    limits.cgroup_pids_max = 8;
    config.resource_limits = &limits;

    capsid_worker *worker = NULL;
    const capsid_result spawned = capsid_worker_spawn(&config, &worker);
    if (spawned != CAPSID_OK) {
        rmdir(cgroup.c_str());
        fail(std::string("cgroup worker spawn: ") +
             capsid_result_string(spawned));
    }
    if (!cgroup_contains_pid(cgroup, capsid_worker_pid(worker))) {
        capsid_worker_destroy(worker);
        rmdir(cgroup.c_str());
        fail("spawned worker is not a member of the requested cgroup");
    }
    if (read_control(cgroup, "cpu.max") != "50000 100000" ||
        read_control(cgroup, "cpu.weight") != "123" ||
        read_control(cgroup, "memory.high") != "134217728" ||
        read_control(cgroup, "memory.max") != "268435456" ||
        read_control(cgroup, "memory.swap.max") != "0" ||
        read_control(cgroup, "pids.max") != "8") {
        capsid_worker_destroy(worker);
        rmdir(cgroup.c_str());
        fail("cgroup controller values were not configured and verified");
    }
    const std::string bundle = read_file(argv[2]);
    const capsid_result loaded = capsid_worker_load_bundle(
        worker,
        reinterpret_cast<const uint8_t *>(bundle.data()),
        bundle.size());
    if (loaded != CAPSID_OK) {
        capsid_worker_destroy(worker);
        rmdir(cgroup.c_str());
        fail("cgroup worker bundle load failed");
    }
    const uint32_t features = wait_ready(worker);
    if ((features & CAPSID_SANDBOX_FEATURE_STRICT_BASE) !=
            CAPSID_SANDBOX_FEATURE_STRICT_BASE ||
        (features & CAPSID_SANDBOX_FEATURE_CGROUP_V2) == 0) {
        capsid_worker_destroy(worker);
        rmdir(cgroup.c_str());
        fail("cgroup worker did not report enforced features");
    }
    capsid_worker_destroy(worker);

    capsid_resource_limits_init(&limits);
    limits.enabled_fields =
        CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_HIGH |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_SWAP_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_PIDS_MAX;
    limits.cgroup_cpu_quota_us = CAPSID_RESOURCE_UNLIMITED;
    limits.cgroup_cpu_period_us = 100000;
    limits.cgroup_memory_high_bytes = CAPSID_RESOURCE_UNLIMITED;
    limits.cgroup_memory_max_bytes = CAPSID_RESOURCE_UNLIMITED;
    limits.cgroup_memory_swap_max_bytes = CAPSID_RESOURCE_UNLIMITED;
    limits.cgroup_pids_max = CAPSID_RESOURCE_PIDS_UNLIMITED;
    config.resource_limits = &limits;

    worker = NULL;
    if (capsid_worker_spawn(&config, &worker) != CAPSID_OK ||
        !cgroup_contains_pid(cgroup, capsid_worker_pid(worker)) ||
        read_control(cgroup, "cpu.max") != "max 100000" ||
        read_control(cgroup, "memory.high") != "max" ||
        read_control(cgroup, "memory.max") != "max" ||
        read_control(cgroup, "memory.swap.max") != "max" ||
        read_control(cgroup, "pids.max") != "max") {
        if (worker) {
            capsid_worker_destroy(worker);
        }
        rmdir(cgroup.c_str());
        fail("cgroup unlimited sentinel was not applied");
    }
    if (capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()) != CAPSID_OK) {
        capsid_worker_destroy(worker);
        rmdir(cgroup.c_str());
        fail("unlimited cgroup worker bundle load failed");
    }
    wait_ready(worker);
    capsid_worker_destroy(worker);

    if (rmdir(cgroup.c_str()) != 0) {
        fail(std::string("cannot remove test cgroup: ") +
             std::strerror(errno));
    }
    return 0;
#endif
}
