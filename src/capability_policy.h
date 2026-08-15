#ifndef CAPSID_CAPABILITY_POLICY_H
#define CAPSID_CAPABILITY_POLICY_H

#include "egress_policy.h"
#include "capsid/runtime.h"

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace capsid {

// Defined in ipc_validation.h (which includes this header); the policy
// set only needs the incomplete type in its declarations.
struct WorkerBindingDescriptor;

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

// --- Binding v1 origin model (docs/binding-technical-design.md §3.1) -----
//
// Native operations run under exactly one origin: the User Runtime or one
// Binding Runtime. Origin comes from the JSContext/opaque carrying the
// operation — never from JavaScript values — so app code cannot forge it.

enum class RuntimeDomain {
    kUser,
    kBinding,
};

struct RuntimeOrigin {
    RuntimeDomain domain = RuntimeDomain::kUser;
    std::string binding_id;  // empty for kUser; required for kBinding

    bool valid() const {
        return domain == RuntimeDomain::kUser ||
               (domain == RuntimeDomain::kBinding &&
                !binding_id.empty() && binding_id.size() <= 63);
    }
};

// Recorded when a Native Handle is created: the owning origin plus the
// creation-time open mode. Every later operation must present a matching
// origin before the syscall; a handle without a recorded owner, or a
// caller origin that does not match, fails closed. `recorded` separates
// a default-constructed owner from one actually attached to a handle.
struct NativeHandleOwner {
    RuntimeOrigin origin;
    uint32_t open_mode = 0;
    bool recorded = false;

    bool valid() const { return recorded && origin.valid(); }
};

bool handle_owner_matches(const NativeHandleOwner &owner,
                          const RuntimeOrigin &caller);

// The §3.3 grantable-module set for the Binding Runtime, restricted to
// what this TJS build dispatches (the builtins table plus
// tjs:internal/core). Shared by the wire decoder and the policy set.
bool binding_module_known(const std::string &name);

// The §4.1 fixed sandbox profile names. Unknown names fail closed at the
// wire and at policy compilation.
bool binding_profile_known(const std::string &name);

// §7.4: the READY sandbox profile digest — "sha256:" over the canonical
// (sorted, de-duplicated, newline-joined) union of every binding's
// sandbox.requires profiles. Empty for zero-binding workers. Independent
// of descriptor arrival order.
std::string compute_binding_profile_digest(
    const std::vector<WorkerBindingDescriptor> &bindings);

// Per-binding policy: the Manifest ∩ App intersection for one Binding ID.
// Module grants use Binding semantics (granted set vs the permanently
// forbidden set); the User facade modules are never granted here.
struct BindingPolicy {
    std::string binding_id;
    std::vector<std::string> modules;   // granted tjs modules
    std::vector<std::string> profiles;  // sandbox requires
    std::vector<std::string> env;       // readable env names
    std::vector<std::string> stdio;     // granted streams
    CapabilityPolicy capability;        // fs READ/WRITE rules
    EgressPolicy egress;                // net allow targets
    bool has_net_policy = false;

    ModuleDecision module_decision(const std::string &name) const;
};

// The worker's binding policy table, compiled from the LOAD_BINDING wire
// descriptors. Fully separate from the User policy: a Binding grant can
// never widen what the User gate allows, and an unknown Binding ID
// evaluates DENIED (never an exception, never a default allow).
class BindingPolicySet {
public:
    BindingPolicySet();

    // Fail-closed and atomic: any invalid descriptor rejects the whole
    // set and leaves no partial state behind.
    bool configure(const std::vector<WorkerBindingDescriptor> &bindings,
                   std::string *error);

    bool has(const std::string &id) const;
    const BindingPolicy *policy(const std::string &id) const;
    PermissionDecision evaluate(const std::string &id,
                                capsid_permission_name permission,
                                const std::string &resource) const;
    std::vector<std::string> ids() const;
    size_t size() const { return policies_.size(); }

private:
    std::vector<BindingPolicy> policies_;
};

}  // namespace capsid

#endif
