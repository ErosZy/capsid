#ifndef CAPSID_CPU_TOPOLOGY_H
#define CAPSID_CPU_TOPOLOGY_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace capsid {
namespace topology {

std::vector<uint32_t> available_cpus();
double parse_cpu_max(const std::string &value);
double cgroup_v2_cpu_quota();
uint32_t recommended_worker_count(size_t available_cpus, double quota_cores);

}  // namespace topology
}  // namespace capsid

#endif
