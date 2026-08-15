#include "capability_policy.h"
#include "capsid/runtime.h"
#include "ipc_validation.h"

#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "test-capability-policy: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

bool manifest_array_contains(const std::string &document,
                             const char *array_name,
                             const char *value) {
    const std::string marker =
        std::string("\"") + array_name + "\"";
    const size_t marker_at = document.find(marker);
    const size_t begin =
        marker_at == std::string::npos
            ? std::string::npos
            : document.find('[', marker_at + marker.size());
    const size_t end =
        begin == std::string::npos
            ? std::string::npos
            : document.find(']', begin + 1);
    const std::string wanted =
        std::string("\"") + value + "\"";
    const size_t found =
        begin == std::string::npos
            ? std::string::npos
            : document.find(wanted, begin + 1);
    return end != std::string::npos &&
           found != std::string::npos && found < end;
}

capsid_permission_rule rule(capsid_permission_name permission,
                          capsid_permission_action action,
                          const char *resource,
                          uint32_t id) {
    capsid_permission_rule value;
    capsid_permission_rule_init(&value);
    value.permission = permission;
    value.action = action;
    value.resource = resource;
    value.rule_id = id;
    return value;
}

capsid_env_entry env_entry(const char *name,
                         const char *value) {
    capsid_env_entry entry;
    capsid_env_entry_init(&entry);
    entry.name = name;
    entry.value = value;
    return entry;
}

capsid_egress_rule net_rule(capsid_egress_action action,
                          const char *target,
                          uint16_t port,
                          uint32_t id) {
    capsid_egress_rule value;
    capsid_egress_rule_init(&value);
    value.action = action;
    value.target = target;
    value.port_start = port;
    value.port_end = port;
    value.rule_id = id;
    return value;
}

bool configure(capsid::CapabilityPolicy *compiled,
               const std::vector<const char *> &modules,
               const std::vector<capsid_permission_rule> &rules,
               const capsid_egress_policy *net_policy,
               std::string *error) {
    capsid_capability_policy policy;
    capsid_capability_policy_init(&policy);
    policy.application_identity = "capability-test";
    policy.allowed_modules = modules.empty() ? NULL : &modules[0];
    policy.allowed_module_count =
        static_cast<uint32_t>(modules.size());
    policy.rules = rules.empty() ? NULL : &rules[0];
    policy.rule_count = static_cast<uint32_t>(rules.size());
    policy.net_policy = net_policy;
    return compiled->configure(&policy, error);
}

void test_initializers_and_default_deny() {
    capsid_permission_rule permission;
    capsid_permission_rule_init(&permission);
    require(permission.struct_size == sizeof(permission),
            "permission rule struct_size was not initialized");
    require(permission.action == CAPSID_PERMISSION_DENY,
            "permission rule did not default to deny");
    require(permission.permission == CAPSID_PERMISSION_NONE,
            "permission rule did not default to NONE");
    require(permission.resource == NULL && permission.rule_id == 0 &&
                permission.reserved == 0,
            "permission rule reserved state was not zeroed");

    capsid_env_entry environment;
    capsid_env_entry_init(&environment);
    require(environment.struct_size == sizeof(environment) &&
                environment.name == NULL &&
                environment.value == NULL &&
                environment.reserved == 0,
            "environment entry did not initialize to an empty descriptor");

    capsid_capability_policy descriptor;
    capsid_capability_policy_init(&descriptor);
    require(descriptor.struct_size == sizeof(descriptor),
            "capability policy struct_size was not initialized");
    require(descriptor.version == CAPSID_CAPABILITY_POLICY_VERSION,
            "capability policy version was not initialized");
    require(descriptor.application_identity == NULL &&
                descriptor.allowed_modules == NULL &&
                descriptor.allowed_module_count == 0 &&
                descriptor.rules == NULL &&
                descriptor.rule_count == 0 &&
                descriptor.net_policy == NULL &&
                descriptor.reserved == 0 &&
                descriptor.env_entries == NULL &&
                descriptor.env_entry_count == 0 &&
                descriptor.env_reserved == 0,
            "capability policy did not default to deny-all");

    capsid::CapabilityPolicy compiled;
    std::string error;
    require(compiled.configure(NULL, &error), error);
    require(!compiled.enabled(), "NULL policy unexpectedly enabled");
    require(compiled.module_decision("capsid:permissions") ==
                capsid::kModuleDenied,
            "default policy exposed capsid:permissions");
    const capsid::PermissionDecision net =
        compiled.query(CAPSID_PERMISSION_NET, "example.com", 443);
    require(net.state == CAPSID_PERMISSION_STATE_DENIED &&
                net.rule_id == 0,
            "default policy granted net");

    const std::string manifest_hash(
        capsid::capability_manifest_hash());
    require(manifest_hash.size() == 64,
            "capability manifest hash is not SHA-256 sized");
    require(
        manifest_hash.find_first_not_of("0123456789abcdef") ==
            std::string::npos,
        "capability manifest hash is not lowercase hexadecimal");
}

void test_module_build_and_visibility_gates() {
    capsid::CapabilityPolicy compiled;
    std::string error;
    std::vector<const char *> modules;
    modules.push_back("capsid:permissions");
    modules.push_back("capsid:fs");
    modules.push_back("capsid:storage");
    modules.push_back("capsid:stdio");
    require(configure(
                &compiled,
                modules,
                std::vector<capsid_permission_rule>(),
                NULL,
                &error),
            error);
    require(compiled.module_decision("capsid:permissions") ==
                capsid::kModuleGranted,
            "compiled and allowed module was not granted");
    require(compiled.module_decision("capsid:fs") ==
                capsid::kModuleGranted,
            "built fs module was not granted");
    require(compiled.module_decision("capsid:storage") ==
                capsid::kModuleGranted,
            "built storage module was not granted");
    require(compiled.module_decision("capsid:stdio") ==
                capsid::kModuleGranted,
            "built stdio module was not granted");
    require(compiled.module_decision("capsid:env") ==
                capsid::kModuleDenied,
            "known but unlisted module did not report denied");
    require(compiled.module_decision("tjs:internal/core") ==
                capsid::kModuleForbidden,
            "internal module did not report forbidden");
    require(compiled.module_decision("https://example.com/app.js") ==
                capsid::kModuleForbidden,
            "remote module did not report forbidden");
    require(compiled.module_decision("not:a-known-module") ==
                capsid::kModuleUnavailable,
            "unknown import did not fail closed as unavailable");

    modules.clear();
    modules.push_back("not:a-known-module");
    require(!configure(
                &compiled,
                modules,
                std::vector<capsid_permission_rule>(),
                NULL,
                &error),
            "unknown allowed module was accepted");
    modules[0] = "tjs:internal/core";
    require(!configure(
                &compiled,
                modules,
                std::vector<capsid_permission_rule>(),
                NULL,
                &error),
            "internal module was accepted in allowed_modules");
}

void test_manifest_matches_module_gates() {
    std::ifstream input(
        CAPSID_CAPABILITY_MANIFEST_PATH,
        std::ios::in | std::ios::binary);
    require(input.good(), "capability manifest could not be read");
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string manifest = contents.str();
    require(
        manifest.find(
            "\"profile\": \"CAPSID-MIN-2025-subset-v0\"") !=
                std::string::npos &&
            manifest.find("\"policy_version\": 2") !=
                std::string::npos,
        "capability manifest profile/version drifted");

    const char *const built[] = {
        "capsid:assert",
        "capsid:getopts",
        "capsid:hashing",
        "capsid:ipaddr",
        "capsid:utils",
        "capsid:uuid",
        "capsid:env",
        "capsid:permissions",
        "capsid:system",
        "capsid:storage",
        "capsid:stdio",
        "capsid:fs"
    };
    const char *const optional[] = {
        "capsid:ffi",
        "capsid:path",
        "capsid:raw-socket",
        "capsid:readline",
        "capsid:sqlite",
        "capsid:net",
        "capsid:websocket"
    };
    for (size_t index = 0;
         index < sizeof(built) / sizeof(built[0]);
         ++index) {
        require(
            manifest_array_contains(
                manifest, "built_and_available", built[index]),
            std::string("built module missing from manifest: ") +
                built[index]);
        capsid::CapabilityPolicy compiled;
        std::vector<const char *> modules(1, built[index]);
        std::string error;
        require(
            configure(
                &compiled,
                modules,
                std::vector<capsid_permission_rule>(),
                NULL,
                &error),
            error);
        require(
            compiled.module_decision(built[index]) ==
                capsid::kModuleGranted,
            std::string("manifest built module is unavailable: ") +
                built[index]);
    }
    const char *const utility_modules[] = {
        "capsid:assert",
        "capsid:getopts",
        "capsid:hashing",
        "capsid:ipaddr",
        "capsid:utils",
        "capsid:uuid"
    };
    for (size_t index = 0;
         index < sizeof(utility_modules) /
                     sizeof(utility_modules[0]);
         ++index) {
        const std::string contract =
            std::string("\"") + utility_modules[index] + "\":";
        const size_t contract_at = manifest.find(contract);
        require(
            contract_at != std::string::npos,
            std::string("utility module contract missing: ") +
                utility_modules[index]);
        const size_t next_contract =
            manifest.find("\n    \"", contract_at + contract.size());
        const std::string entry = manifest.substr(
            contract_at,
            next_contract == std::string::npos
                ? std::string::npos
                : next_contract - contract_at);
        require(
            entry.find("\"exports\": [") != std::string::npos &&
                entry.find("\"runtime_imports\": [") !=
                    std::string::npos &&
                entry.find("\"ambient_authority\": []") !=
                    std::string::npos,
            std::string(
                "utility module import/export/authority contract "
                "is incomplete: ") +
                utility_modules[index]);
    }
    for (size_t index = 0;
         index < sizeof(optional) / sizeof(optional[0]);
         ++index) {
        require(
            manifest_array_contains(
                manifest, "known_but_not_built", optional[index]),
            std::string("optional module missing from manifest: ") +
                optional[index]);
        capsid::CapabilityPolicy compiled;
        std::vector<const char *> modules(1, optional[index]);
        std::string error;
        require(
            configure(
                &compiled,
                modules,
                std::vector<capsid_permission_rule>(),
                NULL,
                &error),
            error);
        require(
            compiled.module_decision(optional[index]) ==
                capsid::kModuleUnavailable,
            std::string("optional module unexpectedly built: ") +
                optional[index]);
    }

    struct ForbiddenCase {
        const char *manifest_name;
        const char *specifier;
    };
    const ForbiddenCase forbidden[] = {
        { "data:", "data:text/javascript,export default 1" },
        { "file:", "file:///tmp/app.js" },
        { "http:", "http:opaque-module" },
        { "https:", "https:opaque-module" },
        { "node:", "node:fs" },
        { "relative-or-absolute-path-import", "./app.js" },
        { "tjs:child-process", "tjs:child-process" },
        { "tjs:http-server", "tjs:http-server" },
        { "tjs:internal/*", "tjs:internal/core" },
        { "tjs:process", "tjs:process" },
        { "tjs:signals", "tjs:signals" },
        { "tjs:wasi", "tjs:wasi" },
        { "tjs:*", "tjs:assert" },
        { "tjs:websocket-server", "tjs:websocket-server" },
        { "tjs:worker", "tjs:worker" },
        { "capsid:http-server", "capsid:http-server" },
        { "capsid:internal/*", "capsid:internal/core" },
        { "capsid:ipc", "capsid:ipc" },
        { "capsid:process", "capsid:process" },
        { "capsid:wasi", "capsid:wasi" },
        { "capsid:worker", "capsid:worker" }
    };
    capsid::CapabilityPolicy denied;
    std::string error;
    require(denied.configure(NULL, &error), error);
    for (size_t index = 0;
         index < sizeof(forbidden) / sizeof(forbidden[0]);
         ++index) {
        require(
            manifest_array_contains(
                manifest,
                "permanently_forbidden",
                forbidden[index].manifest_name),
            std::string("forbidden category missing from manifest: ") +
                forbidden[index].manifest_name);
        require(
            denied.module_decision(forbidden[index].specifier) ==
                capsid::kModuleForbidden,
            std::string("manifest forbidden module was not blocked: ") +
                forbidden[index].specifier);
    }
}

void test_scope_matching_and_deny_precedence() {
    std::vector<capsid_permission_rule> rules;
    rules.push_back(rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_ALLOW,
        "APP_*",
        10));
    rules.push_back(rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_DENY,
        "APP_SECRET",
        11));
    rules.push_back(rule(
        CAPSID_PERMISSION_READ,
        CAPSID_PERMISSION_ALLOW,
        "/srv/app/data",
        20));
    rules.push_back(rule(
        CAPSID_PERMISSION_READ,
        CAPSID_PERMISSION_DENY,
        "/srv/app/data/secrets",
        21));
    rules.push_back(rule(
        CAPSID_PERMISSION_SYS,
        CAPSID_PERMISSION_ALLOW,
        "networkInterfaces",
        30));
    rules.push_back(rule(
        CAPSID_PERMISSION_SYS,
        CAPSID_PERMISSION_ALLOW,
        "runtimeVersion",
        31));
    rules.push_back(rule(
        CAPSID_PERMISSION_RAW_SOCKET,
        CAPSID_PERMISSION_ALLOW,
        NULL,
        40));
    rules.push_back(rule(
        CAPSID_PERMISSION_STORAGE,
        CAPSID_PERMISSION_ALLOW,
        "tenant-a",
        50));
    rules.push_back(rule(
        CAPSID_PERMISSION_STORAGE,
        CAPSID_PERMISSION_DENY,
        "blocked",
        51));
    rules.push_back(rule(
        CAPSID_PERMISSION_STDIO,
        CAPSID_PERMISSION_ALLOW,
        "stdout",
        60));
    rules.push_back(rule(
        CAPSID_PERMISSION_STDIO,
        CAPSID_PERMISSION_ALLOW,
        "stdin",
        61));

    capsid::CapabilityPolicy compiled;
    std::string error;
    require(configure(
                &compiled,
                std::vector<const char *>(),
                rules,
                NULL,
                &error),
            error);

    capsid::PermissionDecision decision = compiled.evaluate(
        CAPSID_PERMISSION_ENV, "APP_MODE");
    require(decision.state == CAPSID_PERMISSION_STATE_GRANTED &&
                decision.rule_id == 10,
            "env suffix wildcard did not grant");
    decision = compiled.evaluate(
        CAPSID_PERMISSION_ENV, "APP_SECRET");
    require(decision.state == CAPSID_PERMISSION_STATE_DENIED &&
                decision.rule_id == 11,
            "env deny did not override allow");
    decision = compiled.evaluate(
        CAPSID_PERMISSION_ENV, "DATABASE_URL");
    require(decision.state == CAPSID_PERMISSION_STATE_DENIED,
            "unmatched env was granted");

    decision = compiled.evaluate(
        CAPSID_PERMISSION_READ, "/srv/app/data/public/file.txt");
    require(decision.state == CAPSID_PERMISSION_STATE_GRANTED &&
                decision.rule_id == 20,
            "path descendant was not granted");
    decision = compiled.evaluate(
        CAPSID_PERMISSION_READ, "/srv/app/data/secrets/key");
    require(decision.state == CAPSID_PERMISSION_STATE_DENIED &&
                decision.rule_id == 21,
            "nested path deny did not override allow");
    decision = compiled.evaluate(
        CAPSID_PERMISSION_READ, "/srv/app/database");
    require(decision.state == CAPSID_PERMISSION_STATE_DENIED,
            "path prefix ignored component boundary");
    decision = compiled.evaluate(
        CAPSID_PERMISSION_SYS, "networkInterfaces");
    require(decision.state == CAPSID_PERMISSION_STATE_GRANTED,
            "fixed sys kind was not granted");
    require(
        compiled.query(
            CAPSID_PERMISSION_SYS,
            "runtimeVersion",
            0).state == CAPSID_PERMISSION_STATE_GRANTED,
        "safe built system information queried as unavailable");
    require(
        compiled.query(
            CAPSID_PERMISSION_SYS,
            "networkInterfaces",
            0).state == CAPSID_PERMISSION_STATE_UNAVAILABLE,
        "ambient system information was exposed by the partial module");
    decision = compiled.evaluate(
        CAPSID_PERMISSION_RAW_SOCKET, "");
    require(decision.state == CAPSID_PERMISSION_STATE_GRANTED,
            "boolean capability was not granted");
    decision = compiled.query(
        CAPSID_PERMISSION_STORAGE, "tenant-a", 0);
    require(decision.state == CAPSID_PERMISSION_STATE_GRANTED &&
                decision.rule_id == 50,
            "built storage namespace did not preserve its allow rule");
    decision = compiled.query(
        CAPSID_PERMISSION_STORAGE, "blocked", 0);
    require(decision.state == CAPSID_PERMISSION_STATE_DENIED &&
                decision.rule_id == 51,
            "storage namespace deny did not win");
    decision = compiled.query(
        CAPSID_PERMISSION_STDIO, "stdout", 0);
    require(decision.state == CAPSID_PERMISSION_STATE_GRANTED &&
                decision.rule_id == 60,
            "built stdout operation did not preserve its allow rule");
    require(
        compiled.query(
            CAPSID_PERMISSION_STDIO,
            "stdin",
            0).state == CAPSID_PERMISSION_STATE_UNAVAILABLE,
            "stdin was exposed by the output-only stdio module");
    decision = compiled.query(
        CAPSID_PERMISSION_READ,
        "/srv/app/data/public/file.txt",
        0);
    require(decision.state == CAPSID_PERMISSION_STATE_GRANTED &&
                decision.rule_id == 20,
            "built fs read operation did not preserve its allow rule");
    require(
        compiled.query(
            CAPSID_PERMISSION_WRITE,
            "/srv/app/data/public/file.txt",
            0).state == CAPSID_PERMISSION_STATE_UNAVAILABLE,
        "write access was exposed by the read-only fs module");

    require(
        compiled.query(
            CAPSID_PERMISSION_ENV,
            "APP_MODE",
            0).state == CAPSID_PERMISSION_STATE_GRANTED,
        "built env operation did not preserve its allow rule");
    require(compiled.query(CAPSID_PERMISSION_NET, "", 0).state ==
                CAPSID_PERMISSION_STATE_DENIED,
            "missing net policy did not query as denied");
}

void test_net_policy_and_rule_ids() {
    std::vector<capsid_egress_rule> rules;
    rules.push_back(net_rule(
        CAPSID_EGRESS_ALLOW, "*.example.com", 443, 101));
    rules.push_back(net_rule(
        CAPSID_EGRESS_DENY, "blocked.example.com", 443, 102));
    capsid_egress_policy net;
    capsid_egress_policy_init(&net);
    net.rules = &rules[0];
    net.rule_count = static_cast<uint32_t>(rules.size());

    capsid::CapabilityPolicy compiled;
    std::string error;
    require(configure(
                &compiled,
                std::vector<const char *>(),
                std::vector<capsid_permission_rule>(),
                &net,
                &error),
            error);

    capsid::PermissionDecision decision =
        compiled.query(CAPSID_PERMISSION_NET, "api.example.com", 443);
    require(decision.state == CAPSID_PERMISSION_STATE_GRANTED &&
                decision.rule_id == 101,
            "net allow decision lost rule id");
    decision =
        compiled.query(CAPSID_PERMISSION_NET, "blocked.example.com", 443);
    require(decision.state == CAPSID_PERMISSION_STATE_DENIED &&
                decision.rule_id == 102,
            "net deny did not override allow or lost rule id");
    decision = compiled.query(CAPSID_PERMISSION_NET, "", 0);
    require(decision.state == CAPSID_PERMISSION_STATE_PARTIAL,
            "scoped net policy did not query as partial");
}

void test_copy_lifetime_and_atomic_failure() {
    std::string identity("before");
    std::string module("capsid:permissions");
    std::string resource("APP_*");
    std::vector<const char *> modules(1, module.c_str());
    std::vector<capsid_permission_rule> rules;
    rules.push_back(rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_ALLOW,
        resource.c_str(),
        7));
    capsid_capability_policy descriptor;
    capsid_capability_policy_init(&descriptor);
    descriptor.application_identity = identity.c_str();
    descriptor.allowed_modules = &modules[0];
    descriptor.allowed_module_count = 1;
    descriptor.rules = &rules[0];
    descriptor.rule_count = 1;

    capsid::CapabilityPolicy compiled;
    std::string error;
    require(compiled.configure(&descriptor, &error), error);
    identity.assign("after");
    module.assign("capsid:fs");
    resource.assign("NOPE");
    require(compiled.application_identity() == "before",
            "policy retained application identity pointer");
    require(compiled.module_decision("capsid:permissions") ==
                capsid::kModuleGranted,
            "policy retained module pointer");
    require(compiled.evaluate(CAPSID_PERMISSION_ENV, "APP_MODE").state ==
                CAPSID_PERMISSION_STATE_GRANTED,
            "policy retained resource pointer");

    capsid_capability_policy invalid = descriptor;
    invalid.version = CAPSID_CAPABILITY_POLICY_VERSION + 1;
    require(!compiled.configure(&invalid, &error),
            "unknown policy version was accepted");
    require(compiled.application_identity() == "before" &&
                compiled.module_decision("capsid:permissions") ==
                    capsid::kModuleGranted,
            "failed configure partially mutated policy");
}

void test_explicit_environment_snapshot() {
    std::string name("APP_MODE");
    std::string value("production");
    const char *module = "capsid:env";
    capsid_permission_rule rules[2] = {
        rule(
            CAPSID_PERMISSION_ENV,
            CAPSID_PERMISSION_ALLOW,
            "APP_*",
            401),
        rule(
            CAPSID_PERMISSION_ENV,
            CAPSID_PERMISSION_DENY,
            "APP_SECRET",
            402)
    };
    capsid_env_entry entries[2] = {
        env_entry(name.c_str(), value.c_str()),
        env_entry("APP_EMPTY", "")
    };
    capsid_capability_policy descriptor;
    capsid_capability_policy_init(&descriptor);
    descriptor.allowed_modules = &module;
    descriptor.allowed_module_count = 1;
    descriptor.rules = rules;
    descriptor.rule_count = 2;
    descriptor.env_entries = entries;
    descriptor.env_entry_count = 2;

    capsid::CapabilityPolicy compiled;
    std::string error;
    require(compiled.configure(&descriptor, &error), error);
    std::string copied;
    require(
        compiled.env_value("APP_MODE", &copied) &&
            copied == "production",
        "explicit environment value was not copied");
    require(
        compiled.env_value("APP_EMPTY", &copied) &&
            copied.empty(),
        "empty environment value was confused with absence");
    require(
        !compiled.env_value("APP_MISSING", &copied),
        "absent environment value was synthesized");
    require(
        compiled.query(
            CAPSID_PERMISSION_ENV,
            "APP_MODE",
            0).state == CAPSID_PERMISSION_STATE_GRANTED,
        "built environment operation still queried as unavailable");

    name.assign("MUTATED");
    value.assign("mutated");
    entries[0].name = "MUTATED";
    entries[0].value = "mutated";
    require(
        compiled.env_value("APP_MODE", &copied) &&
            copied == "production",
        "environment snapshot retained caller-owned storage");

    capsid_env_entry invalid = env_entry("APP_SECRET", "forbidden");
    descriptor.env_entries = &invalid;
    descriptor.env_entry_count = 1;
    require(
        !compiled.configure(&descriptor, &error),
        "explicitly denied environment entry was accepted");

    invalid = env_entry("UNSCOPED", "forbidden");
    require(
        !compiled.configure(&descriptor, &error),
        "environment entry without an allow rule was accepted");

    invalid = env_entry("APP_MODE", "one");
    capsid_env_entry duplicate[2] = {
        invalid,
        env_entry("APP_MODE", "two")
    };
    descriptor.env_entries = duplicate;
    descriptor.env_entry_count = 2;
    require(
        !compiled.configure(&descriptor, &error),
        "duplicate environment entry was accepted");

    descriptor.allowed_modules = NULL;
    descriptor.allowed_module_count = 0;
    descriptor.env_entries = &invalid;
    descriptor.env_entry_count = 1;
    require(
        !compiled.configure(&descriptor, &error),
        "environment snapshot without capsid:env was accepted");

    descriptor.allowed_modules = &module;
    descriptor.allowed_module_count = 1;
    invalid = env_entry("APP_*", "forbidden");
    descriptor.env_entries = &invalid;
    require(
        !compiled.configure(&descriptor, &error),
        "wildcard environment entry name was accepted");

    invalid = env_entry("APP_MODE", NULL);
    require(
        !compiled.configure(&descriptor, &error),
        "NULL environment value was accepted");

    invalid = env_entry("APP_MODE", "value");
    invalid.struct_size--;
    require(
        !compiled.configure(&descriptor, &error),
        "short environment entry descriptor was accepted");

    invalid = env_entry("APP_MODE", "value");
    invalid.reserved = 1;
    require(
        !compiled.configure(&descriptor, &error),
        "environment entry reserved field was accepted");

    std::string oversized(16385, 'x');
    invalid = env_entry("APP_MODE", oversized.c_str());
    require(
        !compiled.configure(&descriptor, &error),
        "oversized environment value was accepted");

    descriptor.env_entries = NULL;
    descriptor.env_entry_count = 1;
    require(
        !compiled.configure(&descriptor, &error),
        "NULL environment entry array was accepted");

    descriptor.env_entries = &invalid;
    descriptor.env_entry_count = 257;
    require(
        !compiled.configure(&descriptor, &error),
        "oversized environment entry count was accepted");

    require(
        compiled.env_value("APP_MODE", &copied) &&
            copied == "production",
        "failed environment configure partially mutated snapshot");
}

void test_version_1_policy_compatibility() {
    struct CapabilityPolicyV1 {
        uint32_t struct_size;
        uint32_t version;
        const char *application_identity;
        const char *const *allowed_modules;
        uint32_t allowed_module_count;
        const capsid_permission_rule *rules;
        uint32_t rule_count;
        const capsid_egress_policy *net_policy;
        uint32_t reserved;
    };
    require(
        sizeof(CapabilityPolicyV1) ==
            offsetof(
                capsid_capability_policy,
                env_entries),
        "version 1 capability policy layout drifted");
    CapabilityPolicyV1 legacy = {};
    legacy.struct_size = sizeof(legacy);
    legacy.version =
        CAPSID_CAPABILITY_POLICY_VERSION_1;
    legacy.application_identity = "legacy-policy";
    const char *module = "capsid:permissions";
    legacy.allowed_modules = &module;
    legacy.allowed_module_count = 1;

    capsid::CapabilityPolicy compiled;
    std::string error;
    require(
        compiled.configure(
            reinterpret_cast<
                const capsid_capability_policy *>(
                &legacy),
            &error),
        error);
    require(
        compiled.enabled() &&
            compiled.version() ==
                CAPSID_CAPABILITY_POLICY_VERSION_1 &&
            compiled.application_identity() ==
                "legacy-policy" &&
            compiled.module_decision(
                "capsid:permissions") ==
                capsid::kModuleGranted &&
            compiled.env_entries().empty(),
        "version 1 capability policy changed semantics");
}

void expect_invalid_rule(const capsid_permission_rule &invalid,
                         const char *label) {
    capsid::CapabilityPolicy compiled;
    std::vector<capsid_permission_rule> rules(1);
    std::memcpy(&rules[0], &invalid, sizeof(invalid));
    std::string error;
    require(!configure(
                &compiled,
                std::vector<const char *>(),
                rules,
                NULL,
                &error),
            std::string(label) + " was accepted");
    require(!error.empty(), std::string(label) + " returned no error");
}

void test_malformed_rules() {
    capsid_permission_rule invalid = rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_ALLOW,
        "x",
        1);
    uint32_t invalid_value = 999;
    std::memcpy(
        &invalid.permission,
        &invalid_value,
        sizeof(invalid_value));
    expect_invalid_rule(invalid, "unknown permission");

    invalid = rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_ALLOW,
        "APP_*",
        1);
    std::memcpy(
        &invalid.action,
        &invalid_value,
        sizeof(invalid_value));
    expect_invalid_rule(invalid, "unknown action");

    invalid = rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_ALLOW,
        "APP_*_BAD",
        1);
    expect_invalid_rule(invalid, "non-suffix env wildcard");

    invalid = rule(
        CAPSID_PERMISSION_READ,
        CAPSID_PERMISSION_ALLOW,
        "/srv/app/../secret",
        1);
    expect_invalid_rule(invalid, "non-canonical path");

    invalid = rule(
        CAPSID_PERMISSION_SYS,
        CAPSID_PERMISSION_ALLOW,
        "arbitraryNativeCall",
        1);
    expect_invalid_rule(invalid, "unknown sys kind");

    invalid = rule(
        CAPSID_PERMISSION_RAW_SOCKET,
        CAPSID_PERMISSION_ALLOW,
        "tcp",
        1);
    expect_invalid_rule(invalid, "scoped boolean permission");

    invalid = rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_ALLOW,
        "APP_*",
        0);
    expect_invalid_rule(invalid, "zero rule id");

    invalid = rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_ALLOW,
        "APP_*",
        1);
    invalid.reserved = 1;
    expect_invalid_rule(invalid, "nonzero reserved field");

    std::vector<capsid_permission_rule> duplicate;
    duplicate.push_back(rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_ALLOW,
        "APP_*",
        1));
    duplicate.push_back(rule(
        CAPSID_PERMISSION_ENV,
        CAPSID_PERMISSION_DENY,
        "APP_SECRET",
        1));
    capsid::CapabilityPolicy compiled;
    std::string error;
    require(!configure(
                &compiled,
                std::vector<const char *>(),
                duplicate,
                NULL,
                &error),
            "duplicate rule id was accepted");
}

// Binding v1 §7.3: per-binding policies and origin isolation
// (docs/binding-technical-design.md §3.1-§3.3). The binding policy set is
// compiled from wire descriptors and is fully separate from the User
// policy: a Binding grant can never widen what the User can do, an
// unknown Binding ID fails closed, and Native Handles carry an
// unforgeable owner that only their origin may touch.

capsid::WorkerBindingDescriptor binding_descriptor(
    const std::string &name,
    const std::vector<std::string> &modules,
    const std::vector<std::string> &profiles,
    const std::vector<std::string> &net_rules,
    const std::vector<std::string> &fs_write) {
    capsid::WorkerBindingDescriptor descriptor;
    descriptor.name = name;
    descriptor.source.assign(3, 0x5a);
    descriptor.config_json = "{}";
    descriptor.modules = modules;
    descriptor.profiles = profiles;
    descriptor.net_rules = net_rules;
    descriptor.fs_read.clear();
    descriptor.fs_write = fs_write;
    descriptor.env.push_back("APP_MODE");
    descriptor.stdio.push_back("stdout");
    return descriptor;
}

void test_binding_policy_origin_isolation() {
    const capsid::WorkerBindingDescriptor mongo = binding_descriptor(
        "mongo",
        {"tjs:internal/core", "tjs:posix-socket"},
        {"network-client", "filesystem-write"},
        {"127.0.0.1:27017"},
        {"/var/lib/capsid/mongo"});

    capsid::BindingPolicySet set;
    std::string error;
    require(set.configure(
                std::vector<capsid::WorkerBindingDescriptor>{mongo},
                &error),
            "valid binding policy set was rejected: " + error);
    require(set.has("mongo") && !set.has("redis"),
            "binding set lookup failed");
    require(set.ids() == std::vector<std::string>{"mongo"},
            "binding set ids are wrong");

    const capsid::BindingPolicy *policy = set.policy("mongo");
    require(policy != NULL, "mongo binding policy missing");

    // §3.1: the Binding gate only consults the named Binding policy.
    require(policy->egress.allows_host("127.0.0.1", 27017),
            "authorized binding net target was denied");
    require(!policy->egress.allows_host("127.0.0.1", 9999),
            "wrong port was allowed by the binding net policy");
    require(!policy->egress.allows_host("evil.example.com", 27017),
            "unknown host was allowed by the binding net policy");

    require(policy->module_decision("tjs:internal/core") ==
                capsid::kModuleGranted,
            "granted binding module was denied");
    require(policy->module_decision("tjs:posix-socket") ==
                capsid::kModuleGranted,
            "granted binding module was denied");
    require(policy->module_decision("capsid:fs") ==
                capsid::kModuleDenied,
            "user facade was granted to the binding policy");
    require(policy->module_decision("tjs:ffi") ==
                capsid::kModuleForbidden,
            "permanently forbidden module was not forbidden");
    require(policy->module_decision("tjs:utils") ==
                capsid::kModuleDenied,
            "ungranted module was allowed");

    require(policy->capability.evaluate(
                CAPSID_PERMISSION_WRITE,
                "/var/lib/capsid/mongo")
                .state == CAPSID_PERMISSION_STATE_GRANTED,
            "binding fs write path was denied");
    require(policy->capability.evaluate(
                CAPSID_PERMISSION_WRITE,
                "/var/lib/capsid/mongo/journal")
                .state == CAPSID_PERMISSION_STATE_GRANTED,
            "binding fs write child path was denied");
    require(policy->capability.evaluate(
                CAPSID_PERMISSION_READ,
                "/var/lib/capsid/mongo")
                .state == CAPSID_PERMISSION_STATE_DENIED,
            "binding fs read on a write-only path was granted");
    require(policy->capability.evaluate(
                CAPSID_PERMISSION_WRITE,
                "/var/lib/capsid/other")
                .state == CAPSID_PERMISSION_STATE_DENIED,
            "binding fs write escaped the authorized directory");
    require(policy->env.size() == 1 && policy->env[0] == "APP_MODE",
            "binding env names are wrong");
    require(policy->stdio.size() == 1 && policy->stdio[0] == "stdout",
            "binding stdio streams are wrong");
    require(policy->profiles.size() == 2 &&
                policy->profiles[0] == "network-client" &&
                policy->profiles[1] == "filesystem-write",
            "binding sandbox profiles are wrong");

    // Unknown Binding ID: fail closed at the set level, never a default
    // allow or an exception.
    require(set.policy("redis") == NULL,
            "unknown binding id resolved to a policy");
    require(set.evaluate("redis", CAPSID_PERMISSION_READ, "/x").state ==
                CAPSID_PERMISSION_STATE_DENIED,
            "unknown binding id evaluated as granted");

    // §3.2: the User policy is never widened by any Binding grant. The
    // user gate below holds a user-only policy and must still deny the
    // binding's write path and the binding's tjs modules.
    capsid::CapabilityPolicy user_policy;
    std::vector<capsid_permission_rule> user_rules;
    user_rules.push_back(rule(
        CAPSID_PERMISSION_READ,
        CAPSID_PERMISSION_ALLOW,
        "/srv/apps/orders/public",
        7));
    require(configure(
                &user_policy,
                std::vector<const char *>{"capsid:fs"},
                user_rules,
                NULL,
                &error),
            "user policy fixture rejected: " + error);
    require(user_policy.evaluate(
                CAPSID_PERMISSION_WRITE,
                "/var/lib/capsid/mongo")
                .state == CAPSID_PERMISSION_STATE_DENIED,
            "binding fs write widened the user policy");
    require(user_policy.evaluate(
                CAPSID_PERMISSION_READ,
                "/var/lib/capsid/mongo")
                .state == CAPSID_PERMISSION_STATE_DENIED,
            "binding fs read widened the user policy");
    require(user_policy.module_decision("tjs:internal/core") !=
                capsid::kModuleGranted,
            "binding module grant widened the user module policy");

    // Atomic configure: any invalid descriptor rejects the whole set and
    // leaves no partial state behind.
    const capsid::WorkerBindingDescriptor forbidden_module =
        binding_descriptor(
            "bad-module", {"tjs:ffi"}, {}, {}, {});
    capsid::BindingPolicySet partial;
    require(!partial.configure(
                std::vector<capsid::WorkerBindingDescriptor>{
                    mongo, forbidden_module},
                &error) &&
                !error.empty(),
            "forbidden binding module was accepted");
    require(!partial.has("mongo") && partial.ids().empty(),
            "failed configure left partial binding state");

    const capsid::WorkerBindingDescriptor bad_target =
        binding_descriptor(
            "bad-target", {"tjs:utils"}, {}, {"noport"}, {});
    capsid::BindingPolicySet target_set;
    require(!target_set.configure(
                std::vector<capsid::WorkerBindingDescriptor>{bad_target},
                &error) &&
                !error.empty(),
            "invalid binding net target was accepted");

    const capsid::WorkerBindingDescriptor bad_profile =
        binding_descriptor(
            "bad-profile", {"tjs:utils"}, {"network-server"}, {}, {});
    capsid::BindingPolicySet profile_set;
    require(!profile_set.configure(
                std::vector<capsid::WorkerBindingDescriptor>{bad_profile},
                &error) &&
                !error.empty(),
            "unknown binding sandbox profile was accepted");

    const capsid::WorkerBindingDescriptor same_id =
        binding_descriptor(
            "mongo", {"tjs:utils"}, {}, {}, {});
    capsid::BindingPolicySet duplicate_set;
    require(!duplicate_set.configure(
                std::vector<capsid::WorkerBindingDescriptor>{
                    mongo, same_id},
                &error) &&
                !error.empty(),
            "duplicate binding ids were accepted");

    // §3.1: Native Handle ownership. A handle without a recorded owner is
    // invalid; only its own origin may touch it.
    capsid::NativeHandleOwner handle;
    require(!handle.valid(),
            "ownerless handle is valid");

    capsid::RuntimeOrigin user_origin;
    require(user_origin.valid(),
            "default user origin is invalid");
    capsid::RuntimeOrigin mongo_origin;
    mongo_origin.domain = capsid::RuntimeDomain::kBinding;
    require(!mongo_origin.valid(),
            "binding origin without an id is valid");
    mongo_origin.binding_id = "mongo";
    require(mongo_origin.valid(),
            "binding origin with an id is invalid");
    capsid::RuntimeOrigin redis_origin;
    redis_origin.domain = capsid::RuntimeDomain::kBinding;
    redis_origin.binding_id = "redis";

    handle.origin = mongo_origin;
    handle.open_mode = 2;  // O_WRONLY creation mode
    handle.recorded = true;
    require(handle.valid(),
            "owned handle is invalid");
    require(capsid::handle_owner_matches(handle, mongo_origin),
            "the owning origin was denied");
    require(!capsid::handle_owner_matches(handle, user_origin),
            "the user origin touched a binding handle");
    require(!capsid::handle_owner_matches(handle, redis_origin),
            "a different binding touched the handle");
    capsid::RuntimeOrigin no_id;
    no_id.domain = capsid::RuntimeDomain::kBinding;
    require(!capsid::handle_owner_matches(handle, no_id),
            "an invalid caller origin touched the handle");
}

}  // namespace

int main() {
    test_initializers_and_default_deny();
    test_module_build_and_visibility_gates();
    test_manifest_matches_module_gates();
    test_scope_matching_and_deny_precedence();
    test_net_policy_and_rule_ids();
    test_copy_lifetime_and_atomic_failure();
    test_explicit_environment_snapshot();
    test_version_1_policy_compatibility();
    test_malformed_rules();
    test_binding_policy_origin_isolation();
    return 0;
}
