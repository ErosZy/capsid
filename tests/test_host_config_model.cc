// WP-05 PR-08: typed host.json model tests. The schema validation
// (test_host_config.cc) is the authority on shape; this suite pins the typed
// projection and the semantic gates that live in the parse:
//   - every schema-accepted field maps into the model (nothing silently
//     ignored);
//   - admin.mode is exactly 0600;
//   - isolation.mode is exactly "strict";
//   - capacity.workersTotal is positive and within the Host int limit;
//   - the frozen size/duration/jitter grammars fail closed.
// A document that passes validate_config_json must parse here; a document
// that passes here must be a faithful projection of the schema fields.

#include "host/config.h"
#include "host/host_config_model.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using capsid::host::ConfigDocument;
using capsid::host::ParsedHostConfig;
using capsid::host::parse_host_config;
using capsid::host::validate_config_json;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-config-model: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

// The full feature surface, schema-valid, with every accepted field present.
constexpr std::string_view kFullDocument = R"json({
  "apiVersion": "capsid/host-v1",
  "applicationsRoot": "/srv/apps",
  "stateRoot": "/var/lib/capsid",
  "secretRootTemplate": "/var/lib/capsid/secrets/{application}",
  "admin": {"unix": "/run/capsid/admin.sock", "mode": "0600"},
  "listeners": [{
    "name": "public",
    "tcp": "0.0.0.0:8443",
    "publicScheme": "https",
    "publicAuthority": "apps.example.com",
    "routing": {"mode": "path", "suffix": "x-capsid-app"},
    "limits": {
      "connections": 128,
      "headerBytes": "64KiB",
      "headerTimeout": "5s",
      "bodyIdleTimeout": "30s",
      "streamIdleTimeout": "60s"
    }
  }],
  "permissions": {
    "modules": ["http", "net"],
    "environmentNames": ["NODE_ENV", "PUBLIC_*"],
    "fsReadRoots": ["/usr/share/data"],
    "fetchTargets": ["api.internal:443,80", "upstream.example.com"],
    "storageNamespaces": ["assets"],
    "stdioStreams": ["stdout"]
  },
  "isolation": {"mode": "strict", "required": ["pid"], "cgroupRoot": "/sys/fs/cgroup"},
  "trustedBytecodeKeys": {
    "release-key": "/etc/capsid/keys/release.raw",
    "ci-key": "/etc/capsid/keys/ci.raw"
  },
  "defaults": {
    "worker": {"jsHeap": "64MiB", "processAddressSpace": "256MiB",
               "memoryMax": "128MiB", "fileDescriptors": 64, "pidsMax": 32},
    "request": {"timeout": "30s", "maxInflightPerWorker": 8,
                "maxStreamingInflightPerWorker": 2,
                "streamIdleTimeoutMs": 60000, "writeTimeoutMs": 15000},
    "pool": {"queueRequests": 64, "queueHeaderBytes": "64KiB",
             "queueTimeout": "5s"}
  },
  "maximums": {
    "worker": {"jsHeap": "256MiB", "processAddressSpace": "1GiB",
               "memoryMax": "512MiB", "fileDescriptors": 256, "pidsMax": 128},
    "request": {"timeout": "60s", "maxInflightPerWorker": 32,
                "maxStreamingInflightPerWorker": 4,
                "streamIdleTimeoutMs": 120000, "writeTimeoutMs": 60000},
    "pool": {"queueRequests": 256, "queueHeaderBytes": "256KiB",
             "queueTimeout": "10s"}
  },
  "capacity": {
    "workersTotal": 4,
    "activationSurgeWorkers": 2,
    "startupsConcurrent": 2,
    "queuedRequestsTotal": 128,
    "queuedHeaderBytesTotal": "128KiB",
    "workerMemoryCommitTotal": "2GiB"
  },
  "recovery": {
    "crashBudget": {"maxEvents": 3, "window": "60s"},
    "restartBackoff": {"initial": "250ms", "maximum": "30s", "jitter": "1000"},
    "replacementsConcurrentPerApp": 1,
    "activeHealthInterval": "15s",
    "activeHealthFailures": 2
  }
})json";

void require_valid_parse(std::string_view json, const char* label,
                         ParsedHostConfig* out) {
    const auto validation = validate_config_json(ConfigDocument::kHost, json);
    require(validation.ok, std::string(label) + " is not schema-valid: '" +
                               validation.error.path + "': " +
                               validation.error.message);
    std::string error;
    require(parse_host_config(json, out, &error),
            std::string(label) + " was rejected by the typed parse: " + error);
}

void require_parse_error(std::string_view json, const char* label) {
    // The document must be schema-valid: the gate under test lives in the
    // typed parse, not in validate_config_json.
    const auto validation = validate_config_json(ConfigDocument::kHost, json);
    require(validation.ok, std::string(label) + " is not schema-valid: '" +
                               validation.error.path + "': " +
                               validation.error.message);
    ParsedHostConfig config;
    std::string error;
    require(!parse_host_config(json, &config, &error),
            std::string(label) + " was accepted by the typed parse");
    require(!error.empty(), std::string(label) + " produced no error text");
}

void test_full_document_maps_every_field() {
    ParsedHostConfig config;
    require_valid_parse(kFullDocument, "full document", &config);

    require(config.applications_root == "/srv/apps",
            "applicationsRoot did not map");
    require(config.state_root == "/var/lib/capsid", "stateRoot did not map");
    require(config.secret_root_template ==
                "/var/lib/capsid/secrets/{application}",
            "secretRootTemplate did not map");
    require(config.admin_unix_path == "/run/capsid/admin.sock",
            "admin.unix did not map");
    require(config.admin_mode == 0600, "admin.mode did not map");

    require(config.listeners.size() == 1, "listeners did not map");
    const auto& listener = config.listeners[0];
    require(listener.name == "public", "listener.name did not map");
    require(listener.tcp == "0.0.0.0:8443", "listener.tcp did not map");
    require(listener.public_scheme == "https",
            "listener.publicScheme did not map");
    require(listener.public_authority == "apps.example.com",
            "listener.publicAuthority did not map");
    require(listener.routing.mode == "path", "routing.mode did not map");
    require(listener.routing.suffix == "x-capsid-app",
            "routing.suffix did not map");
    require(listener.limits.connections == 128,
            "limits.connections did not map");
    require(listener.limits.header_bytes == 64U * 1024U,
            "limits.headerBytes did not convert to bytes");
    require(listener.limits.header_timeout_ms == 5000,
            "limits.headerTimeout did not convert to ms");
    require(listener.limits.body_idle_timeout_ms == 30000,
            "limits.bodyIdleTimeout did not convert to ms");
    require(listener.limits.stream_idle_timeout_ms == 60000,
            "limits.streamIdleTimeout did not convert to ms");

    require(config.policy.module_allowlist.size() == 2,
            "permissions.modules did not map");
    require(config.policy.env_patterns.size() == 2,
            "permissions.environmentNames did not map");
    require(config.policy.fs_read_roots.size() == 1,
            "permissions.fsReadRoots did not map");
    require(config.policy.fetch_targets.size() == 2,
            "permissions.fetchTargets did not map");
    require(config.policy.fetch_targets[0].host == "api.internal" &&
                config.policy.fetch_targets[0].ports.size() == 2,
            "fetch target ports did not map");
    require(config.policy.fetch_targets[1].ports.empty(),
            "any-port fetch target gained ports");
    require(config.policy.storage_allowed, "storage_allowed did not flip");
    require(config.policy.storage_namespaces.size() == 1,
            "permissions.storageNamespaces did not map");
    require(config.policy.stdio_allowed, "stdio_allowed did not flip");
    require(config.policy.stdio_streams.size() == 1,
            "permissions.stdioStreams did not map");

    require(config.isolation_mode == "strict", "isolation.mode did not map");
    require(config.isolation_required.size() == 1 &&
                config.isolation_required[0] == "pid",
            "isolation.required did not map");
    require(config.isolation_cgroup_root == "/sys/fs/cgroup",
            "isolation.cgroupRoot did not map");

    require(config.trusted_keys.size() == 2,
            "trustedBytecodeKeys did not map");
    require(config.trusted_keys[0].key_id == "release-key" &&
                config.trusted_keys[0].key_path ==
                    "/etc/capsid/keys/release.raw",
            "trusted key descriptor did not map");
    require(config.trusted_keys[1].key_id == "ci-key" &&
                config.trusted_keys[1].key_path == "/etc/capsid/keys/ci.raw",
            "second trusted key descriptor did not map");

    require(config.defaults.worker.js_heap == "64MiB",
            "defaults.worker.jsHeap did not map");
    require(config.defaults.worker.process_address_space == "256MiB",
            "defaults.worker.processAddressSpace did not map");
    require(config.defaults.worker.memory_max == "128MiB",
            "defaults.worker.memoryMax did not map");
    require(config.defaults.worker.file_descriptors == 64,
            "defaults.worker.fileDescriptors did not map");
    require(config.defaults.worker.pids_max == 32,
            "defaults.worker.pidsMax did not map");
    require(config.defaults.request.timeout == "30s",
            "defaults.request.timeout did not map");
    require(config.defaults.request.max_inflight_per_worker == 8,
            "defaults.request.maxInflightPerWorker did not map");
    require(config.defaults.request.max_streaming_inflight_per_worker == 2,
            "defaults.request.maxStreamingInflightPerWorker did not map");
    require(config.defaults.request.stream_idle_timeout_ms == 60000,
            "defaults.request.streamIdleTimeoutMs did not map");
    require(config.defaults.request.write_timeout_ms == 15000,
            "defaults.request.writeTimeoutMs did not map");
    require(config.defaults.pool.queue_requests == 64,
            "defaults.pool.queueRequests did not map");
    require(config.defaults.pool.queue_header_bytes == 64U * 1024U,
            "defaults.pool.queueHeaderBytes did not convert");
    require(config.defaults.pool.queue_timeout_ms == 5000,
            "defaults.pool.queueTimeout did not convert");

    require(config.maximums.worker.memory_max == "512MiB",
            "maximums.worker.memoryMax did not map");
    require(config.policy.max_worker_memory_bytes == 512U * 1024U * 1024U,
            "maximums.worker.memoryMax did not reach the policy");
    require(config.policy.max_requests_per_worker == 32,
            "maximums.request.maxInflightPerWorker did not reach the policy");
    require(config.policy.max_streaming_inflight_per_worker == 4,
            "maximums.request.maxStreamingInflightPerWorker did not reach "
            "the policy");
    require(config.policy.max_stream_idle_timeout_ms == 120000,
            "maximums.request.streamIdleTimeoutMs did not reach the policy");
    require(config.policy.max_write_timeout_ms == 60000,
            "maximums.request.writeTimeoutMs did not reach the policy");
    require(config.policy.max_queue_requests == 256,
            "maximums.pool.queueRequests did not reach the policy");
    require(config.policy.max_queue_header_bytes == 256U * 1024U,
            "maximums.pool.queueHeaderBytes did not reach the policy");
    require(config.policy.max_queue_timeout_ms == 10000,
            "maximums.pool.queueTimeout did not reach the policy");

    require(config.capacity.workers_total == 4,
            "capacity.workersTotal did not map");
    require(config.policy.max_workers == 4,
            "capacity.workersTotal did not reach the policy");
    require(config.capacity.activation_surge_workers == 2,
            "capacity.activationSurgeWorkers did not map");
    require(config.capacity.startups_concurrent == 2,
            "capacity.startupsConcurrent did not map");
    require(config.capacity.queued_requests_total == 128,
            "capacity.queuedRequestsTotal did not map");
    require(config.capacity.queued_header_bytes_total == 128U * 1024U,
            "capacity.queuedHeaderBytesTotal did not convert");
    require(config.capacity.worker_memory_commit_total == 2U * 1024U * 1024U * 1024U,
            "capacity.workerMemoryCommitTotal did not convert");

    require(config.recovery.crash_budget.max_events == 3,
            "recovery.crashBudget.maxEvents did not map");
    require(config.recovery.crash_budget.window_ms == 60000,
            "recovery.crashBudget.window did not convert");
    require(config.recovery.restart_backoff.initial_ms == 250,
            "recovery.restartBackoff.initial did not convert");
    require(config.recovery.restart_backoff.maximum_ms == 30000,
            "recovery.restartBackoff.maximum did not convert");
    require(config.recovery.restart_backoff.jitter == "1000",
            "recovery.restartBackoff.jitter did not map");
    require(config.recovery.replacements_concurrent_per_app == 1,
            "recovery.replacementsConcurrentPerApp did not map");
    require(config.recovery.active_health_interval_ms == 15000,
            "recovery.activeHealthInterval did not convert");
    require(config.recovery.active_health_failures == 2,
            "recovery.activeHealthFailures did not map");
}

void test_minimal_document_parses() {
    ParsedHostConfig config;
    require_valid_parse(
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"}
        })json",
        "minimal document", &config);
    require(config.admin_mode == 0600,
            "absent admin.mode must keep the 0600 default");
    require(config.listeners.empty(), "absent listeners must stay empty");
    require(config.policy.module_allowlist.empty(),
            "absent permissions must stay empty");
    require(!config.policy.storage_allowed && !config.policy.stdio_allowed,
            "absent permissions must not flip the allow flags");
    require(config.isolation_mode.empty(),
            "absent isolation.mode must stay empty");
    require(config.trusted_keys.empty(), "absent keys must stay empty");
    require(config.capacity.workers_total == 1,
            "absent capacity.workersTotal must keep the default");
    require(config.policy.max_workers == 1,
            "absent capacity must keep max_workers 1");
    require(config.capacity.activation_surge_workers == 0,
            "absent capacity.activationSurgeWorkers must default to 0");
}

// Binding v1 P0-1: the v2 bindingsRoot maps into the typed model, so the
// production Host actually passes it to ManagedHostOptions.
void test_bindings_root_maps() {
    ParsedHostConfig config;
    require_valid_parse(
        R"json({"apiVersion":"capsid/host-v2","applicationsRoot":"/srv/apps","stateRoot":"/var/lib/capsid","secretRootTemplate":"/var/lib/capsid/secrets/{application}","admin":{"unix":"/run/capsid/admin.sock","mode":"0600"},"bindingsRoot":"/etc/capsid/bindings"})json",
        "host v2 with bindingsRoot",
        &config);
    require(config.bindings_root == "/etc/capsid/bindings",
            "bindingsRoot did not map into the typed model");
    require_valid_parse(
        R"json({"apiVersion":"capsid/host-v1","applicationsRoot":"/srv/apps","stateRoot":"/var/lib/capsid","secretRootTemplate":"/var/lib/capsid/secrets/{application}","admin":{"unix":"/run/capsid/admin.sock","mode":"0600"}})json",
        "host v1 without bindingsRoot",
        &config);
    require(config.bindings_root.empty(),
            "v1 document produced a bindingsRoot");
}

void test_semantic_gates() {
    std::string admin_mode =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock", "mode": "0640"}
        })json";
    require_parse_error(admin_mode, "admin.mode 0640");

    std::string isolation_mode =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "isolation": {"mode": "loose"}
        })json";
    require_parse_error(isolation_mode, "isolation.mode loose");

    // capacity.workersTotal 0 is rejected by the schema itself (positive
    // integer, min 1) — the schema owns that gate; only the Host int-max
    // gate lives in the typed parse.

    std::string giant_workers =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "capacity": {"workersTotal": 4294967296}
        })json";
    require_parse_error(giant_workers, "capacity.workersTotal over int max");

    // §9.4: activationSurgeWorkers is non-negative — a negative surge
    // budget is a capacity misconfiguration, not a reasonable request.
    std::string negative_surge =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "capacity": {"workersTotal": 4,
                       "activationSurgeWorkers": -1}
        })json";
    require_parse_error(negative_surge,
                        "negative capacity.activationSurgeWorkers");

    std::string missing_root =
        R"json({
          "apiVersion": "capsid/host-v1",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"}
        })json";
    require_parse_error(missing_root, "missing applicationsRoot");

    std::string missing_template =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/app",
          "admin": {"unix": "/run/capsid/admin.sock"}
        })json";
    require_parse_error(missing_template,
                        "secretRootTemplate without {application}");
}

void test_grammar_gates() {
    std::string bad_duration =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "maximums": {"pool": {"queueTimeout": "5sX"}}
        })json";
    require_parse_error(bad_duration, "queueTimeout bad unit");

    std::string zero_duration =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "recovery": {"restartBackoff": {"initial": "0s"}}
        })json";
    require_parse_error(zero_duration, "restartBackoff.initial zero");

    std::string bad_size =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "maximums": {"worker": {"memoryMax": "512Mi"}}
        })json";
    require_parse_error(bad_size, "memoryMax bad suffix");

    std::string bad_jitter =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "recovery": {"restartBackoff": {"jitter": "10001"}}
        })json";
    require_parse_error(bad_jitter, "restartBackoff.jitter over 10000");

    std::string negative_integer =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "listeners": [{"limits": {"connections": -1}}]
        })json";
    require_parse_error(negative_integer, "negative limit integer");

    std::string bad_listener_duration =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock"},
          "listeners": [{"limits": {"headerTimeout": "5"}}]
        })json";
    require_parse_error(bad_listener_duration, "headerTimeout without unit");
}

void test_error_is_stable_and_safe() {
    ParsedHostConfig config;
    std::string error;
    std::string document =
        R"json({
          "apiVersion": "capsid/host-v1",
          "applicationsRoot": "/srv/apps",
          "stateRoot": "/var/lib/capsid",
          "secretRootTemplate": "/secrets/{application}",
          "admin": {"unix": "/run/capsid/admin.sock", "mode": "0640"}
        })json";
    require(!parse_host_config(document, &config, &error),
            "bad-mode document was accepted");
    require(error == "admin.mode must be 0600",
            "stable error text changed: '" + error + "'");
    require(error.find('/') == std::string::npos,
            "error text leaks a path: '" + error + "'");
}

}  // namespace

int main() {
    test_full_document_maps_every_field();
    test_minimal_document_parses();
    test_bindings_root_maps();
    test_semantic_gates();
    test_grammar_gates();
    test_error_is_stable_and_safe();
    std::cout << "test-host-config-model: all tests passed" << std::endl;
    return 0;
}
