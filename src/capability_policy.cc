#include "capability_policy.h"
#include "capability_manifest_hash.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <set>

extern "C" {

void capsid_permission_rule_init(capsid_permission_rule *rule) {
    if (!rule) {
        return;
    }
    std::memset(rule, 0, sizeof(*rule));
    rule->struct_size = sizeof(*rule);
    rule->action = CAPSID_PERMISSION_DENY;
    rule->permission = CAPSID_PERMISSION_NONE;
}

void capsid_env_entry_init(capsid_env_entry *entry) {
    if (!entry) {
        return;
    }
    std::memset(entry, 0, sizeof(*entry));
    entry->struct_size = sizeof(*entry);
}

void capsid_capability_policy_init(capsid_capability_policy *policy) {
    if (!policy) {
        return;
    }
    std::memset(policy, 0, sizeof(*policy));
    policy->struct_size = sizeof(*policy);
    policy->version = CAPSID_CAPABILITY_POLICY_VERSION;
}

}

namespace {

void set_error(std::string *error, const char *message) {
    if (error) {
        *error = message;
    }
}

bool starts_with(const std::string &value, const char *prefix) {
    const size_t size = std::strlen(prefix);
    return value.size() >= size &&
           value.compare(0, size, prefix) == 0;
}

bool contains_string(const char *const *values,
                     size_t count,
                     const std::string &wanted) {
    for (size_t index = 0; index < count; ++index) {
        if (wanted == values[index]) {
            return true;
        }
    }
    return false;
}

const char *const kAvailableModules[] = {
    "capsid:assert",
    "capsid:env",
    "capsid:fs",
    "capsid:getopts",
    "capsid:hashing",
    "capsid:ipaddr",
    "capsid:permissions",
    "capsid:stdio",
    "capsid:storage",
    "capsid:system",
    "capsid:utils",
    "capsid:uuid"
};

const char *const kOptionalCapsidModules[] = {
    "capsid:ffi",
    "capsid:net",
    "capsid:path",
    "capsid:raw-socket",
    "capsid:readline",
    "capsid:sqlite",
    "capsid:websocket"
};

const char *const kForbiddenCapsidModules[] = {
    "capsid:process",
    "capsid:worker",
    "capsid:http-server",
    "capsid:ipc",
    "capsid:wasi"
};

bool module_forbidden(const std::string &name) {
    if (name.empty() ||
        name == "tjs:internal" ||
        starts_with(name, "tjs:internal/") ||
        name == "capsid:internal" ||
        starts_with(name, "capsid:internal/") ||
        starts_with(name, "http:") ||
        starts_with(name, "https:") ||
        starts_with(name, "file:") ||
        starts_with(name, "data:") ||
        starts_with(name, "node:") ||
        starts_with(name, "/") ||
        starts_with(name, "./") ||
        starts_with(name, "../") ||
        contains_string(
            kForbiddenCapsidModules,
            sizeof(kForbiddenCapsidModules) /
                sizeof(kForbiddenCapsidModules[0]),
            name)) {
        return true;
    }
    return starts_with(name, "tjs:");
}

bool module_known(const std::string &name) {
    if (contains_string(
            kAvailableModules,
            sizeof(kAvailableModules) /
                sizeof(kAvailableModules[0]),
            name) ||
        contains_string(
            kOptionalCapsidModules,
            sizeof(kOptionalCapsidModules) /
                sizeof(kOptionalCapsidModules[0]),
            name)) {
        return true;
    }
    return false;
}

bool module_available(const std::string &name) {
    return contains_string(
        kAvailableModules,
        sizeof(kAvailableModules) / sizeof(kAvailableModules[0]),
        name);
}

bool valid_identity(const std::string &identity) {
    if (identity.size() > 128) {
        return false;
    }
    for (size_t index = 0; index < identity.size(); ++index) {
        const unsigned char ch =
            static_cast<unsigned char>(identity[index]);
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool normalize_path(const std::string &input,
                    std::string *normalized) {
    if (!normalized || input.empty() || input.size() > 4096) {
        return false;
    }
    std::string root = "/";
    std::size_t start = 1;
    bool windows_drive = false;
#if defined(_WIN32)
    if (input.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(input[0])) &&
        input[1] == ':' && (input[2] == '/' || input[2] == '\\')) {
        root.clear();
        root.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(input[0]))));
        root += ":/";
        start = 3;
        windows_drive = true;
    }
#endif
    if (!windows_drive && input[0] != '/') {
        return false;
    }
    std::vector<std::string> components;
    while (start <= input.size()) {
        const std::size_t end = windows_drive
            ? input.find_first_of("/\\", start)
            : input.find('/', start);
        const std::size_t component_end =
            end == std::string::npos ? input.size() : end;
        const std::string component =
            input.substr(start, component_end - start);
        if (component.empty() || component == ".") {
            /* Repeated and trailing separators normalize away. */
        } else if (component == "..") {
            if (components.empty()) {
                return false;
            }
            components.pop_back();
        } else {
            components.push_back(component);
        }
        if (component_end == input.size()) {
            break;
        }
        start = component_end + 1;
    }
    *normalized = root;
    for (size_t index = 0; index < components.size(); ++index) {
        if (!normalized->empty() && normalized->back() != '/') {
            normalized->push_back('/');
        }
        normalized->append(components[index]);
    }
    if (normalized->size() > root.size() &&
        normalized->back() == '/') {
        normalized->pop_back();
    }
    return true;
}

bool valid_env_pattern(const std::string &resource) {
    if (resource.empty() || resource.size() > 256) {
        return false;
    }
    size_t limit = resource.size();
    if (resource[limit - 1] == '*') {
        --limit;
    }
    if (limit == 0) {
        return resource == "*";
    }
    if (!(std::isalpha(
              static_cast<unsigned char>(resource[0])) ||
          resource[0] == '_')) {
        return false;
    }
    for (size_t index = 0; index < limit; ++index) {
        const unsigned char ch =
            static_cast<unsigned char>(resource[index]);
        if (!(std::isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return resource.find('*') == std::string::npos ||
           resource.find('*') == resource.size() - 1;
}

bool valid_env_name(const std::string &name) {
    return name.find('*') == std::string::npos &&
           valid_env_pattern(name);
}

bool valid_storage_namespace(const std::string &value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    for (size_t index = 0; index < value.size(); ++index) {
        const unsigned char ch =
            static_cast<unsigned char>(value[index]);
        if (!(std::isalnum(ch) || ch == '_' ||
              ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

bool fixed_value(const std::string &resource,
                 const char *const *values,
                 size_t count) {
    return contains_string(values, count, resource);
}

bool normalize_rule_resource(capsid_permission_name permission,
                             const char *input,
                             std::string *resource) {
    if (!resource) {
        return false;
    }
    resource->clear();
    if (permission == CAPSID_PERMISSION_RAW_SOCKET) {
        return !input || input[0] == '\0';
    }
    if (!input) {
        return false;
    }
    const std::string value(input);
    if (value.empty() || value.size() > 4096) {
        return false;
    }
    switch (permission) {
        case CAPSID_PERMISSION_READ:
        case CAPSID_PERMISSION_WRITE:
        case CAPSID_PERMISSION_FFI: {
            std::string normalized;
            if (!normalize_path(value, &normalized) ||
                normalized != value) {
                return false;
            }
            *resource = normalized;
            return true;
        }
        case CAPSID_PERMISSION_ENV:
            if (!valid_env_pattern(value)) {
                return false;
            }
            *resource = value;
            return true;
        case CAPSID_PERMISSION_SYS: {
            static const char *const values[] = {
                "cpus",
                "availableParallelism",
                "loadAvg",
                "uptime",
                "runtimeVersion",
                "featureFlags",
                "memoryUsage",
                "hostname",
                "networkInterfaces",
                "userInfo",
                "homedir",
                "osRelease",
                "systemMemoryInfo",
                "uid",
                "gid",
                "username"
            };
            if (!fixed_value(
                    value, values, sizeof(values) / sizeof(values[0]))) {
                return false;
            }
            *resource = value;
            return true;
        }
        case CAPSID_PERMISSION_STDIO: {
            static const char *const values[] = {
                "stdin", "stdout", "stderr"
            };
            if (!fixed_value(
                    value, values, sizeof(values) / sizeof(values[0]))) {
                return false;
            }
            *resource = value;
            return true;
        }
        case CAPSID_PERMISSION_STORAGE:
            if (!valid_storage_namespace(value)) {
                return false;
            }
            *resource = value;
            return true;
        case CAPSID_PERMISSION_ENGINE: {
            static const char *const values[] = {
                "gc", "snapshot", "profiler"
            };
            if (!fixed_value(
                    value, values, sizeof(values) / sizeof(values[0]))) {
                return false;
            }
            *resource = value;
            return true;
        }
        case CAPSID_PERMISSION_NONE:
        case CAPSID_PERMISSION_NET:
        case CAPSID_PERMISSION_RAW_SOCKET:
            return false;
    }
    return false;
}

bool normalize_query_resource(capsid_permission_name permission,
                              const std::string &input,
                              std::string *resource) {
    if (permission == CAPSID_PERMISSION_RAW_SOCKET) {
        if (!input.empty()) {
            return false;
        }
        resource->clear();
        return true;
    }
    if (permission == CAPSID_PERMISSION_READ ||
        permission == CAPSID_PERMISSION_WRITE ||
        permission == CAPSID_PERMISSION_FFI) {
        return normalize_path(input, resource);
    }
    if (permission == CAPSID_PERMISSION_ENV) {
        if (!valid_env_name(input)) {
            return false;
        }
        *resource = input;
        return true;
    }
    if (permission == CAPSID_PERMISSION_STORAGE) {
        if (!valid_storage_namespace(input)) {
            return false;
        }
        *resource = input;
        return true;
    }
    if (input.empty() || input.size() > 4096) {
        return false;
    }
    *resource = input;
    return true;
}

bool path_matches(const std::string &resource,
                  const std::string &scope) {
    return scope == "/" || resource == scope ||
           (resource.size() > scope.size() &&
            resource[scope.size()] == '/' &&
            resource.compare(0, scope.size(), scope) == 0);
}

bool rule_matches(const capsid::CapabilityPolicy::Rule &rule,
                  const std::string &resource) {
    switch (rule.permission) {
        case CAPSID_PERMISSION_READ:
        case CAPSID_PERMISSION_WRITE:
        case CAPSID_PERMISSION_FFI:
            return path_matches(resource, rule.resource);
        case CAPSID_PERMISSION_ENV:
            return rule.resource == "*" ||
                   (rule.resource[rule.resource.size() - 1] == '*'
                        ? resource.compare(
                              0,
                              rule.resource.size() - 1,
                              rule.resource,
                              0,
                              rule.resource.size() - 1) == 0
                        : resource == rule.resource);
        case CAPSID_PERMISSION_RAW_SOCKET:
            return true;
        case CAPSID_PERMISSION_SYS:
        case CAPSID_PERMISSION_STDIO:
        case CAPSID_PERMISSION_STORAGE:
        case CAPSID_PERMISSION_ENGINE:
            return resource == rule.resource;
        case CAPSID_PERMISSION_NONE:
        case CAPSID_PERMISSION_NET:
            return false;
    }
    return false;
}

bool generic_permission(capsid_permission_name permission) {
    return permission >= CAPSID_PERMISSION_READ &&
           permission <= CAPSID_PERMISSION_ENGINE &&
           permission != CAPSID_PERMISSION_NET;
}

uint32_t permission_action_value(
    const capsid_permission_action &action) {
    static_assert(
        sizeof(capsid_permission_action) == sizeof(uint32_t),
        "public permission action ABI must be 32-bit");
    uint32_t value = 0;
    std::memcpy(&value, &action, sizeof(value));
    return value;
}

uint32_t permission_name_value(
    const capsid_permission_name &permission) {
    static_assert(
        sizeof(capsid_permission_name) == sizeof(uint32_t),
        "public permission name ABI must be 32-bit");
    uint32_t value = 0;
    std::memcpy(&value, &permission, sizeof(value));
    return value;
}

}  // namespace

namespace capsid {

CapabilityPolicy::CapabilityPolicy()
    : enabled_(false),
      version_(0),
      has_net_policy_(false) {}

bool CapabilityPolicy::configure(
    const capsid_capability_policy *policy,
    std::string *error) {
    if (error) {
        error->clear();
    }
    CapabilityPolicy compiled;
    if (!policy) {
        swap(compiled);
        return true;
    }
    const bool version_1 =
        policy->version == CAPSID_CAPABILITY_POLICY_VERSION_1 &&
        policy->struct_size ==
            offsetof(capsid_capability_policy, env_entries);
    const bool version_2 =
        policy->version == CAPSID_CAPABILITY_POLICY_VERSION &&
        policy->struct_size == sizeof(capsid_capability_policy);
    if ((!version_1 && !version_2) ||
        policy->reserved != 0 ||
        (version_2 && policy->env_reserved != 0) ||
        policy->allowed_module_count > 64 ||
        (policy->allowed_module_count != 0 &&
         !policy->allowed_modules) ||
        policy->rule_count > 256 ||
        (policy->rule_count != 0 && !policy->rules) ||
        (version_2 && policy->env_entry_count > 256) ||
        (version_2 && policy->env_entry_count != 0 &&
         !policy->env_entries)) {
        set_error(error, "invalid capability policy descriptor");
        return false;
    }
    compiled.enabled_ = true;
    compiled.version_ = policy->version;
    if (policy->application_identity) {
        compiled.application_identity_ =
            policy->application_identity;
    }
    if (!valid_identity(compiled.application_identity_)) {
        set_error(error, "invalid application identity");
        return false;
    }

    std::set<std::string> module_names;
    for (uint32_t index = 0;
         index < policy->allowed_module_count;
         ++index) {
        if (!policy->allowed_modules[index]) {
            set_error(error, "NULL allowed module");
            return false;
        }
        const std::string name(policy->allowed_modules[index]);
        if (name.size() > 128 || module_forbidden(name) ||
            !module_known(name) ||
            !module_names.insert(name).second) {
            set_error(error, "invalid or duplicate allowed module");
            return false;
        }
        compiled.allowed_modules_.push_back(name);
    }

    std::set<uint32_t> rule_ids;
    for (uint32_t index = 0; index < policy->rule_count; ++index) {
        const capsid_permission_rule &source = policy->rules[index];
        const uint32_t action_value =
            permission_action_value(source.action);
        const uint32_t permission_value =
            permission_name_value(source.permission);
        if (source.struct_size != sizeof(capsid_permission_rule) ||
            action_value > CAPSID_PERMISSION_ALLOW ||
            permission_value < CAPSID_PERMISSION_READ ||
            permission_value > CAPSID_PERMISSION_ENGINE ||
            permission_value == CAPSID_PERMISSION_NET ||
            source.rule_id == 0 || source.reserved != 0 ||
            !rule_ids.insert(source.rule_id).second) {
            set_error(error, "invalid permission rule descriptor");
            return false;
        }
        Rule rule;
        rule.action =
            static_cast<capsid_permission_action>(action_value);
        rule.permission =
            static_cast<capsid_permission_name>(permission_value);
        rule.rule_id = source.rule_id;
        if (!normalize_rule_resource(
                rule.permission,
                source.resource,
                &rule.resource)) {
            set_error(error, "invalid permission rule resource");
            return false;
        }
        compiled.rules_.push_back(rule);
    }

    if (policy->net_policy) {
        std::string net_error;
        if (!compiled.net_policy_.configure(
                policy->net_policy, &net_error)) {
            set_error(error, "invalid capability net policy");
            return false;
        }
        compiled.has_net_policy_ = true;
    }

    const uint32_t env_entry_count =
        version_2 ? policy->env_entry_count : 0;
    if (env_entry_count != 0 &&
        compiled.module_decision("capsid:env") !=
            kModuleGranted) {
        set_error(
            error,
            "environment entries require an authorized capsid:env module");
        return false;
    }
    std::set<std::string> env_names;
    size_t env_payload_size = 0;
    for (uint32_t index = 0;
         index < env_entry_count;
         ++index) {
        const capsid_env_entry &source =
            policy->env_entries[index];
        if (source.struct_size != sizeof(capsid_env_entry) ||
            source.reserved != 0 || !source.name ||
            !source.value) {
            set_error(
                error,
                "invalid environment entry descriptor");
            return false;
        }
        const std::string name(source.name);
        const std::string value(source.value);
        if (!valid_env_name(name) ||
            value.size() > 16384 ||
            !env_names.insert(name).second ||
            compiled.evaluate(
                CAPSID_PERMISSION_ENV,
                name).state !=
                CAPSID_PERMISSION_STATE_GRANTED) {
            set_error(
                error,
                "invalid, duplicate, or unauthorized environment entry");
            return false;
        }
        env_payload_size += name.size() + value.size();
        if (env_payload_size > 48u * 1024u) {
            set_error(error, "environment snapshot is too large");
            return false;
        }
        compiled.env_entries_.push_back(
            std::make_pair(name, value));
    }
    swap(compiled);
    return true;
}

ModuleDecision CapabilityPolicy::module_decision(
    const std::string &name) const {
    if (module_forbidden(name)) {
        return kModuleForbidden;
    }
    const bool allowed =
        std::find(
            allowed_modules_.begin(),
            allowed_modules_.end(),
            name) != allowed_modules_.end();
    if (!allowed) {
        return module_known(name) ? kModuleDenied
                                  : kModuleUnavailable;
    }
    return module_available(name) ? kModuleGranted
                                  : kModuleUnavailable;
}

PermissionDecision CapabilityPolicy::evaluate(
    capsid_permission_name permission,
    const std::string &input) const {
    if (!enabled_ || !generic_permission(permission)) {
        return PermissionDecision();
    }
    std::string resource;
    if (!normalize_query_resource(permission, input, &resource)) {
        return PermissionDecision();
    }
    const Rule *allow = NULL;
    const Rule *deny = NULL;
    for (std::vector<Rule>::const_iterator it = rules_.begin();
         it != rules_.end();
         ++it) {
        if (it->permission != permission ||
            !rule_matches(*it, resource)) {
            continue;
        }
        if (it->action == CAPSID_PERMISSION_DENY) {
            if (!deny || it->resource.size() > deny->resource.size()) {
                deny = &*it;
            }
        } else if (!allow ||
                   it->resource.size() > allow->resource.size()) {
            allow = &*it;
        }
    }
    if (deny) {
        return PermissionDecision(
            CAPSID_PERMISSION_STATE_DENIED,
            deny->rule_id,
            resource);
    }
    if (allow) {
        return PermissionDecision(
            CAPSID_PERMISSION_STATE_GRANTED,
            allow->rule_id,
            resource);
    }
    return PermissionDecision(
        CAPSID_PERMISSION_STATE_DENIED, 0, resource);
}

PermissionDecision CapabilityPolicy::query(
    capsid_permission_name permission,
    const std::string &resource,
    uint16_t port) const {
    if (permission == CAPSID_PERMISSION_NET) {
        if (!enabled_ || !has_net_policy_) {
            return PermissionDecision();
        }
        if (resource.empty() || port == 0) {
            return PermissionDecision(
                net_policy_.query_state(), 0, resource);
        }
        const EgressDecision decision =
            net_policy_.decide_host(resource, port);
        return PermissionDecision(
            decision.allowed ? CAPSID_PERMISSION_STATE_GRANTED
                             : CAPSID_PERMISSION_STATE_DENIED,
            decision.rule_id,
            resource);
    }
    if (permission == CAPSID_PERMISSION_ENV ||
        permission == CAPSID_PERMISSION_STORAGE) {
        return evaluate(permission, resource);
    }
    if (permission == CAPSID_PERMISSION_STDIO) {
        if (resource == "stdout" ||
            resource == "stderr") {
            return evaluate(permission, resource);
        }
        return PermissionDecision(
            CAPSID_PERMISSION_STATE_UNAVAILABLE,
            0,
            resource);
    }
    if (permission == CAPSID_PERMISSION_READ) {
        return evaluate(permission, resource);
    }
    if (permission == CAPSID_PERMISSION_SYS &&
        (resource == "runtimeVersion" ||
         resource == "featureFlags")) {
        return evaluate(permission, resource);
    }
    /*
     * The policy engine understands the remaining future capability scopes,
     * but the restricted build exposes no corresponding operation. Build
     * availability therefore wins over a configured rule.
     */
    return PermissionDecision(
        CAPSID_PERMISSION_STATE_UNAVAILABLE, 0, resource);
}

bool CapabilityPolicy::env_value(
    const std::string &name,
    std::string *value) const {
    for (std::vector<
             std::pair<std::string, std::string> >::const_iterator
             it = env_entries_.begin();
         it != env_entries_.end();
         ++it) {
        if (it->first == name) {
            if (value) {
                *value = it->second;
            }
            return true;
        }
    }
    return false;
}

void CapabilityPolicy::swap(CapabilityPolicy &other) {
    std::swap(enabled_, other.enabled_);
    std::swap(version_, other.version_);
    application_identity_.swap(other.application_identity_);
    allowed_modules_.swap(other.allowed_modules_);
    rules_.swap(other.rules_);
    env_entries_.swap(other.env_entries_);
    std::swap(has_net_policy_, other.has_net_policy_);
    std::swap(net_policy_, other.net_policy_);
}

const char *permission_name(capsid_permission_name permission) {
    switch (permission) {
        case CAPSID_PERMISSION_READ:
            return "read";
        case CAPSID_PERMISSION_WRITE:
            return "write";
        case CAPSID_PERMISSION_NET:
            return "net";
        case CAPSID_PERMISSION_ENV:
            return "env";
        case CAPSID_PERMISSION_SYS:
            return "sys";
        case CAPSID_PERMISSION_FFI:
            return "ffi";
        case CAPSID_PERMISSION_RAW_SOCKET:
            return "rawSocket";
        case CAPSID_PERMISSION_STDIO:
            return "stdio";
        case CAPSID_PERMISSION_STORAGE:
            return "storage";
        case CAPSID_PERMISSION_ENGINE:
            return "engine";
        case CAPSID_PERMISSION_NONE:
            return "none";
    }
    return "unknown";
}

const char *permission_resource_kind(
    capsid_permission_name permission) {
    switch (permission) {
        case CAPSID_PERMISSION_READ:
        case CAPSID_PERMISSION_WRITE:
        case CAPSID_PERMISSION_FFI:
            return "path";
        case CAPSID_PERMISSION_NET:
            return "host";
        case CAPSID_PERMISSION_ENV:
            return "variable";
        case CAPSID_PERMISSION_SYS:
            return "kind";
        case CAPSID_PERMISSION_RAW_SOCKET:
            return "boolean";
        case CAPSID_PERMISSION_STDIO:
            return "stream";
        case CAPSID_PERMISSION_STORAGE:
            return "namespace";
        case CAPSID_PERMISSION_ENGINE:
            return "operation";
        case CAPSID_PERMISSION_NONE:
            return "none";
    }
    return "unknown";
}

const char *permission_state_name(capsid_permission_state state) {
    switch (state) {
        case CAPSID_PERMISSION_STATE_DENIED:
            return "denied";
        case CAPSID_PERMISSION_STATE_GRANTED:
            return "granted";
        case CAPSID_PERMISSION_STATE_PARTIAL:
            return "partial";
        case CAPSID_PERMISSION_STATE_UNAVAILABLE:
            return "unavailable";
    }
    return "denied";
}

const char *capability_manifest_hash() {
    return CAPSID_CAPABILITY_MANIFEST_SHA256;
}

}  // namespace capsid
