#ifndef CAPSID_CAPABILITY_POLICY_H
#define CAPSID_CAPABILITY_POLICY_H

#include "egress_policy.h"
#include "capsid/runtime.h"

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace capsid {

enum ModuleDecision {
    kModuleGranted,
    kModuleDenied,
    kModuleUnavailable,
    kModuleForbidden
};

struct PermissionDecision {
    capsid_permission_state state;
    uint32_t rule_id;
    std::string resource;

    PermissionDecision(
        capsid_permission_state value = CAPSID_PERMISSION_STATE_DENIED,
        uint32_t matched_rule = 0,
        const std::string &normalized_resource = std::string())
        : state(value),
          rule_id(matched_rule),
          resource(normalized_resource) {}
};

class CapabilityPolicy {
public:
    struct Rule {
        capsid_permission_action action;
        capsid_permission_name permission;
        std::string resource;
        uint32_t rule_id;
    };

    CapabilityPolicy();

    bool configure(const capsid_capability_policy *policy,
                   std::string *error);

    bool enabled() const { return enabled_; }
    uint32_t version() const { return version_; }
    const std::string &application_identity() const {
        return application_identity_;
    }
    const std::vector<std::string> &allowed_modules() const {
        return allowed_modules_;
    }
    const std::vector<Rule> &rules() const { return rules_; }
    const std::vector<std::pair<std::string, std::string> >
        &env_entries() const {
        return env_entries_;
    }
    bool has_net_policy() const { return has_net_policy_; }
    const EgressPolicy &net_policy() const { return net_policy_; }

    ModuleDecision module_decision(const std::string &name) const;
    PermissionDecision evaluate(
        capsid_permission_name permission,
        const std::string &resource) const;
    PermissionDecision query(
        capsid_permission_name permission,
        const std::string &resource,
        uint16_t port) const;
    bool env_value(const std::string &name,
                   std::string *value) const;

    void swap(CapabilityPolicy &other);

private:
    bool enabled_;
    uint32_t version_;
    std::string application_identity_;
    std::vector<std::string> allowed_modules_;
    std::vector<Rule> rules_;
    std::vector<std::pair<std::string, std::string> > env_entries_;
    bool has_net_policy_;
    EgressPolicy net_policy_;
};

const char *permission_name(capsid_permission_name permission);
const char *permission_resource_kind(capsid_permission_name permission);
const char *permission_state_name(capsid_permission_state state);
const char *capability_manifest_hash();

}  // namespace capsid

#endif
