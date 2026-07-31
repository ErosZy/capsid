#include "capsid/runtime.hpp"

#include <type_traits>

static_assert(!std::is_copy_constructible<capsid::Worker>::value, "Worker must be move-only");
static_assert(std::is_move_constructible<capsid::Worker>::value, "Worker must be movable");
static_assert(
    !std::is_copy_constructible<
        capsid::CapabilityPolicyBuilder>::value,
    "CapabilityPolicyBuilder must not expose stale copied pointers");
static_assert(
    std::is_same<
        decltype(capsid::recommended_worker_count()),
        uint32_t>::value,
    "recommended worker count type drifted");

int main() {
    capsid::CapabilityPolicyBuilder builder;
    builder
        .application_identity("cpp-test")
        .allow_module("capsid:permissions")
        .allow_module("capsid:env")
        .allow(CAPSID_PERMISSION_RAW_SOCKET, 1)
        .allow(CAPSID_PERMISSION_ENV, "APP_*", 2)
        .deny(CAPSID_PERMISSION_ENV, "APP_SECRET", 3)
        .environment("APP_MODE", "test")
        .net(
            CAPSID_EGRESS_ALLOW,
            "api.example.com",
            443,
            443,
            4);
    const capsid_capability_policy &policy =
        builder.descriptor();
    if (!policy.application_identity ||
        std::string(policy.application_identity) != "cpp-test" ||
        policy.allowed_module_count != 2 ||
        policy.rule_count != 3 ||
        policy.env_entry_count != 1 ||
        std::string(policy.env_entries[0].name) != "APP_MODE" ||
        std::string(policy.env_entries[0].value) != "test" ||
        !policy.net_policy ||
        policy.net_policy->rule_count != 1 ||
        policy.net_policy->rules[0].rule_id != 4) {
        return 1;
    }
    return 0;
}
