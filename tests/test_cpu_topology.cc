#include "cpu_topology.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main() {
    require(
        capsid::topology::parse_cpu_max("max 100000") == 0.0,
        "unlimited quota");
    require(
        std::fabs(
            capsid::topology::parse_cpu_max("150000 100000") -
            1.5) < 0.000001,
        "fractional quota");
    require(
        capsid::topology::parse_cpu_max("invalid") == 0.0,
        "invalid quota");
    require(
        capsid::topology::recommended_worker_count(4, 0.0) == 4,
        "affinity-only count");
    require(
        capsid::topology::recommended_worker_count(4, 1.5) == 2,
        "fractional quota rounds up");
    require(
        capsid::topology::recommended_worker_count(4, 8.0) == 4,
        "quota bounded by affinity");
    require(
        capsid::topology::recommended_worker_count(0, 0.0) == 1,
        "fallback count");
    return 0;
}
