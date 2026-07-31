#include "ipc_validation.h"

#include "capsid/runtime.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace {

bool reject(std::string *error, const char *message) {
    if (error) {
        *error = message;
    }
    return false;
}

bool contains_nul(const std::string &value) {
    return value.find('\0') != std::string::npos;
}

bool read_string16(const uint8_t **cursor,
                   const uint8_t *end,
                   std::string *value) {
    uint16_t size = 0;
    if (!capsid::protocol::read_u16(cursor, end, &size) ||
        static_cast<size_t>(end - *cursor) < size) {
        return false;
    }
    value->assign(
        reinterpret_cast<const char *>(*cursor), size);
    *cursor += size;
    return true;
}

bool read_string32(const uint8_t **cursor,
                   const uint8_t *end,
                   std::string *value) {
    uint32_t size = 0;
    if (!capsid::protocol::read_u32(cursor, end, &size) ||
        static_cast<uint64_t>(end - *cursor) < size) {
        return false;
    }
    value->assign(
        reinterpret_cast<const char *>(*cursor), size);
    *cursor += size;
    return true;
}

struct WireEgressPolicy {
    WireEgressPolicy() : default_action(CAPSID_EGRESS_DENY) {}

    capsid_egress_action default_action;
    std::vector<std::string> targets;
    std::vector<capsid_egress_rule> rules;
};

bool decode_wire_egress(const uint8_t **cursor,
                        const uint8_t *end,
                        WireEgressPolicy *decoded,
                        std::string *error) {
    uint32_t default_action = 0;
    uint32_t rule_count = 0;
    if (!decoded ||
        !capsid::protocol::read_u32(
            cursor, end, &default_action) ||
        !capsid::protocol::read_u32(
            cursor, end, &rule_count) ||
        default_action > CAPSID_EGRESS_ALLOW ||
        rule_count > 256) {
        return reject(error, "invalid HELLO egress header");
    }

    WireEgressPolicy candidate;
    candidate.default_action =
        static_cast<capsid_egress_action>(default_action);
    candidate.targets.resize(rule_count);
    candidate.rules.resize(rule_count);
    for (uint32_t index = 0; index < rule_count; ++index) {
        uint32_t action = 0;
        capsid_egress_rule_init(&candidate.rules[index]);
        if (!capsid::protocol::read_u32(
                cursor, end, &action) ||
            !capsid::protocol::read_u16(
                cursor,
                end,
                &candidate.rules[index].port_start) ||
            !capsid::protocol::read_u16(
                cursor,
                end,
                &candidate.rules[index].port_end) ||
            !capsid::protocol::read_u32(
                cursor,
                end,
                &candidate.rules[index].rule_id) ||
            !read_string16(
                cursor,
                end,
                &candidate.targets[index]) ||
            action > CAPSID_EGRESS_ALLOW ||
            contains_nul(candidate.targets[index])) {
            return reject(error, "invalid HELLO egress rule");
        }
        candidate.rules[index].action =
            static_cast<capsid_egress_action>(action);
        candidate.rules[index].target =
            candidate.targets[index].c_str();
    }
    *decoded = candidate;
    /*
     * std::vector copy above preserves strings but not their c_str pointers.
     * Rebind every public descriptor to the committed storage.
     */
    for (uint32_t index = 0; index < rule_count; ++index) {
        decoded->rules[index].target =
            decoded->targets[index].c_str();
    }
    return true;
}

bool configure_wire_egress(const WireEgressPolicy &wire,
                           capsid::EgressPolicy *policy,
                           std::string *error) {
    capsid_egress_policy descriptor;
    capsid_egress_policy_init(&descriptor);
    descriptor.default_action = wire.default_action;
    descriptor.rules =
        wire.rules.empty() ? NULL : &wire.rules[0];
    descriptor.rule_count =
        static_cast<uint32_t>(wire.rules.size());
    if (!policy->configure(&descriptor, error)) {
        return reject(error, "invalid HELLO egress policy");
    }
    return true;
}

bool decode_wire_capability(const uint8_t **cursor,
                            const uint8_t *end,
                            capsid::CapabilityPolicy *output,
                            std::string *error) {
    uint32_t version = 0;
    std::string application_identity;
    uint16_t module_count = 0;
    if (!output ||
        !capsid::protocol::read_u32(cursor, end, &version) ||
        !read_string16(
            cursor, end, &application_identity) ||
        contains_nul(application_identity) ||
        !capsid::protocol::read_u16(
            cursor, end, &module_count) ||
        module_count > 64) {
        return reject(error, "invalid HELLO capability header");
    }

    std::vector<std::string> modules(module_count);
    std::vector<const char *> module_pointers(module_count);
    for (uint16_t index = 0; index < module_count; ++index) {
        if (!read_string16(
                cursor, end, &modules[index]) ||
            contains_nul(modules[index])) {
            return reject(error, "invalid HELLO allowed module");
        }
        module_pointers[index] = modules[index].c_str();
    }

    uint16_t rule_count = 0;
    if (!capsid::protocol::read_u16(
            cursor, end, &rule_count) ||
        rule_count > 256) {
        return reject(error, "invalid HELLO permission header");
    }
    std::vector<std::string> resources(rule_count);
    std::vector<capsid_permission_rule> rules(rule_count);
    for (uint16_t index = 0; index < rule_count; ++index) {
        uint32_t action = 0;
        uint32_t permission = 0;
        capsid_permission_rule_init(&rules[index]);
        if (!capsid::protocol::read_u32(
                cursor, end, &action) ||
            !capsid::protocol::read_u32(
                cursor, end, &permission) ||
            !capsid::protocol::read_u32(
                cursor, end, &rules[index].rule_id) ||
            !read_string16(
                cursor, end, &resources[index]) ||
            action > CAPSID_PERMISSION_ALLOW ||
            permission < CAPSID_PERMISSION_READ ||
            permission > CAPSID_PERMISSION_ENGINE ||
            permission == CAPSID_PERMISSION_NET ||
            contains_nul(resources[index])) {
            return reject(error, "invalid HELLO permission rule");
        }
        rules[index].action =
            static_cast<capsid_permission_action>(action);
        rules[index].permission =
            static_cast<capsid_permission_name>(permission);
        rules[index].resource =
            resources[index].empty()
                ? NULL
                : resources[index].c_str();
    }

    WireEgressPolicy net;
    if (!decode_wire_egress(cursor, end, &net, error)) {
        return false;
    }
    const bool has_environment_section = *cursor != end;
    uint16_t env_entry_count = 0;
    if (has_environment_section &&
        (!capsid::protocol::read_u16(
             cursor, end, &env_entry_count) ||
         env_entry_count > 256)) {
        return reject(
            error,
            "invalid HELLO environment snapshot header");
    }
    std::vector<std::string> env_names(env_entry_count);
    std::vector<std::string> env_values(env_entry_count);
    std::vector<capsid_env_entry> env_entries(env_entry_count);
    for (uint16_t index = 0;
         index < env_entry_count;
         ++index) {
        capsid_env_entry_init(&env_entries[index]);
        if (!read_string16(
                cursor, end, &env_names[index]) ||
            !read_string16(
                cursor, end, &env_values[index]) ||
            contains_nul(env_names[index]) ||
            contains_nul(env_values[index])) {
            return reject(
                error,
                "invalid HELLO environment snapshot entry");
        }
        env_entries[index].name =
            env_names[index].c_str();
        env_entries[index].value =
            env_values[index].c_str();
    }
    if (version == 0) {
        if (!application_identity.empty() ||
            !modules.empty() ||
            !rules.empty() ||
            net.default_action != CAPSID_EGRESS_DENY ||
            !net.rules.empty() ||
            !env_entries.empty()) {
            return reject(
                error,
                "disabled HELLO capability policy has data");
        }
        if (!output->configure(NULL, error)) {
            return reject(
                error,
                "invalid disabled HELLO capability policy");
        }
        return true;
    }
    if (version != CAPSID_CAPABILITY_POLICY_VERSION_1 &&
        version != CAPSID_CAPABILITY_POLICY_VERSION) {
        return reject(
            error, "unknown HELLO capability policy version");
    }
    if (version == CAPSID_CAPABILITY_POLICY_VERSION &&
        !has_environment_section) {
        return reject(
            error,
            "version 2 capability policy lacks environment section");
    }
    if (version == CAPSID_CAPABILITY_POLICY_VERSION_1 &&
        !env_entries.empty()) {
        return reject(
            error,
            "version 1 capability policy has environment data");
    }

    capsid_egress_policy net_descriptor;
    capsid_egress_policy_init(&net_descriptor);
    net_descriptor.default_action = net.default_action;
    net_descriptor.rules =
        net.rules.empty() ? NULL : &net.rules[0];
    net_descriptor.rule_count =
        static_cast<uint32_t>(net.rules.size());

    capsid_capability_policy descriptor;
    capsid_capability_policy_init(&descriptor);
    descriptor.version = version;
    descriptor.application_identity =
        application_identity.empty()
            ? NULL
            : application_identity.c_str();
    descriptor.allowed_modules =
        module_pointers.empty() ? NULL : &module_pointers[0];
    descriptor.allowed_module_count = module_count;
    descriptor.rules = rules.empty() ? NULL : &rules[0];
    descriptor.rule_count = rule_count;
    descriptor.net_policy = &net_descriptor;
    descriptor.env_entries =
        env_entries.empty() ? NULL : &env_entries[0];
    descriptor.env_entry_count = env_entry_count;
    if (version == CAPSID_CAPABILITY_POLICY_VERSION_1) {
        descriptor.struct_size =
            offsetof(capsid_capability_policy, env_entries);
        descriptor.version =
            CAPSID_CAPABILITY_POLICY_VERSION_1;
    }
    if (!output->configure(&descriptor, error)) {
        return reject(error, "invalid HELLO capability policy");
    }
    return true;
}

bool decode_hello(const capsid::protocol::Frame &frame,
                  capsid::WorkerStartupConfig *output,
                  std::string *error) {
    if (!output || frame.type != capsid::protocol::kHello ||
        frame.flags != 0 || frame.request_id != 0 ||
        frame.payload.size() <
            capsid::protocol::kHelloLegacyFixedPayloadSize ||
        frame.payload.size() > capsid::protocol::kMaxPayloadSize) {
        return reject(error, "invalid HELLO frame");
    }

    capsid::WorkerStartupConfig decoded;
    const uint8_t *cursor = &frame.payload[0];
    const uint8_t *end = cursor + frame.payload.size();
    uint32_t abi = 0;
    uint8_t strict = 0;
    if (!capsid::protocol::read_u32(&cursor, end, &abi) ||
        abi != CAPSID_ABI_VERSION ||
        !capsid::protocol::read_u64(
            &cursor, end, &decoded.js_heap_limit) ||
        !capsid::protocol::read_u64(
            &cursor, end, &decoded.process_memory_limit) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decoded.file_descriptor_limit) ||
        !capsid::protocol::read_u64(
            &cursor, end, &decoded.timeout_ms) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decoded.js_stack_size) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decoded.max_inflight) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decoded.initial_window) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decoded.max_header_bytes) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decoded.max_queued_bytes) ||
        cursor == end) {
        return reject(error, "truncated HELLO fixed fields");
    }
    strict = *cursor++;
    decoded.strict_sandbox = strict != 0;
    if (!capsid::protocol::read_u32(
            &cursor, end, &decoded.sandbox_required_features) ||
        !capsid::protocol::read_u32(
            &cursor,
            end,
            &decoded.preinstalled_sandbox_features)) {
        return reject(error, "truncated HELLO sandbox fields");
    }

    uint16_t ca_size = 0;
    if (!capsid::protocol::read_u16(&cursor, end, &ca_size) ||
        ca_size > 4096 ||
        static_cast<size_t>(end - cursor) <
            static_cast<size_t>(ca_size) + 43) {
        return reject(error, "invalid HELLO CA path");
    }
    decoded.tls_ca_bundle_path.assign(
        reinterpret_cast<const char *>(cursor), ca_size);
    cursor += ca_size;
    if (contains_nul(decoded.tls_ca_bundle_path) ||
        !capsid::protocol::read_u64(
            &cursor,
            end,
            &decoded.max_fetch_request_body_bytes) ||
        !capsid::protocol::read_u64(
            &cursor,
            end,
            &decoded.max_fetch_response_body_bytes)) {
        return reject(error, "invalid HELLO Fetch controls");
    }

    WireEgressPolicy legacy_egress;
    if (!decode_wire_egress(
            &cursor, end, &legacy_egress, error) ||
        cursor == end) {
        return reject(error, "invalid HELLO legacy egress state");
    }
    const uint8_t legacy_egress_configured = *cursor++;
    if (legacy_egress_configured > 1 ||
        (!legacy_egress_configured &&
         (legacy_egress.default_action != CAPSID_EGRESS_DENY ||
          !legacy_egress.rules.empty())) ||
        !configure_wire_egress(
            legacy_egress,
            &decoded.egress_policy,
            error) ||
        !decode_wire_capability(
            &cursor,
            end,
            &decoded.capability_policy,
            error)) {
        return false;
    }
    decoded.legacy_egress_configured =
        legacy_egress_configured != 0;
    if (cursor != end) {
        return reject(error, "trailing HELLO payload");
    }

    const uint64_t max_safe_js_integer =
        UINT64_C(9007199254740991);
    const bool valid =
        strict <= 1 &&
        decoded.js_heap_limit <=
            static_cast<uint64_t>(
                std::numeric_limits<int>::max()) &&
        decoded.timeout_ms != 0 &&
        decoded.js_stack_size != 0 &&
        decoded.max_inflight != 0 &&
        decoded.initial_window != 0 &&
        decoded.max_header_bytes >= 8 &&
        decoded.max_header_bytes <=
            capsid::protocol::kMaxPayloadSize &&
        decoded.max_queued_bytes >=
            capsid::protocol::kHeaderSize +
                capsid::protocol::kHelloLegacyFixedPayloadSize &&
        decoded.max_queued_bytes <=
            capsid::protocol::kMaxBufferedBytes &&
        frame.payload.size() <=
            decoded.max_queued_bytes -
                capsid::protocol::kHeaderSize &&
        decoded.file_descriptor_limit >= 4 &&
        decoded.max_fetch_request_body_bytes <=
            max_safe_js_integer &&
        decoded.max_fetch_response_body_bytes <=
            max_safe_js_integer &&
        (decoded.sandbox_required_features &
         ~static_cast<uint32_t>(
             CAPSID_SANDBOX_FEATURE_ALL)) == 0 &&
        (decoded.preinstalled_sandbox_features &
         ~static_cast<uint32_t>(
             CAPSID_SANDBOX_FEATURE_CGROUP_V2)) == 0 &&
        (decoded.preinstalled_sandbox_features &
         ~decoded.sandbox_required_features) == 0 &&
        (decoded.strict_sandbox ||
         (decoded.sandbox_required_features == 0 &&
          decoded.preinstalled_sandbox_features == 0)) &&
        (!decoded.strict_sandbox ||
         (decoded.sandbox_required_features &
          CAPSID_SANDBOX_FEATURE_STRICT_BASE) ==
             CAPSID_SANDBOX_FEATURE_STRICT_BASE) &&
        (!decoded.process_memory_limit ||
         decoded.process_memory_limit >= decoded.js_heap_limit);
    if (!valid) {
        return reject(error, "invalid HELLO configuration");
    }
    *output = decoded;
    return true;
}

}  // namespace

namespace capsid {

WorkerStartupConfig::WorkerStartupConfig()
    : js_stack_size(1024u * 1024u),
      max_inflight(128),
      initial_window(256u * 1024u),
      max_header_bytes(64u * 1024u),
      max_queued_bytes(4u * 1024u * 1024u),
      file_descriptor_limit(64),
      timeout_ms(30000),
      strict_sandbox(false),
      sandbox_required_features(0),
      preinstalled_sandbox_features(0),
      js_heap_limit(64u * 1024u * 1024u),
      process_memory_limit(256u * 1024u * 1024u),
      max_fetch_request_body_bytes(0),
      max_fetch_response_body_bytes(0),
      legacy_egress_configured(false) {}

WorkerStartupState::WorkerStartupState()
    : hello_received_(false),
      bundle_started_(false),
      bundle_complete_(false),
      bundle_is_trusted_bytecode_(false),
      bundle_name_("capsid:app/main") {}

bool WorkerStartupState::consume(const protocol::Frame &frame,
                                 std::string *error) {
    if (error) {
        error->clear();
    }
    if (!hello_received_) {
        WorkerStartupConfig decoded;
        if (!decode_hello(frame, &decoded, error)) {
            return false;
        }
        config_ = decoded;
        hello_received_ = true;
        return true;
    }
    if (frame.type != protocol::kLoadBundle ||
        frame.request_id != 0 ||
        (frame.flags &
         ~(protocol::kFlagStart |
           protocol::kFlagEnd |
           protocol::kFlagBundleName |
           protocol::kFlagTrustedBytecode)) != 0 ||
        ((frame.flags & protocol::kFlagBundleName) != 0 &&
         (frame.flags & protocol::kFlagStart) == 0) ||
        ((frame.flags & protocol::kFlagTrustedBytecode) != 0 &&
         (frame.flags & protocol::kFlagStart) == 0) ||
        bundle_complete_) {
        return reject(error, "invalid startup frame sequence");
    }

    const bool starts =
        (frame.flags & protocol::kFlagStart) != 0;
    if ((starts && bundle_started_) ||
        (!starts && !bundle_started_)) {
        return reject(error, "invalid bundle start sequence");
    }
    size_t payload_offset = 0;
    std::string new_name = bundle_name_;
    if ((frame.flags & protocol::kFlagBundleName) != 0) {
        if (frame.payload.size() < sizeof(uint16_t)) {
            return reject(error, "truncated bundle name");
        }
        const uint8_t *cursor = &frame.payload[0];
        const uint8_t *end = cursor + frame.payload.size();
        uint16_t name_size = 0;
        if (!protocol::read_u16(
                &cursor, end, &name_size) ||
            name_size == 0 || name_size > 4096 ||
            static_cast<size_t>(end - cursor) < name_size) {
            return reject(error, "invalid bundle name");
        }
        new_name.assign(
            reinterpret_cast<const char *>(cursor), name_size);
        if (contains_nul(new_name)) {
            return reject(error, "NUL in bundle name");
        }
        cursor += name_size;
        payload_offset =
            static_cast<size_t>(cursor - &frame.payload[0]);
    }
    const size_t payload_size =
        frame.payload.size() - payload_offset;
    if (bundle_.size() > config_.max_queued_bytes ||
        payload_size >
            config_.max_queued_bytes - bundle_.size()) {
        return reject(error, "bundle buffer limit exceeded");
    }

    if (starts) {
        bundle_started_ = true;
        bundle_name_ = new_name;
        bundle_is_trusted_bytecode_ =
            (frame.flags & protocol::kFlagTrustedBytecode) != 0;
    }
    bundle_.insert(
        bundle_.end(),
        frame.payload.begin() +
            static_cast<ptrdiff_t>(payload_offset),
        frame.payload.end());
    if ((frame.flags & protocol::kFlagEnd) != 0) {
        bundle_complete_ = true;
    }
    return true;
}

bool decode_worker_request_head(const protocol::Frame &frame,
                                uint32_t max_header_bytes,
                                WorkerRequestHead *output,
                                std::string *error) {
    if (error) {
        error->clear();
    }
    if (!output || frame.type != protocol::kRequestHead ||
        frame.flags != 0 || frame.request_id == 0 ||
        frame.payload.size() < 8 ||
        frame.payload.size() > max_header_bytes ||
        frame.payload.size() > protocol::kMaxPayloadSize) {
        return reject(error, "invalid request head frame");
    }
    const uint8_t *cursor = &frame.payload[0];
    const uint8_t *end = cursor + frame.payload.size();
    WorkerRequestHead decoded;
    uint16_t count = 0;
    if (!read_string16(&cursor, end, &decoded.method) ||
        !read_string32(&cursor, end, &decoded.url) ||
        decoded.method.empty() || decoded.url.empty() ||
        !protocol::read_u16(&cursor, end, &count)) {
        return reject(error, "invalid request head fields");
    }
    decoded.headers.reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        WorkerRequestHeader header;
        if (!read_string16(&cursor, end, &header.name) ||
            !read_string32(&cursor, end, &header.value) ||
            header.name.empty()) {
            return reject(error, "invalid request header");
        }
        decoded.headers.push_back(header);
    }
    if (cursor != end) {
        return reject(error, "trailing request head payload");
    }
    *output = decoded;
    return true;
}

}  // namespace capsid
