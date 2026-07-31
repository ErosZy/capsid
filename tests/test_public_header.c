#include "capsid/runtime.h"

int main(void) {
    if (sizeof(capsid_egress_action) != sizeof(uint32_t) ||
        sizeof(capsid_permission_action) != sizeof(uint32_t) ||
        sizeof(capsid_permission_name) != sizeof(uint32_t) ||
        sizeof(capsid_permission_state) != sizeof(uint32_t)) {
        return 1;
    }
    capsid_egress_rule egress_rule;
    capsid_egress_rule_init(&egress_rule);
    if (egress_rule.struct_size != sizeof(egress_rule) ||
        egress_rule.action != CAPSID_EGRESS_DENY ||
        egress_rule.target != NULL ||
        egress_rule.port_start != 0 ||
        egress_rule.port_end != 0 ||
        egress_rule.rule_id != 0 ||
        egress_rule.reserved != 0) {
        return 8;
    }

    capsid_egress_policy egress_policy;
    capsid_egress_policy_init(&egress_policy);
    if (egress_policy.struct_size != sizeof(egress_policy) ||
        egress_policy.default_action != CAPSID_EGRESS_DENY ||
        egress_policy.rules != NULL ||
        egress_policy.rule_count != 0 ||
        egress_policy.reserved != 0) {
        return 9;
    }

    capsid_resource_limits limits;
    capsid_resource_limits_init(&limits);
    if (limits.struct_size != sizeof(limits) ||
        limits.enabled_fields != 0 ||
        limits.reserved != 0) {
        return 5;
    }

    capsid_permission_rule permission_rule;
    capsid_permission_rule_init(&permission_rule);
    if (permission_rule.struct_size != sizeof(permission_rule) ||
        permission_rule.permission != CAPSID_PERMISSION_NONE ||
        permission_rule.action != CAPSID_PERMISSION_DENY ||
        permission_rule.resource != NULL ||
        permission_rule.rule_id != 0 ||
        permission_rule.reserved != 0) {
        return 10;
    }

    capsid_env_entry environment;
    capsid_env_entry_init(&environment);
    if (environment.struct_size != sizeof(environment) ||
        environment.name != NULL ||
        environment.value != NULL ||
        environment.reserved != 0) {
        return 11;
    }

    capsid_capability_policy capability_policy;
    capsid_capability_policy_init(&capability_policy);
    if (capability_policy.struct_size != sizeof(capability_policy) ||
        capability_policy.version !=
            CAPSID_CAPABILITY_POLICY_VERSION ||
        capability_policy.application_identity != NULL ||
        capability_policy.allowed_modules != NULL ||
        capability_policy.allowed_module_count != 0 ||
        capability_policy.rules != NULL ||
        capability_policy.rule_count != 0 ||
        capability_policy.net_policy != NULL ||
        capability_policy.reserved != 0 ||
        capability_policy.env_entries != NULL ||
        capability_policy.env_entry_count != 0 ||
        capability_policy.env_reserved != 0) {
        return 12;
    }

    capsid_audit_record audit;
    capsid_audit_record_init(&audit);
    if (audit.struct_size != sizeof(audit) ||
        audit.version != 0 ||
        audit.worker_id != 0 ||
        audit.request_id != 0 ||
        audit.application_identity.data != NULL ||
        audit.manifest_hash.size != 0) {
        return 13;
    }
    if (capsid_audit_record_decode(NULL, &audit) !=
            CAPSID_INVALID_ARGUMENT ||
        CAPSID_EVENT_AUDIT <= CAPSID_EVENT_REQUEST_TIMEOUT) {
        return 14;
    }
    {
        capsid_memory_metrics metrics;
        uint8_t payload[4 + 18 * 8] = {0};
        capsid_event event = {0};
        size_t index;
        payload[0] = CAPSID_MEMORY_METRICS_VERSION;
        for (index = 0; index < 18; ++index) {
            payload[4 + index * 8] =
                (uint8_t)(index + 1);
        }
        event.struct_size = sizeof(event);
        event.type = CAPSID_EVENT_MEMORY_METRICS;
        event.payload.data = payload;
        event.payload.size = sizeof(payload);
        capsid_memory_metrics_init(&metrics);
        if (metrics.struct_size != sizeof(metrics) ||
            capsid_memory_metrics_decode(&event, &metrics) != CAPSID_OK ||
            metrics.version != CAPSID_MEMORY_METRICS_VERSION ||
            metrics.malloc_size != 1 ||
            metrics.binary_object_size != 18 ||
            CAPSID_EVENT_MEMORY_METRICS <= CAPSID_EVENT_AUDIT ||
            capsid_worker_request_memory_metrics(NULL) !=
                CAPSID_INVALID_ARGUMENT) {
            return 17;
        }
        event.payload.size--;
        capsid_memory_metrics_init(&metrics);
        if (capsid_memory_metrics_decode(&event, &metrics) !=
                CAPSID_PROTOCOL_ERROR) {
            return 18;
        }
    }

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    if (config.abi_version != CAPSID_ABI_VERSION ||
        CAPSID_ABI_VERSION != 7u) {
        return 1;
    }
    if (config.tls_ca_bundle_path != NULL ||
        config.max_fetch_request_body_bytes != 0 ||
        config.max_fetch_response_body_bytes != 0 ||
        config.sandbox_required_features != 0 ||
        config.sandbox_cgroup_path != NULL ||
        config.resource_limits != NULL ||
        config.egress_policy != NULL ||
        config.capability_policy != NULL ||
        config.sandbox_network_namespace_fd != -1 ||
        config.egress_reserved != 0) {
        return 2;
    }
    if (CAPSID_RESOURCE_LIMIT_ALL !=
        (CAPSID_RESOURCE_LIMIT_FILE_DESCRIPTORS |
         CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX |
         CAPSID_RESOURCE_LIMIT_CGROUP_CPU_WEIGHT |
         CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_HIGH |
         CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_MAX |
         CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_SWAP_MAX |
         CAPSID_RESOURCE_LIMIT_CGROUP_PIDS_MAX)) {
        return 6;
    }
    if (CAPSID_RESOURCE_UNLIMITED != UINT64_MAX ||
        CAPSID_RESOURCE_PIDS_UNLIMITED != UINT32_MAX) {
        return 7;
    }
    if ((CAPSID_SANDBOX_FEATURE_STRICT_BASE &
         (CAPSID_SANDBOX_FEATURE_RLIMITS |
          CAPSID_SANDBOX_FEATURE_NO_NEW_PRIVS |
          CAPSID_SANDBOX_FEATURE_SECCOMP |
          CAPSID_SANDBOX_FEATURE_LANDLOCK)) !=
        CAPSID_SANDBOX_FEATURE_STRICT_BASE) {
        return 4;
    }
    if (capsid_worker_load_bundle_named(NULL, NULL, 0, NULL) !=
        CAPSID_INVALID_ARGUMENT) {
        return 3;
    }
    if (capsid_worker_load_trusted_bytecode_named(NULL, NULL, 0, NULL) !=
        CAPSID_INVALID_ARGUMENT) {
        return 15;
    }
    if (capsid_response_status_text(NULL, NULL) !=
        CAPSID_INVALID_ARGUMENT) {
        return 14;
    }
    {
        const uint32_t workers = capsid_recommended_worker_count();
        const uint32_t cpus = capsid_available_cpu_count();
        uint32_t cpu = UINT32_MAX;
        if (workers == 0 ||
            (cpus != 0 &&
             (workers > cpus ||
              capsid_available_cpu_at(0, &cpu) != CAPSID_OK ||
              cpu == UINT32_MAX)) ||
            capsid_available_cpu_at(cpus, &cpu) !=
                CAPSID_INVALID_ARGUMENT ||
            capsid_worker_set_cpu_affinity(NULL, 0) !=
                CAPSID_INVALID_ARGUMENT) {
            return 15;
        }
    }
    {
        capsid_bytes status_text;
        capsid_event event = {0};
        const uint8_t valid_payload[] = {
            3, 0, 'O', 'K', '!', 0, 0
        };
        event.struct_size = sizeof(event);
        event.type = CAPSID_EVENT_RESPONSE_HEAD;
        event.payload.data = valid_payload;
        event.payload.size = sizeof(valid_payload);
        if (capsid_response_status_text(&event, &status_text) != CAPSID_OK ||
            status_text.size != 3 ||
            status_text.data[0] != 'O' ||
            status_text.data[1] != 'K' ||
            status_text.data[2] != '!') {
            return 15;
        }
    }
    {
        capsid_bytes status_text;
        capsid_event event = {0};
        const uint8_t truncated_payload[] = {4, 0, 'B', 'a'};
        event.struct_size = sizeof(event);
        event.type = CAPSID_EVENT_RESPONSE_HEAD;
        event.payload.data = truncated_payload;
        event.payload.size = sizeof(truncated_payload);
        if (capsid_response_status_text(&event, &status_text) !=
                CAPSID_PROTOCOL_ERROR ||
            status_text.data != NULL ||
            status_text.size != 0) {
            return 16;
        }
    }
    return 0;
}
