#ifndef CAPSID_RUNTIME_HPP
#define CAPSID_RUNTIME_HPP

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capsid/runtime.h"

namespace capsid {

inline uint32_t recommended_worker_count() {
    return capsid_recommended_worker_count();
}

inline std::vector<uint32_t> available_cpus() {
    const uint32_t count = capsid_available_cpu_count();
    std::vector<uint32_t> output;
    output.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t cpu = 0;
        if (capsid_available_cpu_at(index, &cpu) != CAPSID_OK) {
            throw std::runtime_error("could not enumerate available CPUs");
        }
        output.push_back(cpu);
    }
    return output;
}

class CapabilityPolicyBuilder {
public:
    CapabilityPolicyBuilder()
        : net_default_(CAPSID_EGRESS_DENY),
          has_net_policy_(false) {
        capsid_capability_policy_init(&descriptor_);
        capsid_egress_policy_init(&net_descriptor_);
    }

    CapabilityPolicyBuilder &application_identity(
        const std::string &identity) {
        application_identity_ = identity;
        return *this;
    }

    CapabilityPolicyBuilder &allow_module(
        const std::string &module) {
        modules_.push_back(module);
        return *this;
    }

    CapabilityPolicyBuilder &allow(
        capsid_permission_name permission,
        const std::string &resource,
        uint32_t rule_id) {
        add_rule(
            CAPSID_PERMISSION_ALLOW,
            permission,
            resource,
            true,
            rule_id);
        return *this;
    }

    CapabilityPolicyBuilder &deny(
        capsid_permission_name permission,
        const std::string &resource,
        uint32_t rule_id) {
        add_rule(
            CAPSID_PERMISSION_DENY,
            permission,
            resource,
            true,
            rule_id);
        return *this;
    }

    CapabilityPolicyBuilder &allow(
        capsid_permission_name permission,
        uint32_t rule_id) {
        add_rule(
            CAPSID_PERMISSION_ALLOW,
            permission,
            std::string(),
            false,
            rule_id);
        return *this;
    }

    CapabilityPolicyBuilder &deny(
        capsid_permission_name permission,
        uint32_t rule_id) {
        add_rule(
            CAPSID_PERMISSION_DENY,
            permission,
            std::string(),
            false,
            rule_id);
        return *this;
    }

    CapabilityPolicyBuilder &environment(
        const std::string &name,
        const std::string &value) {
        EnvSpec spec;
        spec.name = name;
        spec.value = value;
        environment_.push_back(spec);
        return *this;
    }

    CapabilityPolicyBuilder &net_default(
        capsid_egress_action action) {
        net_default_ = action;
        has_net_policy_ = true;
        return *this;
    }

    CapabilityPolicyBuilder &net(
        capsid_egress_action action,
        const std::string &target,
        uint16_t port_start,
        uint16_t port_end,
        uint32_t rule_id) {
        NetSpec spec;
        spec.action = action;
        spec.target = target;
        spec.port_start = port_start;
        spec.port_end = port_end;
        spec.rule_id = rule_id;
        net_rules_.push_back(spec);
        has_net_policy_ = true;
        return *this;
    }

    const capsid_capability_policy &descriptor() const {
        rebuild();
        return descriptor_;
    }

    CapabilityPolicyBuilder(const CapabilityPolicyBuilder &) = delete;
    CapabilityPolicyBuilder &operator=(
        const CapabilityPolicyBuilder &) = delete;

private:
    struct RuleSpec {
        capsid_permission_action action;
        capsid_permission_name permission;
        std::string resource;
        bool has_resource;
        uint32_t rule_id;
    };

    struct NetSpec {
        capsid_egress_action action;
        std::string target;
        uint16_t port_start;
        uint16_t port_end;
        uint32_t rule_id;
    };

    struct EnvSpec {
        std::string name;
        std::string value;
    };

    void add_rule(capsid_permission_action action,
                  capsid_permission_name permission,
                  const std::string &resource,
                  bool has_resource,
                  uint32_t rule_id) {
        RuleSpec spec;
        spec.action = action;
        spec.permission = permission;
        spec.resource = resource;
        spec.has_resource = has_resource;
        spec.rule_id = rule_id;
        rules_.push_back(spec);
    }

    void rebuild() const {
        module_pointers_.resize(modules_.size());
        for (size_t index = 0; index < modules_.size(); ++index) {
            module_pointers_[index] = modules_[index].c_str();
        }
        rule_descriptors_.resize(rules_.size());
        for (size_t index = 0; index < rules_.size(); ++index) {
            capsid_permission_rule_init(
                &rule_descriptors_[index]);
            rule_descriptors_[index].action =
                rules_[index].action;
            rule_descriptors_[index].permission =
                rules_[index].permission;
            rule_descriptors_[index].resource =
                rules_[index].has_resource
                    ? rules_[index].resource.c_str()
                    : NULL;
            rule_descriptors_[index].rule_id =
                rules_[index].rule_id;
        }
        net_rule_descriptors_.resize(net_rules_.size());
        for (size_t index = 0;
             index < net_rules_.size();
             ++index) {
            capsid_egress_rule_init(
                &net_rule_descriptors_[index]);
            net_rule_descriptors_[index].action =
                net_rules_[index].action;
            net_rule_descriptors_[index].target =
                net_rules_[index].target.c_str();
            net_rule_descriptors_[index].port_start =
                net_rules_[index].port_start;
            net_rule_descriptors_[index].port_end =
                net_rules_[index].port_end;
            net_rule_descriptors_[index].rule_id =
                net_rules_[index].rule_id;
        }

        capsid_egress_policy_init(&net_descriptor_);
        net_descriptor_.default_action = net_default_;
        net_descriptor_.rules =
            net_rule_descriptors_.empty()
                ? NULL
                : &net_rule_descriptors_[0];
        net_descriptor_.rule_count =
            static_cast<uint32_t>(
                net_rule_descriptors_.size());

        capsid_capability_policy_init(&descriptor_);
        descriptor_.application_identity =
            application_identity_.empty()
                ? NULL
                : application_identity_.c_str();
        descriptor_.allowed_modules =
            module_pointers_.empty()
                ? NULL
                : &module_pointers_[0];
        descriptor_.allowed_module_count =
            static_cast<uint32_t>(module_pointers_.size());
        descriptor_.rules =
            rule_descriptors_.empty()
                ? NULL
                : &rule_descriptors_[0];
        descriptor_.rule_count =
            static_cast<uint32_t>(
                rule_descriptors_.size());
        descriptor_.net_policy =
            has_net_policy_ ? &net_descriptor_ : NULL;
        env_descriptors_.resize(environment_.size());
        for (size_t index = 0;
             index < environment_.size();
             ++index) {
            capsid_env_entry_init(&env_descriptors_[index]);
            env_descriptors_[index].name =
                environment_[index].name.c_str();
            env_descriptors_[index].value =
                environment_[index].value.c_str();
        }
        descriptor_.env_entries =
            env_descriptors_.empty()
                ? NULL
                : &env_descriptors_[0];
        descriptor_.env_entry_count =
            static_cast<uint32_t>(
                env_descriptors_.size());
    }

    std::string application_identity_;
    std::vector<std::string> modules_;
    std::vector<RuleSpec> rules_;
    capsid_egress_action net_default_;
    std::vector<NetSpec> net_rules_;
    bool has_net_policy_;
    std::vector<EnvSpec> environment_;

    mutable std::vector<const char *> module_pointers_;
    mutable std::vector<capsid_permission_rule>
        rule_descriptors_;
    mutable std::vector<capsid_egress_rule>
        net_rule_descriptors_;
    mutable std::vector<capsid_env_entry>
        env_descriptors_;
    mutable capsid_egress_policy net_descriptor_;
    mutable capsid_capability_policy descriptor_;
};

class Error : public std::runtime_error {
public:
    explicit Error(capsid_result result)
        : std::runtime_error(capsid_result_string(result)), result_(result) {}

    capsid_result result() const { return result_; }

private:
    capsid_result result_;
};

class Worker {
public:
    explicit Worker(const capsid_worker_config &config) : handle_(NULL) {
        const capsid_result result = capsid_worker_spawn(&config, &handle_);
        if (result != CAPSID_OK) {
            throw Error(result);
        }
    }

    ~Worker() {
        if (handle_) {
            capsid_worker_destroy(handle_);
        }
    }

    Worker(Worker &&other) noexcept : handle_(other.handle_) { other.handle_ = NULL; }

    Worker &operator=(Worker &&other) noexcept {
        if (this != &other) {
            if (handle_) {
                capsid_worker_destroy(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = NULL;
        }
        return *this;
    }

    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    int fd() const { return capsid_worker_fd(handle_); }
    int64_t pid() const { return capsid_worker_pid(handle_); }
    capsid_worker *get() const { return handle_; }
    void set_cpu_affinity(uint32_t cpu) {
        const capsid_result result =
            capsid_worker_set_cpu_affinity(handle_, cpu);
        if (result != CAPSID_OK) {
            throw Error(result);
        }
    }

private:
    capsid_worker *handle_;
};

}  // namespace capsid

#endif
