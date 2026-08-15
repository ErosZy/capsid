#ifndef CAPSID_SANDBOX_H
#define CAPSID_SANDBOX_H

#include <stdint.h>

#include <string>
#include <vector>

#include "capsid/runtime.h"

namespace capsid {

struct SandboxConfig {
    SandboxConfig()
        : address_space_limit(0),
          file_descriptor_limit(64),
          strict(false),
          required_features(0),
          preinstalled_features(0),
          network_namespace_fd(-1) {}

    uint64_t address_space_limit;
    uint32_t file_descriptor_limit;
    bool strict;
    uint32_t required_features;
    uint32_t preinstalled_features;
    int network_namespace_fd;
    std::vector<std::string> read_only_paths;
    // Binding v1 §4.1: the union of every Binding's sandbox.requires
    // profiles. The process-level filter is the union; per-origin gates
    // stay separate. Consumed by the Linux launcher (Binding §7.9);
    // ignored on platforms without kernel-level profiles.
    std::vector<std::string> binding_profiles;
};

bool apply_sandbox(const SandboxConfig &config,
                   uint32_t *applied_features,
                   std::string *error);

}  // namespace capsid

#endif
