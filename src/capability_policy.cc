#include "capability_policy.h"
#include "capability_manifest_hash.h"
#include "ipc_validation.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>

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

// --- Binding v1 policy set (§3.1-§3.3) ----------------------------------

namespace {

// FNV-1a, mirroring the Host policy compiler's rule-id hash
// (src/host/policy_compiler.cc). The label schemes here are the worker
// authority for Binding rules; the Host generation compiler must adopt
// the same labels when it starts producing binding rule ids.
uint32_t binding_rule_id(const std::string &label) {
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < label.size(); ++index) {
        hash ^= static_cast<uint8_t>(label[index]);
        hash *= 16777619u;
    }
    return hash == 0 ? 1 : hash;
}

const char *const kBindingModules[] = {
    "capsid:assert",        "capsid:getopts",       "capsid:hashing",
    "capsid:internal/core", "capsid:internal/path", "capsid:ipaddr",
    "capsid:path",          "capsid:readline",      "capsid:sqlite",
    "capsid:utils",         "capsid:uuid",
    "capsid:wasi",
};

const char *const kBindingProfiles[] = {
    "network-client", "filesystem-read", "filesystem-write",
    "filesystem-watch", "sqlite", "wasi",
};

bool contains(const char *const *names, size_t count,
              const std::string &value) {
    for (size_t index = 0; index < count; ++index) {
        if (value == names[index]) {
            return true;
        }
    }
    return false;
}

// Permanently forbidden Binding modules (§3.3): server, process and
// escape capabilities stay closed no matter what a Manifest asks for.
bool binding_module_forbidden(const std::string &name) {
    static const char *const forbidden[] = {
        "capsid:ffi",         "capsid:worker",
        "capsid:http-server", "capsid:process",
        "capsid:signals",     "capsid:internal/worker",
        "capsid:posix-socket",
    };
    return contains(forbidden,
                    sizeof(forbidden) / sizeof(forbidden[0]), name);
}

// Minimal SHA-256 (FIPS 180-4) so the worker-side digest has no OpenSSL
// link dependency (the Host verifies digests with OpenSSL independently).
void sha256_bytes(const uint8_t *data, size_t size, uint8_t out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    std::vector<uint8_t> message(data, data + size);
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    const uint64_t bit_length = static_cast<uint64_t>(size) * 8;
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(
            static_cast<uint8_t>((bit_length >> shift) & 0xff));
    }
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        uint32_t w[64];
        for (int index = 0; index < 16; ++index) {
            w[index] =
                (static_cast<uint32_t>(message[offset + index * 4]) << 24) |
                (static_cast<uint32_t>(message[offset + index * 4 + 1])
                 << 16) |
                (static_cast<uint32_t>(message[offset + index * 4 + 2])
                 << 8) |
                static_cast<uint32_t>(message[offset + index * 4 + 3]);
        }
        for (int index = 16; index < 64; ++index) {
            const uint32_t s0 =
                ((w[index - 15] >> 7) | (w[index - 15] << 25)) ^
                ((w[index - 15] >> 18) | (w[index - 15] << 14)) ^
                (w[index - 15] >> 3);
            const uint32_t s1 =
                ((w[index - 2] >> 17) | (w[index - 2] << 15)) ^
                ((w[index - 2] >> 19) | (w[index - 2] << 13)) ^
                (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int index = 0; index < 64; ++index) {
            const uint32_t S1 =
                ((e >> 6) | (e << 26)) ^
                ((e >> 11) | (e << 21)) ^
                ((e >> 25) | (e << 7));
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + S1 + ch + k[index] + w[index];
            const uint32_t S0 =
                ((a >> 2) | (a << 30)) ^
                ((a >> 13) | (a << 19)) ^
                ((a >> 22) | (a << 10));
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int index = 0; index < 8; ++index) {
        out[index * 4] = static_cast<uint8_t>(h[index] >> 24);
        out[index * 4 + 1] = static_cast<uint8_t>(h[index] >> 16);
        out[index * 4 + 2] = static_cast<uint8_t>(h[index] >> 8);
        out[index * 4 + 3] = static_cast<uint8_t>(h[index]);
    }
}

bool split_net_target(const std::string &net_target,
                      std::string *host,
                      uint16_t *port) {
    if (net_target.empty()) {
        return false;
    }
    if (net_target[0] == '[') {
        const size_t close = net_target.find(']');
        if (close == std::string::npos ||
            close + 1 >= net_target.size() ||
            net_target[close + 1] != ':') {
            return false;
        }
        *host = net_target.substr(1, close - 1);
        const std::string port_text = net_target.substr(close + 2);
        unsigned int value = 0;
        for (size_t index = 0; index < port_text.size(); ++index) {
            const char c = port_text[index];
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + static_cast<unsigned int>(c - '0');
        }
        if (value < 1 || value > 65535) {
            return false;
        }
        *port = static_cast<uint16_t>(value);
        return true;
    }
    const size_t colon = net_target.find(':');
    if (colon == std::string::npos ||
        net_target.find(':', colon + 1) != std::string::npos) {
        return false;
    }
    *host = net_target.substr(0, colon);
    const std::string port_text = net_target.substr(colon + 1);
    unsigned int value = 0;
    for (size_t index = 0; index < port_text.size(); ++index) {
        const char c = port_text[index];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned int>(c - '0');
    }
    if (value < 1 || value > 65535) {
        return false;
    }
    *port = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

bool handle_owner_matches(const NativeHandleOwner &owner,
                          const RuntimeOrigin &caller) {
    if (!owner.valid() || !caller.valid()) {
        return false;
    }
    if (owner.origin.domain != caller.domain) {
        return false;
    }
    return owner.origin.domain != RuntimeDomain::kBinding ||
           owner.origin.binding_id == caller.binding_id;
}

bool binding_module_known(const std::string &name) {
    return contains(kBindingModules,
                    sizeof(kBindingModules) / sizeof(kBindingModules[0]),
                    name);
}

bool binding_profile_known(const std::string &name) {
    return contains(kBindingProfiles,
                    sizeof(kBindingProfiles) /
                        sizeof(kBindingProfiles[0]),
                    name);
}

std::string compute_binding_profile_digest(
    const std::vector<WorkerBindingDescriptor> &bindings) {
    if (bindings.empty()) {
        return {};
    }
    std::set<std::string> profiles;
    for (size_t binding_index = 0;
         binding_index < bindings.size();
         ++binding_index) {
        const WorkerBindingDescriptor &descriptor =
            bindings[binding_index];
        for (size_t index = 0;
             index < descriptor.profiles.size();
             ++index) {
            profiles.insert(descriptor.profiles[index]);
        }
    }
    std::string canonical;
    for (std::set<std::string>::const_iterator profile =
             profiles.begin();
         profile != profiles.end();
         ++profile) {
        canonical += *profile;
        canonical += '\n';
    }
    uint8_t digest[32];
    sha256_bytes(
        reinterpret_cast<const uint8_t *>(canonical.data()),
        canonical.size(),
        digest);
    static const char kHex[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(71);
    for (unsigned int index = 0; index < 32; ++index) {
        result.push_back(kHex[digest[index] >> 4]);
        result.push_back(kHex[digest[index] & 0x0f]);
    }
    return result;
}

ModuleDecision BindingPolicy::module_decision(
    const std::string &name) const {
    for (size_t index = 0; index < modules.size(); ++index) {
        if (modules[index] == name) {
            return kModuleGranted;
        }
    }
    if (binding_module_forbidden(name)) {
        return kModuleForbidden;
    }
    return kModuleDenied;
}

BindingPolicySet::BindingPolicySet() {}

bool BindingPolicySet::configure(
    const std::vector<WorkerBindingDescriptor> &bindings,
    std::string *error) {
    if (error) {
        error->clear();
    }
    std::vector<BindingPolicy> compiled;
    compiled.reserve(bindings.size());
    std::set<std::string> seen_ids;
    for (size_t binding_index = 0;
         binding_index < bindings.size();
         ++binding_index) {
        const WorkerBindingDescriptor &descriptor =
            bindings[binding_index];
        if (!seen_ids.insert(descriptor.name).second) {
            if (error) {
                *error = "duplicate binding id";
            }
            return false;
        }
        for (size_t index = 0;
             index < descriptor.profiles.size();
             ++index) {
            if (!binding_profile_known(descriptor.profiles[index])) {
                if (error) {
                    *error = "unknown binding sandbox profile";
                }
                return false;
            }
        }
        for (size_t index = 0;
             index < descriptor.modules.size();
             ++index) {
            if (!binding_module_known(descriptor.modules[index])) {
                if (error) {
                    *error = "binding module is not grantable";
                }
                return false;
            }
        }
        // §4.1: a non-empty resource permission requires its sandbox
        // profile. Empty allow lists grant nothing and need no profile.
        {
            const bool has_network_client =
                std::find(descriptor.profiles.begin(),
                          descriptor.profiles.end(),
                          "network-client") !=
                descriptor.profiles.end();
            if (!descriptor.net_rules.empty() &&
                !has_network_client) {
                if (error) {
                    *error =
                        "net rules require the network-client sandbox "
                        "profile";
                }
                return false;
            }
            const bool has_filesystem_read =
                std::find(descriptor.profiles.begin(),
                          descriptor.profiles.end(),
                          "filesystem-read") !=
                descriptor.profiles.end();
            if (!descriptor.fs_read.empty() &&
                !has_filesystem_read) {
                if (error) {
                    *error =
                        "fs read requires the filesystem-read sandbox "
                        "profile";
                }
                return false;
            }
            const bool has_filesystem_write =
                std::find(descriptor.profiles.begin(),
                          descriptor.profiles.end(),
                          "filesystem-write") !=
                descriptor.profiles.end();
            if (!descriptor.fs_write.empty() &&
                !has_filesystem_write) {
                if (error) {
                    *error =
                        "fs write requires the filesystem-write sandbox "
                        "profile";
                }
                return false;
            }
            const bool has_sqlite_module =
                std::find(descriptor.modules.begin(),
                          descriptor.modules.end(),
                          "capsid:sqlite") != descriptor.modules.end();
            const bool has_sqlite_profile =
                std::find(descriptor.profiles.begin(),
                          descriptor.profiles.end(),
                          "sqlite") != descriptor.profiles.end();
            if (has_sqlite_module && !has_sqlite_profile) {
                if (error) {
                    *error =
                        "capsid:sqlite requires the sqlite sandbox profile";
                }
                return false;
            }
            const bool has_wasi_module =
                std::find(descriptor.modules.begin(),
                          descriptor.modules.end(),
                          "capsid:wasi") != descriptor.modules.end();
            const bool has_wasi_profile =
                std::find(descriptor.profiles.begin(),
                          descriptor.profiles.end(),
                          "wasi") != descriptor.profiles.end();
            if (has_wasi_module && !has_wasi_profile) {
                if (error) {
                    *error =
                        "capsid:wasi requires the wasi sandbox profile";
                }
                return false;
            }
        }

        BindingPolicy policy;
        policy.binding_id = descriptor.name;
        policy.modules = descriptor.modules;
        policy.profiles = descriptor.profiles;
        policy.env = descriptor.env;
        policy.stdio = descriptor.stdio;

        std::vector<std::string> resources;
        std::vector<capsid_permission_rule> rules;
        resources.reserve(descriptor.fs_read.size() +
                          descriptor.fs_write.size());
        rules.reserve(descriptor.fs_read.size() +
                      descriptor.fs_write.size());
        for (size_t index = 0;
             index < descriptor.fs_read.size();
             ++index) {
            resources.push_back(descriptor.fs_read[index]);
            capsid_permission_rule rule;
            capsid_permission_rule_init(&rule);
            rule.permission = CAPSID_PERMISSION_READ;
            rule.action = CAPSID_PERMISSION_ALLOW;
            rule.resource = resources.back().c_str();
            rule.rule_id = binding_rule_id(
                "binding-fs-read:" + resources.back());
            rules.push_back(rule);
        }
        for (size_t index = 0;
             index < descriptor.fs_write.size();
             ++index) {
            resources.push_back(descriptor.fs_write[index]);
            capsid_permission_rule rule;
            capsid_permission_rule_init(&rule);
            rule.permission = CAPSID_PERMISSION_WRITE;
            rule.action = CAPSID_PERMISSION_ALLOW;
            rule.resource = resources.back().c_str();
            rule.rule_id = binding_rule_id(
                "binding-fs-write:" + resources.back());
            rules.push_back(rule);
        }
        capsid_capability_policy capability_descriptor;
        capsid_capability_policy_init(&capability_descriptor);
        capability_descriptor.application_identity =
            descriptor.name.c_str();
        capability_descriptor.rules =
            rules.empty() ? NULL : &rules[0];
        capability_descriptor.rule_count =
            static_cast<uint32_t>(rules.size());
        if (!policy.capability.configure(
                &capability_descriptor, error)) {
            if (error) {
                *error = "invalid binding permission rules";
            }
            return false;
        }

        if (!descriptor.net_rules.empty()) {
            std::vector<std::string> targets;
            std::vector<capsid_egress_rule> egress_rules;
            targets.reserve(descriptor.net_rules.size());
            egress_rules.reserve(descriptor.net_rules.size());
            for (size_t index = 0;
                 index < descriptor.net_rules.size();
                 ++index) {
                std::string host;
                uint16_t port = 0;
                if (!split_net_target(
                        descriptor.net_rules[index], &host, &port)) {
                    if (error) {
                        *error = "invalid binding net target";
                    }
                    return false;
                }
                targets.push_back(host);
                capsid_egress_rule rule;
                capsid_egress_rule_init(&rule);
                rule.action = CAPSID_EGRESS_ALLOW;
                rule.target = targets.back().c_str();
                rule.port_start = port;
                rule.port_end = port;
                rule.rule_id = binding_rule_id(
                    "binding-net:" + descriptor.net_rules[index]);
                egress_rules.push_back(rule);
            }
            capsid_egress_policy egress_descriptor;
            capsid_egress_policy_init(&egress_descriptor);
            egress_descriptor.default_action = CAPSID_EGRESS_DENY;
            egress_descriptor.rules =
                egress_rules.empty() ? NULL : &egress_rules[0];
            egress_descriptor.rule_count =
                static_cast<uint32_t>(egress_rules.size());
            if (!policy.egress.configure(
                    &egress_descriptor, error)) {
                if (error) {
                    *error = "invalid binding net policy";
                }
                return false;
            }
            policy.has_net_policy = true;
        }

        compiled.push_back(policy);
    }
    policies_.swap(compiled);
    return true;
}

bool BindingPolicySet::has(const std::string &id) const {
    return policy(id) != NULL;
}

const BindingPolicy *BindingPolicySet::policy(
    const std::string &id) const {
    for (size_t index = 0; index < policies_.size(); ++index) {
        if (policies_[index].binding_id == id) {
            return &policies_[index];
        }
    }
    return NULL;
}

PermissionDecision BindingPolicySet::evaluate(
    const std::string &id,
    capsid_permission_name permission,
    const std::string &resource) const {
    const BindingPolicy *found = policy(id);
    if (found == NULL) {
        return PermissionDecision();
    }
    return found->capability.evaluate(permission, resource);
}

std::vector<std::string> BindingPolicySet::ids() const {
    std::vector<std::string> result;
    result.reserve(policies_.size());
    for (size_t index = 0; index < policies_.size(); ++index) {
        result.push_back(policies_[index].binding_id);
    }
    return result;
}

}  // namespace capsid
