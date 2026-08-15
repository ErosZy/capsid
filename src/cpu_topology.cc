#include "cpu_topology.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#if defined(__linux__)
#include <sched.h>
#elif defined(_WIN32)
#include "win32_compat.h"
#endif

namespace capsid {
namespace topology {

std::vector<uint32_t> available_cpus() {
    std::vector<uint32_t> output;
#if defined(__linux__)
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
        return output;
    }
    for (uint32_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &affinity)) {
            output.push_back(cpu);
        }
    }
#elif defined(_WIN32)
    // The process affinity mask reports the CPUs the worker may run on,
    // mirroring sched_getaffinity on Linux.
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (!GetProcessAffinityMask(
            GetCurrentProcess(), &process_mask, &system_mask)) {
        return output;
    }
    const uint32_t width = sizeof(DWORD_PTR) * 8;
    for (uint32_t cpu = 0; cpu < width; ++cpu) {
        if ((process_mask & (static_cast<DWORD_PTR>(1) << cpu)) != 0) {
            output.push_back(cpu);
        }
    }
#endif
    return output;
}

double parse_cpu_max(const std::string &value) {
    std::istringstream input(value);
    std::string quota_text;
    uint64_t period = 0;
    std::string trailing;
    if (!(input >> quota_text >> period) ||
        period == 0 ||
        (input >> trailing) ||
        quota_text == "max") {
        return 0.0;
    }
    std::istringstream quota_input(quota_text);
    uint64_t quota = 0;
    if (!(quota_input >> quota) || quota == 0 ||
        (quota_input >> trailing)) {
        return 0.0;
    }
    return static_cast<double>(quota) /
        static_cast<double>(period);
}

double cgroup_v2_cpu_quota() {
#if !defined(__linux__)
    return 0.0;
#else
    std::ifstream membership("/proc/self/cgroup");
    if (!membership) {
        return 0.0;
    }
    std::string relative;
    std::string line;
    while (std::getline(membership, line)) {
        if (line.compare(0, 3, "0::") == 0) {
            relative = line.substr(3);
            break;
        }
    }
    if (relative.empty() || relative.find("..") != std::string::npos) {
        return 0.0;
    }
    const std::string path =
        std::string("/sys/fs/cgroup") + relative +
        (relative[relative.size() - 1] == '/' ? "" : "/") +
        "cpu.max";
    std::ifstream input(path.c_str());
    if (!input) {
        return 0.0;
    }
    std::string value;
    std::getline(input, value);
    return parse_cpu_max(value);
#endif
}

uint32_t recommended_worker_count(
    size_t available,
    double quota_cores) {
    uint32_t count = available == 0
        ? 1
        : available > UINT32_MAX
            ? UINT32_MAX
            : static_cast<uint32_t>(available);
    if (quota_cores > 0.0) {
        const double rounded = std::ceil(quota_cores);
        const uint32_t quota_workers =
            rounded > static_cast<double>(UINT32_MAX)
                ? UINT32_MAX
                : rounded < 1.0
                    ? 1
                    : static_cast<uint32_t>(rounded);
        if (quota_workers < count) {
            count = quota_workers;
        }
    }
    return count == 0 ? 1 : count;
}

}  // namespace topology
}  // namespace capsid
