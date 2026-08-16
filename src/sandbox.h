#ifndef CAPSID_SANDBOX_H
#define CAPSID_SANDBOX_H

#include <stdint.h>

#include <string>
#include <vector>

#include "capsid/runtime.h"

namespace capsid {

// Binding v1 §7.9: the kernel's SECCOMP_MODE_FILTER state value (the
// userspace headers only ship the SET_MODE_FILTER operation constant).
// Reported in the READY proof; the Host compares it as the installed
// filter mode.
static constexpr uint32_t kSeccompModeFilter = 2u;

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
    // Binding fs paths entering the process-level Landlock union; the
    // per-origin Native gates stay separate and authoritative.
    std::vector<std::string> binding_read_paths;
    std::vector<std::string> binding_write_paths;
};

bool apply_sandbox(const SandboxConfig &config,
                   uint32_t *applied_features,
                   uint32_t *landlock_abi,
                   uint32_t *seccomp_mode,
                   std::string *error);

// Read-only Host/worker proof helpers. They never install or relax a sandbox.
// The namespace identity is stable for the lifetime of the namespace and is
// empty when no descriptor is configured.
uint32_t query_landlock_abi();
std::string network_namespace_identity(int descriptor);

}  // namespace capsid

#endif
