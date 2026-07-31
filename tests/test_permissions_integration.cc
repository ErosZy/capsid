#include "capsid/runtime.h"

#include <poll.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Audit {
    capsid_audit_stage stage;
    capsid_audit_decision decision;
    uint64_t worker_id;
    uint64_t request_id;
    uint32_t rule_id;
    uint32_t policy_version;
    std::string application;
    std::string module;
    std::string capability;
    std::string resource_kind;
    std::string resource;
    std::string manifest_hash;
};

void fail(const std::string &message) {
    std::cerr << "test-permissions-integration: "
              << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(
            std::string(operation) + ": " +
            capsid_result_string(result));
    }
}

std::string bytes(const capsid_bytes &value) {
    return value.size == 0
               ? std::string()
               : std::string(
                     reinterpret_cast<const char *>(value.data),
                     value.size);
}

Audit decode_audit(const capsid_event &event) {
    capsid_audit_record record;
    capsid_audit_record_init(&record);
    require_result(
        capsid_audit_record_decode(&event, &record),
        "decode audit");
    Audit decoded;
    decoded.stage = record.stage;
    decoded.decision = record.decision;
    decoded.worker_id = record.worker_id;
    decoded.request_id = record.request_id;
    decoded.rule_id = record.rule_id;
    decoded.policy_version = record.policy_version;
    decoded.application = bytes(record.application_identity);
    decoded.module = bytes(record.module);
    decoded.capability = bytes(record.capability);
    decoded.resource_kind = bytes(record.resource_kind);
    decoded.resource = bytes(record.resource);
    decoded.manifest_hash = bytes(record.manifest_hash);
    require(
        decoded.worker_id != 0 &&
            decoded.manifest_hash.size() == 64,
        "audit identity/manifest fields are incomplete");
    return decoded;
}

void wait_io(capsid_worker *worker, bool writable) {
    struct pollfd descriptor = {};
    descriptor.fd = capsid_worker_fd(worker);
    descriptor.events =
        POLLIN | (writable ? POLLOUT : 0);
    poll(&descriptor, 1, 50);
}

std::string event_text(const capsid_event &event) {
    return bytes(event.payload);
}

bool has_audit(const std::vector<Audit> &audits,
               capsid_audit_stage stage,
               capsid_audit_decision decision,
               const char *module,
               const char *capability,
               uint32_t rule_id) {
    for (std::vector<Audit>::const_iterator it = audits.begin();
         it != audits.end();
         ++it) {
        if (it->stage == stage &&
            it->decision == decision &&
            it->module == (module ? module : "") &&
            it->capability ==
                (capability ? capability : "") &&
            it->rule_id == rule_id) {
            return true;
        }
    }
    return false;
}

size_t count_audits(const std::vector<Audit> &audits,
                    capsid_audit_stage stage,
                    capsid_audit_decision decision,
                    const char *capability,
                    uint32_t rule_id) {
    size_t count = 0;
    for (std::vector<Audit>::const_iterator it = audits.begin();
         it != audits.end();
         ++it) {
        if (it->stage == stage &&
            it->decision == decision &&
            it->capability ==
                (capability ? capability : "") &&
            it->rule_id == rule_id) {
            ++count;
        }
    }
    return count;
}

bool has_stage(const std::vector<Audit> &audits,
               capsid_audit_stage stage) {
    for (std::vector<Audit>::const_iterator it = audits.begin();
         it != audits.end();
         ++it) {
        if (it->stage == stage) {
            return true;
        }
    }
    return false;
}

void load_and_expect_startup(
    capsid_worker *worker,
    const std::string &bundle,
    bool expect_ready,
    const char *expected_error,
    std::vector<Audit> *audits) {
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load bundle");
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(
                std::string("startup flush: ") +
                capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_AUDIT) {
                audits->push_back(decode_audit(event));
                continue;
            }
            if (event.type == CAPSID_EVENT_READY) {
                require(
                    expect_ready,
                    "unexpected READY for denied module");
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                require(
                    !expect_ready,
                    std::string("unexpected startup error: ") +
                        event_text(event));
                require(
                    event_text(event).find(expected_error) !=
                        std::string::npos,
                    std::string("wrong startup error: ") +
                        event_text(event));
                return;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before startup decision");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(
                std::string("startup event: ") +
                capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("startup decision timed out");
        }
        wait_io(worker, flush == CAPSID_WOULD_BLOCK);
    }
}

std::string run_request_id(
    capsid_worker *worker,
    uint64_t request_id,
    std::vector<Audit> *audits,
    std::vector<std::string> *logs = NULL) {
    require_result(
        capsid_worker_begin_request(
            worker,
            request_id,
            "GET",
            "https://application.test/",
            NULL,
            0),
        "begin request");
    require_result(
        capsid_worker_end_request(worker, request_id),
        "end request");
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    std::string body;
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(
                std::string("request flush: ") +
                capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_AUDIT) {
                audits->push_back(decode_audit(event));
                continue;
            }
            if (event.type == CAPSID_EVENT_LOG) {
                require(
                    event.payload.size >= 2,
                    "log payload is truncated");
                const size_t level_size =
                    static_cast<size_t>(event.payload.data[0]) |
                    (static_cast<size_t>(
                         event.payload.data[1]) << 8);
                require(
                    level_size <= event.payload.size - 2,
                    "log level length is invalid");
                if (logs) {
                    const char *payload =
                        reinterpret_cast<const char *>(
                            event.payload.data);
                    logs->push_back(
                        std::string(payload + 2, level_size) +
                        "|" +
                        std::string(
                            payload + 2 + level_size,
                            event.payload.size -
                                2 - level_size));
                }
                continue;
            }
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT ||
                event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                require(
                    event.request_id == request_id,
                    "response body request id mismatch");
                body.append(
                    reinterpret_cast<const char *>(
                        event.payload.data),
                    event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker,
                        event.request_id,
                        static_cast<uint32_t>(
                            event.payload.size)),
                    "grant response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                require(
                    event.request_id == request_id,
                    "response end request id mismatch");
                return body;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(
                    std::string("request error: ") +
                    event_text(event));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during permission query");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(
                std::string("request event: ") +
                capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("permission request timed out");
        }
        wait_io(worker, flush == CAPSID_WOULD_BLOCK);
    }
}

std::string run_request(
    capsid_worker *worker,
    std::vector<Audit> *audits) {
    return run_request_id(worker, 9, audits);
}

capsid_worker *spawn(
    const char *worker_path,
    const std::vector<const char *> &modules,
    capsid_egress_policy *net_policy,
    capsid_egress_policy *legacy_policy = NULL) {
    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "permissions-test-app";
    capability.allowed_modules =
        modules.empty() ? NULL : &modules[0];
    capability.allowed_module_count =
        static_cast<uint32_t>(modules.size());
    capability.net_policy = net_policy;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.capability_policy = &capability;
    config.egress_policy = legacy_policy;

    capsid_worker *worker = NULL;
    require_result(
        capsid_worker_spawn(&config, &worker),
        "spawn worker");
    return worker;
}

void test_query_and_operation_audit(const char *worker_path) {
    capsid_egress_rule rules[2];
    capsid_egress_rule_init(&rules[0]);
    rules[0].action = CAPSID_EGRESS_ALLOW;
    rules[0].target = "*.example.com";
    rules[0].port_start = rules[0].port_end = 443;
    rules[0].rule_id = 101;
    capsid_egress_rule_init(&rules[1]);
    rules[1].action = CAPSID_EGRESS_DENY;
    rules[1].target = "blocked.example.com";
    rules[1].port_start = rules[1].port_end = 443;
    rules[1].rule_id = 102;
    capsid_egress_policy net;
    capsid_egress_policy_init(&net);
    net.rules = rules;
    net.rule_count = 2;

    std::vector<const char *> modules;
    modules.push_back("capsid:permissions");
    capsid_worker *worker = spawn(worker_path, modules, &net);
    const std::string bundle =
        "import { permissions } from 'capsid:permissions';\n"
        "export default { async fetch() {\n"
        "  const allowed = permissions.query({ name: 'net', "
        "host: 'api.example.com', port: 443 });\n"
        "  const blocked = permissions.query({ name: 'net', "
        "host: 'blocked.example.com', port: 443 });\n"
        "  const summary = permissions.query({ name: 'net' });\n"
        "  const env = permissions.query({ name: 'env', "
        "variable: 'APP_MODE' });\n"
        "  let mutationRejected = false;\n"
        "  try { permissions.query = () => 'granted'; } "
        "catch (_) { mutationRejected = true; }\n"
        "  let unknownRejected = false;\n"
        "  try { permissions.query({ name: 'unknown' }); } "
        "catch (_) { unknownRejected = true; }\n"
        "  const fetchResult = await "
        "fetch('https://blocked.example.com/').then("
        "() => 'unexpected').catch(() => 'blocked');\n"
        "  for (let i = 0; i < 100; ++i) {\n"
        "    permissions.query({ name: 'net', "
        "host: 'blocked.example.com', port: 443 });\n"
        "  }\n"
        "  return new Response([allowed, blocked, summary, env, "
        "Object.isFrozen(permissions), mutationRejected, "
        "'request' in permissions, 'revoke' in permissions, "
        "unknownRejected, fetchResult].join('|'));\n"
        "} };\n";

    std::vector<Audit> audits;
    load_and_expect_startup(
        worker, bundle, true, NULL, &audits);
    const std::string body = run_request(worker, &audits);
    require(
        body ==
            "granted|denied|partial|denied|true|true|"
            "false|false|true|blocked",
        std::string("unexpected query result: ") + body);
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_MODULE,
            CAPSID_AUDIT_ALLOW,
            "capsid:permissions",
            "module",
            0),
        "allowed module audit missing");
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_QUERY,
            CAPSID_AUDIT_ALLOW,
            "capsid:permissions",
            "net",
            101),
        "granted net query audit missing");
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_QUERY,
            CAPSID_AUDIT_DENY,
            "capsid:permissions",
            "net",
            102),
        "denied net query audit missing");
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_DENY,
            "",
            "net",
            102),
        "direct-fetch operation denial audit missing");
    const size_t repeated_denials = count_audits(
        audits,
        CAPSID_AUDIT_STAGE_QUERY,
        CAPSID_AUDIT_DENY,
        "net",
        102);
    require(
        repeated_denials > 1 && repeated_denials <= 9,
        "same-denial audit did not preserve the first records "
        "or exceeded its per-event rate limit");
    for (std::vector<Audit>::const_iterator it = audits.begin();
         it != audits.end();
         ++it) {
        require(
            it->application == "permissions-test-app" &&
                it->policy_version ==
                    CAPSID_CAPABILITY_POLICY_VERSION,
            "audit policy identity/version mismatch");
    }
    capsid_worker_destroy(worker);
}

void test_network_policy_intersection(const char *worker_path) {
    capsid_egress_rule capability_rule;
    capsid_egress_rule_init(&capability_rule);
    capability_rule.action = CAPSID_EGRESS_ALLOW;
    capability_rule.target = "*.example.com";
    capability_rule.port_start =
        capability_rule.port_end = 443;
    capability_rule.rule_id = 201;
    capsid_egress_policy capability_net;
    capsid_egress_policy_init(&capability_net);
    capability_net.rules = &capability_rule;
    capability_net.rule_count = 1;

    capsid_egress_rule legacy_rules[2];
    capsid_egress_rule_init(&legacy_rules[0]);
    legacy_rules[0].action = CAPSID_EGRESS_ALLOW;
    legacy_rules[0].target = "*.example.com";
    legacy_rules[0].port_start =
        legacy_rules[0].port_end = 443;
    legacy_rules[0].rule_id = 211;
    capsid_egress_rule_init(&legacy_rules[1]);
    legacy_rules[1].action = CAPSID_EGRESS_DENY;
    legacy_rules[1].target = "intersection.example.com";
    legacy_rules[1].port_start =
        legacy_rules[1].port_end = 443;
    legacy_rules[1].rule_id = 212;
    capsid_egress_policy legacy_net;
    capsid_egress_policy_init(&legacy_net);
    legacy_net.rules = legacy_rules;
    legacy_net.rule_count = 2;

    std::vector<const char *> modules;
    modules.push_back("capsid:permissions");
    capsid_worker *worker = spawn(
        worker_path, modules, &capability_net, &legacy_net);
    const std::string bundle =
        "import { permissions } from 'capsid:permissions';\n"
        "export default { async fetch() {\n"
        "  const state = permissions.query({ name: 'net', "
        "host: 'intersection.example.com', port: 443 });\n"
        "  const result = await "
        "fetch('https://intersection.example.com/').then("
        "() => 'unexpected').catch(() => 'blocked');\n"
        "  return new Response(state + '|' + result);\n"
        "} };\n";
    std::vector<Audit> audits;
    load_and_expect_startup(
        worker, bundle, true, NULL, &audits);
    require(
        run_request(worker, &audits) == "denied|blocked",
        "legacy and capability net policies were not intersected");
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_QUERY,
            CAPSID_AUDIT_DENY,
            "capsid:permissions",
            "net",
            212) &&
            has_audit(
                audits,
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                "",
                "net",
                212),
        "intersection denial did not retain the legacy rule id");
    capsid_worker_destroy(worker);
}

void test_spawn_policy_copy_and_validation(const char *worker_path) {
    std::string identity("immutable-app");
    std::string module("capsid:permissions");
    std::string target("immutable.example.com");
    const char *module_pointer = module.c_str();

    capsid_egress_rule net_rule;
    capsid_egress_rule_init(&net_rule);
    net_rule.action = CAPSID_EGRESS_ALLOW;
    net_rule.target = target.c_str();
    net_rule.port_start = net_rule.port_end = 443;
    net_rule.rule_id = 301;
    capsid_egress_policy net;
    capsid_egress_policy_init(&net);
    net.rules = &net_rule;
    net.rule_count = 1;

    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = identity.c_str();
    capability.allowed_modules = &module_pointer;
    capability.allowed_module_count = 1;
    capability.net_policy = &net;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.capability_policy = &capability;
    capsid_worker *worker = NULL;
    require_result(
        capsid_worker_spawn(&config, &worker),
        "spawn immutable-policy worker");

    identity.assign("mutated-app");
    module.assign("capsid:fs");
    target.assign("mutated.example.com");
    net_rule.action = CAPSID_EGRESS_DENY;
    capability.allowed_module_count = 0;

    const std::string bundle =
        "import { permissions } from 'capsid:permissions';\n"
        "export default { fetch() {\n"
        "  return new Response(permissions.query({ name: 'net', "
        "host: 'immutable.example.com', port: 443 }));\n"
        "} };\n";
    std::vector<Audit> audits;
    load_and_expect_startup(
        worker, bundle, true, NULL, &audits);
    require(
        run_request(worker, &audits) == "granted",
        "spawn retained caller-owned policy storage");
    for (std::vector<Audit>::const_iterator it = audits.begin();
         it != audits.end();
         ++it) {
        require(
            it->application == "immutable-app",
            "spawn retained caller-owned identity storage");
    }
    capsid_worker_destroy(worker);

    capsid_capability_policy malformed;
    capsid_capability_policy_init(&malformed);
    malformed.version = CAPSID_CAPABILITY_POLICY_VERSION + 1;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.capability_policy = &malformed;
    worker = NULL;
    require(
        capsid_worker_spawn(&config, &worker) ==
                CAPSID_INVALID_ARGUMENT &&
            worker == NULL,
        "malformed capability policy reached worker spawn");
}

void test_module_gate(const char *worker_path,
                      const std::vector<const char *> &modules,
                      const char *specifier,
                      const char *expected_error,
                      capsid_audit_stage stage,
                      capsid_audit_decision decision) {
    capsid_worker *worker =
        spawn(worker_path, modules, NULL);
    const std::string bundle =
        std::string("import '") + specifier +
        "';\nexport default { fetch() { "
        "return new Response(); } };\n";
    std::vector<Audit> audits;
    load_and_expect_startup(
        worker,
        bundle,
        false,
        expected_error,
        &audits);
    require(
        has_audit(
            audits,
            stage,
            decision,
            specifier,
            "module",
            0),
        std::string("module gate audit missing for ") +
            specifier);
    capsid_worker_destroy(worker);
}

void test_utility_modules_contract(const char *worker_path) {
    static const char *const module_names[] = {
        "capsid:assert",
        "capsid:getopts",
        "capsid:hashing",
        "capsid:ipaddr",
        "capsid:utils",
        "capsid:uuid"
    };
    std::vector<const char *> modules(
        module_names,
        module_names +
            sizeof(module_names) / sizeof(module_names[0]));
    capsid_worker *worker = spawn(worker_path, modules, NULL);
    const std::string bundle =
        "import * as assertModule from 'capsid:assert';\n"
        "import * as getoptsModule from 'capsid:getopts';\n"
        "import * as hashingModule from 'capsid:hashing';\n"
        "import * as ipaddrModule from 'capsid:ipaddr';\n"
        "import * as utilsModule from 'capsid:utils';\n"
        "import * as uuidModule from 'capsid:uuid';\n"
        "const sameKeys = (value, keys) => "
        "JSON.stringify(Object.keys(value).sort()) === "
        "JSON.stringify(keys.slice().sort());\n"
        "const check = (condition, message) => { "
        "if (!condition) throw new Error(message); };\n"
        "export default { async fetch() {\n"
        "  check(sameKeys(assertModule, ['default']), "
        "'assert exports');\n"
        "  check(sameKeys(getoptsModule, ['default']), "
        "'getopts exports');\n"
        "  check(sameKeys(hashingModule, "
        "['SUPPORTED_TYPES', 'createHash']), 'hashing exports');\n"
        "  check(sameKeys(ipaddrModule, ['default']), "
        "'ipaddr exports');\n"
        "  check(sameKeys(utilsModule, ['format', 'inspect']), "
        "'utils exports');\n"
        "  check(sameKeys(uuidModule, ['default']), "
        "'uuid exports');\n"
        "  assertModule.default.ok(true);\n"
        "  const options = getoptsModule.default("
        "['--port', '42', '-v'], { boolean: ['v'] });\n"
        "  check(options.port === 42 && options.v === true, "
        "'getopts behavior');\n"
        "  check(hashingModule.SUPPORTED_TYPES.includes('sha256'), "
        "'hash list');\n"
        "  check(hashingModule.createHash('sha256').update('abc').digest() "
        "=== 'ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad', 'hash behavior');\n"
        "  check(ipaddrModule.default.parse('127.0.0.1').toString() "
        "=== '127.0.0.1', 'ipaddr behavior');\n"
        "  check(utilsModule.format('%s:%d', 'port', 42) === 'port:42', "
        "'utils behavior');\n"
        "  const id = uuidModule.default.v5('capsid', "
        "uuidModule.default.NIL);\n"
        "  check(uuidModule.default.validate(id) && "
        "uuidModule.default.version(id) === 5, 'uuid behavior');\n"
        "  for (const name of ['tjs', 'process', 'Deno', 'Bun']) {\n"
        "    check(!(name in globalThis), 'ambient global: ' + name);\n"
        "  }\n"
        "  let coreDenied = false;\n"
        "  try { await import('tjs:internal/core'); } "
        "catch (error) { coreDenied = "
        "String(error).includes('module is forbidden'); }\n"
        "  check(coreDenied, 'internal import escaped');\n"
        "  return new Response('ok');\n"
        "} };\n";

    std::vector<Audit> audits;
    load_and_expect_startup(
        worker, bundle, true, NULL, &audits);
    require(
        run_request(worker, &audits) == "ok",
        "utility module behavior contract failed");
    for (size_t index = 0;
         index < sizeof(module_names) / sizeof(module_names[0]);
         ++index) {
        require(
            has_audit(
                audits,
                CAPSID_AUDIT_STAGE_MODULE,
                CAPSID_AUDIT_ALLOW,
                module_names[index],
                "module",
                0),
            std::string("utility module allow audit missing: ") +
                module_names[index]);
    }
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_MODULE,
            CAPSID_AUDIT_DENY,
            "tjs:internal/core",
            "module",
            0),
        "direct internal-import denial audit missing");
    require(
        !has_stage(audits, CAPSID_AUDIT_STAGE_OPERATION),
        "utility module exercised ambient operation authority");
    capsid_worker_destroy(worker);
}

capsid_worker *spawn_environment_worker(
    const char *worker_path,
    std::string *mode_name,
    std::string *mode_value) {
    static const char *const modules[] = {
        "capsid:env"
    };
    capsid_permission_rule rules[3];
    capsid_permission_rule_init(&rules[0]);
    rules[0].action = CAPSID_PERMISSION_ALLOW;
    rules[0].permission = CAPSID_PERMISSION_ENV;
    rules[0].resource = "APP_*";
    rules[0].rule_id = 401;
    capsid_permission_rule_init(&rules[1]);
    rules[1].action = CAPSID_PERMISSION_DENY;
    rules[1].permission = CAPSID_PERMISSION_ENV;
    rules[1].resource = "APP_SECRET";
    rules[1].rule_id = 402;
    capsid_permission_rule_init(&rules[2]);
    rules[2].action = CAPSID_PERMISSION_ALLOW;
    rules[2].permission = CAPSID_PERMISSION_ENV;
    rules[2].resource = "CAPSID_*";
    rules[2].rule_id = 403;

    capsid_env_entry entries[2];
    capsid_env_entry_init(&entries[0]);
    entries[0].name = mode_name->c_str();
    entries[0].value = mode_value->c_str();
    capsid_env_entry_init(&entries[1]);
    entries[1].name = "APP_EMPTY";
    entries[1].value = "";

    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "environment-test-app";
    capability.allowed_modules = modules;
    capability.allowed_module_count = 1;
    capability.rules = rules;
    capability.rule_count = 3;
    capability.env_entries = entries;
    capability.env_entry_count = 2;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.capability_policy = &capability;
    capsid_worker *worker = NULL;
    require_result(
        capsid_worker_spawn(&config, &worker),
        "spawn environment worker");
    return worker;
}

std::string exercise_environment_worker(
    capsid_worker *worker,
    std::vector<Audit> *audits) {
    const std::string bundle =
        "import * as module from 'capsid:env';\n"
        "const { env } = module;\n"
        "export default { fetch() {\n"
        "  const exactExports = "
        "JSON.stringify(Object.keys(module)) === "
        "JSON.stringify(['env']);\n"
        "  const exactMethods = "
        "JSON.stringify(Object.keys(env)) === "
        "JSON.stringify(['get']);\n"
        "  let mutationRejected = false;\n"
        "  try { env.get = () => 'escaped'; } "
        "catch (_) { mutationRejected = true; }\n"
        "  let denied = false;\n"
        "  try { env.get('APP_SECRET'); } "
        "catch (error) { denied = "
        "String(error).includes('environment access denied'); }\n"
        "  let invalid = false;\n"
        "  try { env.get('APP_*'); } "
        "catch (error) { invalid = "
        "String(error).includes('invalid environment variable name'); }\n"
        "  return new Response([\n"
        "    exactExports, exactMethods, Object.isFrozen(env),\n"
        "    mutationRejected, env.get('APP_MODE'),\n"
        "    env.get('APP_EMPTY') === '',\n"
        "    env.get('APP_MISSING') === undefined,\n"
        "    env.get('CAPSID_AMBIENT_ONLY') === undefined,\n"
        "    denied, invalid,\n"
        "    !('tjs' in globalThis) && !('process' in globalThis)\n"
        "  ].join('|'));\n"
        "} };\n";
    load_and_expect_startup(
        worker, bundle, true, NULL, audits);
    return run_request(worker, audits);
}

void test_environment_module_contract(
    const char *worker_path) {
    require(
        setenv(
            "CAPSID_AMBIENT_ONLY",
            "must-not-leak",
            1) == 0,
        "could not prepare ambient environment negative control");

    std::string first_name("APP_MODE");
    std::string first_value("production");
    capsid_worker *first = spawn_environment_worker(
        worker_path, &first_name, &first_value);
    first_name.assign("MUTATED_NAME");
    first_value.assign("mutated-value");
    std::vector<Audit> first_audits;
    const std::string first_body =
        exercise_environment_worker(
            first, &first_audits);
    require(
        first_body ==
            "true|true|true|true|production|true|true|"
            "true|true|true|true",
        std::string(
            "unexpected environment contract result: ") +
            first_body);
    require(
        has_audit(
            first_audits,
            CAPSID_AUDIT_STAGE_MODULE,
            CAPSID_AUDIT_ALLOW,
            "capsid:env",
            "module",
            0),
        "environment module allow audit missing");
    require(
        has_audit(
            first_audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_ALLOW,
            "capsid:env",
            "env",
            401),
        "environment allow audit missing");
    require(
        has_audit(
            first_audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_DENY,
            "capsid:env",
            "env",
            402),
        "environment deny audit missing");
    capsid_worker_destroy(first);

    std::string second_name("APP_MODE");
    std::string second_value("staging");
    capsid_worker *second = spawn_environment_worker(
        worker_path, &second_name, &second_value);
    std::vector<Audit> second_audits;
    const std::string second_body =
        exercise_environment_worker(
            second, &second_audits);
    require(
        second_body.find("|staging|") !=
            std::string::npos &&
            second_body.find("production") ==
                std::string::npos,
        "environment values crossed worker boundaries");
    capsid_worker_destroy(second);
    unsetenv("CAPSID_AMBIENT_ONLY");
}

void test_system_module_contract(
    const char *worker_path) {
    const char *module = "capsid:system";
    capsid_permission_rule rules[3];
    capsid_permission_rule_init(&rules[0]);
    rules[0].action = CAPSID_PERMISSION_ALLOW;
    rules[0].permission = CAPSID_PERMISSION_SYS;
    rules[0].resource = "runtimeVersion";
    rules[0].rule_id = 501;
    capsid_permission_rule_init(&rules[1]);
    rules[1].action = CAPSID_PERMISSION_ALLOW;
    rules[1].permission = CAPSID_PERMISSION_SYS;
    rules[1].resource = "featureFlags";
    rules[1].rule_id = 502;
    capsid_permission_rule_init(&rules[2]);
    rules[2].action = CAPSID_PERMISSION_ALLOW;
    rules[2].permission = CAPSID_PERMISSION_SYS;
    rules[2].resource = "networkInterfaces";
    rules[2].rule_id = 503;

    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "system-test-app";
    capability.allowed_modules = &module;
    capability.allowed_module_count = 1;
    capability.rules = rules;
    capability.rule_count = 3;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.capability_policy = &capability;
    capsid_worker *worker = NULL;
    require_result(
        capsid_worker_spawn(&config, &worker),
        "spawn system worker");

    const std::string bundle =
        "import * as module from 'capsid:system';\n"
        "const { system } = module;\n"
        "export default { fetch() {\n"
        "  const version = system.get('runtimeVersion');\n"
        "  const flags = system.get('featureFlags');\n"
        "  let ambientUnavailable = false;\n"
        "  try { system.get('networkInterfaces'); } "
        "catch (error) { ambientUnavailable = "
        "String(error).includes('system information is unavailable'); }\n"
        "  return new Response([\n"
        "    JSON.stringify(Object.keys(module)) === "
        "JSON.stringify(['system']),\n"
        "    JSON.stringify(Object.keys(system)) === "
        "JSON.stringify(['get']),\n"
        "    Object.isFrozen(system), version === '0.1.0',\n"
        "    Object.isFrozen(flags),\n"
        "    JSON.stringify(Object.keys(flags).sort()) === "
        "JSON.stringify(['capabilityPolicyVersion','profile',"
        "'trustedBytecode','wasm']),\n"
        "    flags.profile === 'CAPSID-MIN-2025-subset-v0',\n"
        "    flags.capabilityPolicyVersion === 2,\n"
        "    flags.trustedBytecode === true && flags.wasm === true,\n"
        "    ambientUnavailable,\n"
        "    !('tjs' in globalThis) && !('process' in globalThis)\n"
        "  ].join('|'));\n"
        "} };\n";
    std::vector<Audit> audits;
    load_and_expect_startup(
        worker, bundle, true, NULL, &audits);
    const std::string body =
        run_request(worker, &audits);
    require(
        body ==
            "true|true|true|true|true|true|true|true|"
            "true|true|true",
        std::string(
            "unexpected system contract result: ") +
            body);
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_ALLOW,
            "capsid:system",
            "sys",
            501) &&
            has_audit(
                audits,
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_ALLOW,
                "capsid:system",
                "sys",
                502),
        "safe system metadata allow audits missing");
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_UNAVAILABLE,
            "capsid:system",
            "sys",
            0),
        "ambient system metadata unavailable audit missing");
    capsid_worker_destroy(worker);
}

void test_storage_module_contract(
    const char *worker_path) {
    const char *module = "capsid:storage";
    capsid_permission_rule rules[3];
    capsid_permission_rule_init(&rules[0]);
    rules[0].action = CAPSID_PERMISSION_ALLOW;
    rules[0].permission = CAPSID_PERMISSION_STORAGE;
    rules[0].resource = "tenant-a";
    rules[0].rule_id = 601;
    capsid_permission_rule_init(&rules[1]);
    rules[1].action = CAPSID_PERMISSION_DENY;
    rules[1].permission = CAPSID_PERMISSION_STORAGE;
    rules[1].resource = "blocked";
    rules[1].rule_id = 602;
    capsid_permission_rule_init(&rules[2]);
    rules[2].action = CAPSID_PERMISSION_ALLOW;
    rules[2].permission = CAPSID_PERMISSION_STORAGE;
    rules[2].resource = "entry-limit";
    rules[2].rule_id = 603;

    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "storage-test-app";
    capability.allowed_modules = &module;
    capability.allowed_module_count = 1;
    capability.rules = rules;
    capability.rule_count = 3;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.capability_policy = &capability;

    const std::string bundle =
        "import * as module from 'capsid:storage';\n"
        "const { storage } = module;\n"
        "export default { fetch() {\n"
        "  const current = Number(storage.get('tenant-a', 'count') ?? '0') + 1;\n"
        "  storage.set('tenant-a', 'count', String(current));\n"
        "  storage.set('tenant-a', 'unicode', '你好');\n"
        "  let denied = false;\n"
        "  try { storage.get('blocked', 'key'); } "
        "catch (error) { denied = String(error).includes('storage access denied'); }\n"
        "  let invalid = false;\n"
        "  try { storage.get('bad/name', 'key'); } "
        "catch (error) { invalid = String(error).includes('invalid storage namespace'); }\n"
        "  let largeValue = false;\n"
        "  try { storage.set('tenant-a', 'large', 'x'.repeat(16385)); } "
        "catch (error) { largeValue = String(error).includes('storage value exceeds'); }\n"
        "  storage.set('tenant-a', 'q1', 'x'.repeat(16384));\n"
        "  storage.set('tenant-a', 'q2', 'x'.repeat(16384));\n"
        "  storage.set('tenant-a', 'q3', 'x'.repeat(16384));\n"
        "  let quota = false;\n"
        "  try { storage.set('tenant-a', 'q4', 'x'.repeat(16384)); } "
        "catch (error) { quota = String(error).includes('storage quota exceeded'); }\n"
        "  for (let index = 0; index < 256; ++index) "
        "storage.set('entry-limit', `e${index}`, '');\n"
        "  let entryQuota = false;\n"
        "  try { storage.set('entry-limit', 'overflow', ''); } "
        "catch (error) { entryQuota = "
        "String(error).includes('storage quota exceeded'); }\n"
        "  const keys = storage.keys('tenant-a');\n"
        "  const deleted = storage.delete('tenant-a', 'unicode');\n"
        "  return new Response([\n"
        "    JSON.stringify(Object.keys(module)) === JSON.stringify(['storage']),\n"
        "    JSON.stringify(Object.keys(storage)) === "
        "JSON.stringify(['get','set','delete','clear','keys']),\n"
        "    Object.isFrozen(storage), Object.isFrozen(keys),\n"
        "    current, storage.get('tenant-a', 'unicode') === undefined,\n"
        "    deleted, denied, invalid, largeValue, quota, entryQuota,\n"
        "    keys.includes('count') && keys.includes('unicode'),\n"
        "    !('tjs' in globalThis) && !('process' in globalThis)\n"
        "  ].join('|'));\n"
        "} };\n";

    capsid_worker *first = NULL;
    require_result(
        capsid_worker_spawn(&config, &first),
        "spawn first storage worker");
    std::vector<Audit> first_audits;
    load_and_expect_startup(
        first, bundle, true, NULL, &first_audits);
    const std::string first_body =
        run_request_id(first, 6011, &first_audits);
    const std::string second_body =
        run_request_id(first, 6012, &first_audits);
    require(
        first_body ==
            "true|true|true|true|1|true|true|true|true|"
            "true|true|true|true|true",
        std::string("unexpected first storage result: ") +
            first_body);
    require(
        second_body.find("|2|") != std::string::npos,
        std::string("storage did not persist across requests: ") +
            second_body);
    require(
        has_audit(
            first_audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_ALLOW,
            "capsid:storage",
            "storage",
            601) &&
            has_audit(
                first_audits,
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                "capsid:storage",
                "storage",
                602),
        "storage namespace audits missing");
    require(
        count_audits(
            first_audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_DENY,
            "storage",
            601) >= 2,
        "storage quota denials were not audited");
    require(
        has_audit(
            first_audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_DENY,
            "capsid:storage",
            "storage",
            603),
        "storage entry-limit denial audit missing");
    capsid_worker_destroy(first);

    capsid_worker *second = NULL;
    require_result(
        capsid_worker_spawn(&config, &second),
        "spawn second storage worker");
    std::vector<Audit> second_audits;
    load_and_expect_startup(
        second, bundle, true, NULL, &second_audits);
    require(
        run_request_id(second, 6021, &second_audits).find("|1|") !=
            std::string::npos,
        "storage state crossed worker boundaries");
    capsid_worker_destroy(second);
}

void test_stdio_module_contract(
    const char *worker_path) {
    const char *module = "capsid:stdio";
    capsid_permission_rule rules[3];
    capsid_permission_rule_init(&rules[0]);
    rules[0].action = CAPSID_PERMISSION_ALLOW;
    rules[0].permission = CAPSID_PERMISSION_STDIO;
    rules[0].resource = "stdout";
    rules[0].rule_id = 701;
    capsid_permission_rule_init(&rules[1]);
    rules[1].action = CAPSID_PERMISSION_DENY;
    rules[1].permission = CAPSID_PERMISSION_STDIO;
    rules[1].resource = "stderr";
    rules[1].rule_id = 702;
    capsid_permission_rule_init(&rules[2]);
    rules[2].action = CAPSID_PERMISSION_ALLOW;
    rules[2].permission = CAPSID_PERMISSION_STDIO;
    rules[2].resource = "stdin";
    rules[2].rule_id = 703;

    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "stdio-test-app";
    capability.allowed_modules = &module;
    capability.allowed_module_count = 1;
    capability.rules = rules;
    capability.rule_count = 3;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.capability_policy = &capability;
    capsid_worker *worker = NULL;
    require_result(
        capsid_worker_spawn(&config, &worker),
        "spawn stdio worker");

    const std::string bundle =
        "import * as module from 'capsid:stdio';\n"
        "const { stdio } = module;\n"
        "export default { fetch() {\n"
        "  stdio.write('stdout', 'hello\\0world');\n"
        "  let denied = false;\n"
        "  try { stdio.write('stderr', 'secret'); } "
        "catch (error) { denied = "
        "String(error).includes('stdio access denied'); }\n"
        "  let stdinUnavailable = false;\n"
        "  try { stdio.write('stdin', 'input'); } "
        "catch (error) { stdinUnavailable = "
        "String(error).includes('stdio stream is unavailable'); }\n"
        "  let invalid = false;\n"
        "  try { stdio.write('console', 'bad'); } "
        "catch (error) { invalid = "
        "String(error).includes('invalid stdio stream'); }\n"
        "  let large = false;\n"
        "  try { stdio.write('stdout', 'x'.repeat(16385)); } "
        "catch (error) { large = "
        "String(error).includes('stdio message exceeds'); }\n"
        "  return new Response([\n"
        "    JSON.stringify(Object.keys(module)) === JSON.stringify(['stdio']),\n"
        "    JSON.stringify(Object.keys(stdio)) === JSON.stringify(['write']),\n"
        "    Object.isFrozen(stdio), denied, stdinUnavailable, invalid, large,\n"
        "    !('tjs' in globalThis) && !('process' in globalThis)\n"
        "  ].join('|'));\n"
        "} };\n";
    std::vector<Audit> audits;
    std::vector<std::string> logs;
    load_and_expect_startup(
        worker, bundle, true, NULL, &audits);
    require(
        run_request_id(worker, 7011, &audits, &logs) ==
            "true|true|true|true|true|true|true|true",
        "unexpected stdio contract result");
    require(logs.size() == 1, "stdio emitted unexpected log count");
    const std::string expected_log(
        "stdout|hello\0world", 18);
    require(
        logs[0] == expected_log,
        "stdio log payload did not preserve bytes");
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_ALLOW,
            "capsid:stdio",
            "stdio",
            701) &&
            has_audit(
                audits,
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                "capsid:stdio",
                "stdio",
                702) &&
            has_audit(
                audits,
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_UNAVAILABLE,
                "capsid:stdio",
                "stdio",
                0),
        "stdio allow/deny/unavailable audits missing");
    capsid_worker_destroy(worker);
}

void test_fs_module_contract(
    const char *worker_path) {
    char directory_template[] =
        "/tmp/capsid-fs-contract.XXXXXX";
    char *directory_value = mkdtemp(directory_template);
    require(directory_value != NULL, "create fs fixture directory");
    const std::string directory(directory_value);
    const std::string allowed = directory + "/allowed.txt";
    const std::string denied = directory + "/denied.txt";
    const std::string large = directory + "/large.txt";
    const std::string link = directory + "/link.txt";
    {
        std::ofstream output(allowed.c_str(), std::ios::binary);
        output << "hello";
    }
    {
        std::ofstream output(denied.c_str(), std::ios::binary);
        output << "secret";
    }
    {
        std::ofstream output(large.c_str(), std::ios::binary);
        std::string chunk(64u * 1024u, 'x');
        for (size_t index = 0; index < 17; ++index) {
            output.write(chunk.data(), chunk.size());
        }
    }
    require(
        symlink("/etc/passwd", link.c_str()) == 0,
        "create fs symlink fixture");

    const char *module = "capsid:fs";
    capsid_permission_rule rules[2];
    capsid_permission_rule_init(&rules[0]);
    rules[0].action = CAPSID_PERMISSION_ALLOW;
    rules[0].permission = CAPSID_PERMISSION_READ;
    rules[0].resource = directory.c_str();
    rules[0].rule_id = 801;
    capsid_permission_rule_init(&rules[1]);
    rules[1].action = CAPSID_PERMISSION_DENY;
    rules[1].permission = CAPSID_PERMISSION_READ;
    rules[1].resource = denied.c_str();
    rules[1].rule_id = 802;

    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "fs-test-app";
    capability.allowed_modules = &module;
    capability.allowed_module_count = 1;
    capability.rules = rules;
    capability.rule_count = 2;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.capability_policy = &capability;
    capsid_worker *worker = NULL;
    require_result(
        capsid_worker_spawn(&config, &worker),
        "spawn fs worker");

    const std::string bundle =
        "import * as module from 'capsid:fs';\n"
        "const { fs } = module;\n"
        "const root = '" + directory + "';\n"
        "export default { fetch() {\n"
        "  const entries = fs.list(root);\n"
        "  const info = fs.stat(root + '/allowed.txt');\n"
        "  let denied = false;\n"
        "  try { fs.readText(root + '/denied.txt'); } "
        "catch (error) { denied = "
        "String(error).includes('filesystem access denied'); }\n"
        "  let symlinkDenied = false;\n"
        "  try { fs.readText(root + '/link.txt'); } "
        "catch (error) { symlinkDenied = "
        "String(error).includes('filesystem symlinks are disabled'); }\n"
        "  let noncanonical = false;\n"
        "  try { fs.readText(root + '/../escape'); } "
        "catch (error) { noncanonical = "
        "String(error).includes('invalid filesystem path'); }\n"
        "  let quota = false;\n"
        "  try { fs.readText(root + '/large.txt'); } "
        "catch (error) { quota = "
        "String(error).includes('filesystem file exceeds'); }\n"
        "  return new Response([\n"
        "    JSON.stringify(Object.keys(module)) === JSON.stringify(['fs']),\n"
        "    JSON.stringify(Object.keys(fs)) === "
        "JSON.stringify(['readText','stat','list']), Object.isFrozen(fs),\n"
        "    fs.readText(root + '/allowed.txt') === 'hello',\n"
        "    info.type === 'file' && info.size === 5 && Object.isFrozen(info),\n"
        "    Object.isFrozen(entries),\n"
        "    JSON.stringify(entries) === "
        "JSON.stringify(['allowed.txt','denied.txt','large.txt','link.txt']),\n"
        "    denied, symlinkDenied, noncanonical, quota,\n"
        "    !('tjs' in globalThis) && !('process' in globalThis)\n"
        "  ].join('|'));\n"
        "} };\n";
    std::vector<Audit> audits;
    load_and_expect_startup(
        worker, bundle, true, NULL, &audits);
    require(
        run_request_id(worker, 8011, &audits) ==
            "true|true|true|true|true|true|true|true|"
            "true|true|true|true",
        "unexpected fs contract result");
    require(
        has_audit(
            audits,
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_ALLOW,
            "capsid:fs",
            "read",
            801) &&
            has_audit(
                audits,
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                "capsid:fs",
                "read",
                802),
        "fs allow/deny audits missing");
    capsid_worker_destroy(worker);

    rules[0].resource = link.c_str();
    capability.rule_count = 1;
    capsid_worker *symlink_root_worker = NULL;
    require_result(
        capsid_worker_spawn(
            &config, &symlink_root_worker),
        "spawn fs symlink-root worker");
    std::vector<Audit> symlink_root_audits;
    load_and_expect_startup(
        symlink_root_worker,
        "import { fs } from 'capsid:fs';\n"
        "export default { fetch() { return new Response(String(fs)); } };\n",
        false,
        "Landlock path must not be a symlink",
        &symlink_root_audits);
    capsid_worker_destroy(symlink_root_worker);

    unlink(link.c_str());
    unlink(large.c_str());
    unlink(denied.c_str());
    unlink(allowed.c_str());
    rmdir(directory.c_str());
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 &&
        std::string(argv[2]) == "--utility-modules") {
        test_utility_modules_contract(argv[1]);
        return 0;
    }
    if (argc == 3 &&
        std::string(argv[2]) == "--environment") {
        test_environment_module_contract(argv[1]);
        return 0;
    }
    if (argc == 3 &&
        std::string(argv[2]) == "--system") {
        test_system_module_contract(argv[1]);
        return 0;
    }
    if (argc == 3 &&
        std::string(argv[2]) == "--storage") {
        test_storage_module_contract(argv[1]);
        return 0;
    }
    if (argc == 3 &&
        std::string(argv[2]) == "--stdio") {
        test_stdio_module_contract(argv[1]);
        return 0;
    }
    if (argc == 3 &&
        std::string(argv[2]) == "--fs") {
        test_fs_module_contract(argv[1]);
        return 0;
    }
    if (argc != 2) {
        fail("expected capsid-worker path and optional test selector");
    }
    test_query_and_operation_audit(argv[1]);
    test_network_policy_intersection(argv[1]);
    test_spawn_policy_copy_and_validation(argv[1]);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "capsid:permissions",
        "module is not authorized: capsid:permissions",
        CAPSID_AUDIT_STAGE_MODULE,
        CAPSID_AUDIT_DENY);

    std::vector<const char *> unavailable;
    unavailable.clear();
    unavailable.push_back("capsid:readline");
    test_module_gate(
        argv[1],
        unavailable,
        "capsid:readline",
        "module is unavailable: capsid:readline",
        CAPSID_AUDIT_STAGE_BUILD,
        CAPSID_AUDIT_UNAVAILABLE);

    unavailable.clear();
    unavailable.push_back("capsid:ffi");
    test_module_gate(
        argv[1],
        unavailable,
        "capsid:ffi",
        "module is unavailable: capsid:ffi",
        CAPSID_AUDIT_STAGE_BUILD,
        CAPSID_AUDIT_UNAVAILABLE);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "tjs:assert",
        "module is forbidden: tjs:assert",
        CAPSID_AUDIT_STAGE_MODULE,
        CAPSID_AUDIT_DENY);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "tjs:internal/core",
        "module is forbidden: tjs:internal/core",
        CAPSID_AUDIT_STAGE_MODULE,
        CAPSID_AUDIT_DENY);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "tjs:process",
        "module is forbidden: tjs:process",
        CAPSID_AUDIT_STAGE_MODULE,
        CAPSID_AUDIT_DENY);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "tjs:http-server",
        "module is forbidden: tjs:http-server",
        CAPSID_AUDIT_STAGE_MODULE,
        CAPSID_AUDIT_DENY);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "tjs:worker",
        "module is forbidden: tjs:worker",
        CAPSID_AUDIT_STAGE_MODULE,
        CAPSID_AUDIT_DENY);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "tjs:wasi",
        "module is forbidden: tjs:wasi",
        CAPSID_AUDIT_STAGE_MODULE,
        CAPSID_AUDIT_DENY);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "capsid:wasi",
        "module is forbidden: capsid:wasi",
        CAPSID_AUDIT_STAGE_MODULE,
        CAPSID_AUDIT_DENY);

    test_module_gate(
        argv[1],
        std::vector<const char *>(),
        "unknown:module",
        "module is unavailable: unknown:module",
        CAPSID_AUDIT_STAGE_BUILD,
        CAPSID_AUDIT_UNAVAILABLE);
    return 0;
}
