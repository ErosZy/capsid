#include "host/config.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using capsid::host::ConfigDocument;
using capsid::host::ConfigErrorCode;
using capsid::host::ConfigValidationResult;
using capsid::host::kMaxConfigBytes;
using capsid::host::kMaxConfigNesting;
using capsid::host::validate_config_json;

[[noreturn]] void fail(const std::string &message) {
    std::cerr << "test-host-config: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

void require_valid(ConfigDocument document,
                   std::string_view json,
                   const char *label) {
    const ConfigValidationResult result =
        validate_config_json(document, json);
    require(result.ok, std::string(label) + " was rejected at '" +
                           result.error.path + "': " + result.error.message);
    require(result.error.code == ConfigErrorCode::kNone,
            std::string(label) + " succeeded with a non-empty error code");
    require(result.error.path.empty(),
            std::string(label) + " succeeded with an error path");
    require(result.error.message.empty(),
            std::string(label) + " succeeded with an error message");
}

void require_unknown_field(ConfigDocument document,
                           std::string_view json,
                           const char *expected_path,
                           const char *label) {
    const ConfigValidationResult result =
        validate_config_json(document, json);
    require(!result.ok, std::string(label) + " was accepted");
    require(result.error.code == ConfigErrorCode::kUnknownField,
            std::string(label) + " did not report kUnknownField");
    require(result.error.path == expected_path,
            std::string(label) + " reported path '" + result.error.path +
                "' instead of '" + expected_path + "'");
    require(!result.error.message.empty(),
            std::string(label) + " did not provide a safe diagnostic");
}

void require_error(ConfigDocument document,
                   std::string_view json,
                   ConfigErrorCode expected_code,
                   const char *expected_path,
                   const char *label) {
    const ConfigValidationResult result =
        validate_config_json(document, json);
    require(!result.ok, std::string(label) + " was accepted");
    require(result.error.code == expected_code,
            std::string(label) + " reported the wrong error code");
    require(result.error.path == expected_path,
            std::string(label) + " reported path '" + result.error.path +
                "' instead of '" + expected_path + "'");
    require(!result.error.message.empty(),
            std::string(label) + " did not provide a safe diagnostic");
}

void test_minimal_documents_are_valid() {
    require_valid(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1"})json",
        "minimal Host config");
    require_valid(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","pool":{"minReady":1,"maxWorkers":1}})json",
        "minimal App config");
}

void test_complete_v1_document_shapes_are_valid() {
    require_valid(
        ConfigDocument::kHost,
        R"json({
          "apiVersion":"capsid/host-v1",
          "applicationsRoot":"/srv/capsid/apps",
          "stateRoot":"/var/lib/capsid",
          "secretRootTemplate":"/run/capsid/secrets/{application}",
          "admin":{"unix":"/run/capsid/admin.sock","mode":"0600"},
          "listeners":[{
            "name":"public",
            "tcp":"127.0.0.1:8080",
            "publicScheme":"https",
            "routing":{"mode":"subdomain","suffix":".apps.example.com"},
            "limits":{
              "connections":4096,
              "headerBytes":"64KiB",
              "headerTimeout":"5s",
              "bodyIdleTimeout":"30s",
              "streamIdleTimeout":"60s"
            }
          }],
          "permissions":{
            "modules":["capsid:permissions","capsid:stdio"],
            "environmentNames":["APP_MODE","API_*"],
            "fsReadRoots":["/srv/capsid/data"],
            "fetchTargets":["api.internal.example.com:443"],
            "storageNamespaces":["orders-cache"],
            "stdioStreams":["stdout","stderr"]
          },
          "isolation":{
            "mode":"strict",
            "required":["no_new_privs","landlock","seccomp","user_namespace","mount_namespace"],
            "cgroupRoot":"/sys/fs/cgroup/capsid-host"
          },
          "trustedBytecodeKeys":{
            "release-2026":"/etc/capsid/bytecode-keys/release-2026.pub"
          },
          "defaults":{
            "worker":{
              "jsHeap":"64MiB","processAddressSpace":"256MiB",
              "memoryMax":"256MiB","fileDescriptors":64,"pidsMax":8
            },
            "request":{
              "timeout":"3s","maxInflightPerWorker":8,
              "maxStreamingInflightPerWorker":2
            },
            "pool":{
              "queueRequests":128,"queueHeaderBytes":"2MiB",
              "queueTimeout":"250ms"
            }
          },
          "maximums":{
            "worker":{
              "jsHeap":"256MiB","processAddressSpace":"1GiB",
              "memoryMax":"1GiB","fileDescriptors":256,"pidsMax":32
            },
            "request":{
              "timeout":"30s","maxInflightPerWorker":32,
              "maxStreamingInflightPerWorker":2
            },
            "pool":{
              "queueRequests":1024,"queueHeaderBytes":"16MiB",
              "queueTimeout":"5s"
            }
          },
          "capacity":{
            "workersTotal":128,"startupsConcurrent":4,
            "queuedRequestsTotal":4096,"queuedHeaderBytesTotal":"64MiB",
            "workerMemoryCommitTotal":"24GiB"
          },
          "recovery":{
            "crashBudget":{"maxEvents":5,"window":"60s"},
            "restartBackoff":{"initial":"250ms","maximum":"30s","jitter":"20%"},
            "replacementsConcurrentPerApp":1,
            "activeHealthInterval":"30s","activeHealthFailures":2
          }
        })json",
        "complete Host v1 shape");

    require_valid(
        ConfigDocument::kApplication,
        R"json({
          "apiVersion":"capsid/app-v1",
          "entry":"bundle.mjs",
          "permissions":{
            "modules":["capsid:env","capsid:fs","capsid:stdio"],
            "env":{
              "APP_MODE":{"value":"production"},
              "API_TOKEN":{"valueFrom":"orders-api-token"}
            },
            "fs":{"read":{"allow":["/srv/capsid/data/orders"],"deny":[]}},
            "fetch":{"allow":["orders-api.internal.example.com:443"]},
            "storage":{"namespaces":["orders-cache"]},
            "stdio":["stdout","stderr"]
          },
          "worker":{
            "jsHeap":"64MiB","processAddressSpace":"256MiB",
            "memoryMax":"256MiB","fileDescriptors":64,"pidsMax":8
          },
          "request":{
            "timeout":"3s","maxInflightPerWorker":8,
            "maxStreamingInflightPerWorker":2
          },
          "pool":{
            "minReady":4,"maxWorkers":4,"queueRequests":128,
            "queueHeaderBytes":"2MiB","queueTimeout":"250ms"
          },
          "healthCheck":{"path":"/_capsid/health","timeout":"1s"}
        })json",
        "complete App v1 shape");
}

void test_complete_v1_shapes_remain_recursively_fail_closed() {
    require_unknown_field(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","listeners":[{"name":"public","tcp":"127.0.0.1:8080","publicScheme":"http","routing":{"mode":"path"},"limits":{"connections":1,"headerBytes":"1KiB","headerTimeout":"1s","bodyIdleTimeout":"1s","streamIdleTimeout":"1s","surprise":true}}]})json",
        "/listeners/0/limits/surprise",
        "unknown listener limit");
    require_unknown_field(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","permissions":{"env":{"TOKEN":{"valueFrom":"token","path":"/tmp/token"}}},"pool":{"minReady":1,"maxWorkers":1}})json",
        "/permissions/env/TOKEN/path",
        "unknown App environment entry field");
}

void test_listener_public_authority_is_a_frozen_v1_shape() {
    require_valid(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","listeners":[{"name":"public","tcp":"127.0.0.1:8080","publicScheme":"https","publicAuthority":"api.example.com:443","routing":{"mode":"path"}}]})json",
        "path listener with fixed public authority");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","listeners":[{"publicAuthority":443}]})json",
        ConfigErrorCode::kInvalidValue,
        "/listeners/0/publicAuthority",
        "non-string public authority");
}

void test_complete_v1_collections_and_limits_are_typed() {
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","listeners":{}})json",
        ConfigErrorCode::kInvalidValue,
        "/listeners",
        "non-array Host listeners");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","listeners":["public"]})json",
        ConfigErrorCode::kInvalidValue,
        "/listeners/0",
        "non-object Host listener");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","trustedBytecodeKeys":{"release/2026~a":7}})json",
        ConfigErrorCode::kInvalidValue,
        "/trustedBytecodeKeys/release~12026~0a",
        "non-string trusted bytecode public key path");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","permissions":{"modules":["capsid:stdio",7]}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/modules/1",
        "non-string Host module allowlist item");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","capacity":{"workersTotal":0}})json",
        ConfigErrorCode::kInvalidValue,
        "/capacity/workersTotal",
        "zero Host worker capacity");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","worker":{"fileDescriptors":0},"pool":{"minReady":1,"maxWorkers":1}})json",
        ConfigErrorCode::kInvalidValue,
        "/worker/fileDescriptors",
        "zero App file descriptor limit");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","request":{"maxInflightPerWorker":0},"pool":{"minReady":1,"maxWorkers":1}})json",
        ConfigErrorCode::kInvalidValue,
        "/request/maxInflightPerWorker",
        "zero App request inflight limit");
}

std::string app_with_environment_entry(std::string_view name,
                                       std::string_view entry_json) {
    return "{\"apiVersion\":\"capsid/app-v1\",\"permissions\":{\"env\":{\"" +
           std::string(name) + "\":" + std::string(entry_json) +
           "}},\"pool\":{\"minReady\":1,\"maxWorkers\":1}}";
}

void test_environment_request_schema_is_fail_closed() {
    require_valid(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","permissions":{"environmentNames":["APP_MODE","API_*"]}})json",
        "valid Host environment allowlist");
    require_valid(
        ConfigDocument::kApplication,
        app_with_environment_entry("APP_MODE", R"json({"value":""})json"),
        "empty literal environment value");
    require_valid(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "API_TOKEN", R"json({"valueFrom":"orders-api-token"})json"),
        "secret environment request");

    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","permissions":{"environmentNames":["API-*"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/environmentNames/0",
        "invalid Host environment pattern");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry("1TOKEN", R"json({"value":"x"})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/1TOKEN",
        "environment name beginning with a digit");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry("API_*", R"json({"value":"x"})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_*",
        "wildcard App environment name");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry("API_TOKEN", "{}"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_TOKEN",
        "environment request without value source");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "API_TOKEN",
            R"json({"value":"literal","valueFrom":"orders-api-token"})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_TOKEN",
        "environment request with two value sources");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry("API_TOKEN", R"json({"valueFrom":""})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_TOKEN/valueFrom",
        "empty secret key ID");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "API_TOKEN", R"json({"valueFrom":"../orders/token"})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_TOKEN/valueFrom",
        "secret key path escape");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "API_TOKEN", R"json({"valueFrom":"orders..token"})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_TOKEN/valueFrom",
        "secret key with an empty component");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "API_TOKEN", R"json({"valueFrom":"orders$token"})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_TOKEN/valueFrom",
        "secret key with non-contract punctuation");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "API_TOKEN", R"json({"value":"prefix\u0000suffix"})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_TOKEN/value",
        "NUL literal environment value");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "API_TOKEN", R"json({"valueFrom":"token\u0000suffix"})json"),
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/API_TOKEN/valueFrom",
        "NUL secret key ID");
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            R"json(API_TOKEN\u0000shadow)json", R"json({"value":"x"})json"),
        ConfigErrorCode::kInvalidJson,
        "",
        "NUL App environment member name");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","mystery\u0000suffix":true})json",
        ConfigErrorCode::kInvalidJson,
        "",
        "NUL Host object member name");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","applicationsRoot":"/srv/apps\u0000shadow"})json",
        ConfigErrorCode::kInvalidValue,
        "/applicationsRoot",
        "NUL in a general configuration string");

    const std::string exact_limit_value(16U * 1024U, 'x');
    require_valid(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "LARGE_VALUE", "{\"value\":\"" + exact_limit_value + "\"}"),
        "literal environment value exactly at the limit");
    const std::string over_limit_value(16U * 1024U + 1U, 'x');
    require_error(
        ConfigDocument::kApplication,
        app_with_environment_entry(
            "LARGE_VALUE", "{\"value\":\"" + over_limit_value + "\"}"),
        ConfigErrorCode::kResourceLimit,
        "/permissions/env/LARGE_VALUE/value",
        "literal environment value over the limit");

    std::string too_many =
        R"json({"apiVersion":"capsid/app-v1","permissions":{"env":{)json";
    for (std::size_t i = 0; i < 257; ++i) {
        if (i != 0) {
            too_many += ',';
        }
        too_many += "\"ENV_" + std::to_string(i) + "\":{\"value\":\"x\"}";
    }
    too_many += R"json(}},"pool":{"minReady":1,"maxWorkers":1}})json";
    require_error(ConfigDocument::kApplication,
                  too_many,
                  ConfigErrorCode::kResourceLimit,
                  "/permissions/env",
                  "too many App environment entries");
}

void test_network_namespace_is_not_in_either_schema() {
    require_unknown_field(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","networkNamespace":"inherit"})json",
        "/networkNamespace",
        "Host root networkNamespace");
    require_unknown_field(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","isolation":{"networkNamespace":"inherit"}})json",
        "/isolation/networkNamespace",
        "Host isolation networkNamespace");
    require_unknown_field(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","pool":{"minReady":1,"maxWorkers":1},"networkNamespace":"inherit"})json",
        "/networkNamespace",
        "App root networkNamespace");
    require_unknown_field(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"networkNamespace":"inherit"},"pool":{"minReady":1,"maxWorkers":1}})json",
        "/worker/networkNamespace",
        "App worker networkNamespace");
}

void test_unrelated_unknown_fields_use_the_same_fail_closed_path() {
    require_unknown_field(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","mystery":true})json",
        "/mystery",
        "Host unrelated unknown field");
    require_unknown_field(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","pool":{"minReady":1,"maxWorkers":1,"elastic":true}})json",
        "/pool/elastic",
        "App pool unrelated unknown field");
}

void test_json_envelope_is_strict() {
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","apiVersion":"capsid/host-v1"})json",
        ConfigErrorCode::kDuplicateKey,
        "",
        "duplicate Host root key");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":1,"minReady":1,"maxWorkers":1}})json",
        ConfigErrorCode::kDuplicateKey,
        "",
        "duplicate nested App key");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1",})json",
        ConfigErrorCode::kInvalidJson,
        "",
        "trailing comma");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1" /* comment */})json",
        ConfigErrorCode::kInvalidJson,
        "",
        "JSON comment");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1"} trailing)json",
        ConfigErrorCode::kInvalidJson,
        "",
        "trailing JSON input");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":NaN})json",
        ConfigErrorCode::kInvalidJson,
        "",
        "non-standard NaN");
    require_error(
        ConfigDocument::kHost,
        "[]",
        ConfigErrorCode::kInvalidValue,
        "",
        "non-object Host root");
}

void test_api_versions_are_exact() {
    require_error(
        ConfigDocument::kHost,
        "{}",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "missing Host apiVersion");
    require_error(
        ConfigDocument::kApplication,
        R"json({"pool":{"minReady":1,"maxWorkers":1}})json",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "missing App apiVersion");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":7})json",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "non-string Host apiVersion");
    // Binding v1 keeps v1/v2 as the exact supported pair; v3 stays the
    // unsupported future-version example.
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v3"})json",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "unsupported Host apiVersion");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v3","pool":{"minReady":1,"maxWorkers":1}})json",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "unsupported App apiVersion");
    require_error(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/app-v1"})json",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "App apiVersion used as Host config");
}

void test_unknown_fields_precede_value_validation() {
    require_unknown_field(
        ConfigDocument::kHost,
        R"json({"apiVersion":7,"mystery":true})json",
        "/mystery",
        "Host unknown field beside invalid value");
    require_unknown_field(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":"one","maxWorkers":1,"elastic":true}})json",
        "/pool/elastic",
        "nested App unknown field beside invalid value");
}

void test_static_pool_is_explicit_positive_and_fixed_size() {
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs"})json",
        ConfigErrorCode::kInvalidValue,
        "/pool",
        "App without an explicit pool");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"maxWorkers":1}})json",
        ConfigErrorCode::kInvalidValue,
        "/pool/minReady",
        "App pool without minReady");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":1}})json",
        ConfigErrorCode::kInvalidValue,
        "/pool/maxWorkers",
        "App pool without maxWorkers");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":0,"maxWorkers":0}})json",
        ConfigErrorCode::kInvalidValue,
        "/pool/minReady",
        "zero-sized App pool");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":-1,"maxWorkers":-1}})json",
        ConfigErrorCode::kInvalidValue,
        "/pool/minReady",
        "negative App pool size");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":1,"maxWorkers":0}})json",
        ConfigErrorCode::kInvalidValue,
        "/pool/maxWorkers",
        "zero App maxWorkers");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":1.5,"maxWorkers":1}})json",
        ConfigErrorCode::kInvalidValue,
        "/pool/minReady",
        "fractional App minReady");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":1,"maxWorkers":2}})json",
        ConfigErrorCode::kInvalidValue,
        "/pool",
        "elastic App pool in static v1");
    require_error(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":2,"maxWorkers":1}})json",
        ConfigErrorCode::kInvalidValue,
        "/pool",
        "inverted App pool bounds");
    require_valid(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":4,"maxWorkers":4}})json",
        "four-worker static App pool");
}

std::string host_document_with_array_nesting(std::size_t array_count) {
    std::string document = R"json({"mystery":)json";
    document.append(array_count, '[');
    document += "null";
    document.append(array_count, ']');
    document += '}';
    return document;
}

void test_config_resource_limits_precede_json_dom_work() {
    const std::string minimal_host =
        R"json({"apiVersion":"capsid/host-v1"})json";
    require(minimal_host.size() < kMaxConfigBytes,
            "minimal Host fixture exceeds the configured byte limit");

    std::string at_byte_limit = minimal_host;
    at_byte_limit.resize(kMaxConfigBytes, ' ');
    require_valid(ConfigDocument::kHost,
                  at_byte_limit,
                  "Host config exactly at the byte limit");

    std::string over_byte_limit = at_byte_limit;
    over_byte_limit.push_back(' ');
    require_error(ConfigDocument::kHost,
                  over_byte_limit,
                  ConfigErrorCode::kResourceLimit,
                  "",
                  "Host config over the byte limit");

    std::string over_app_byte_limit =
        R"json({"apiVersion":"capsid/app-v1","pool":{"minReady":1,"maxWorkers":1}})json";
    over_app_byte_limit.resize(kMaxConfigBytes + 1, ' ');
    require_error(ConfigDocument::kApplication,
                  over_app_byte_limit,
                  ConfigErrorCode::kResourceLimit,
                  "",
                  "App config over the byte limit");

    // The root object and innermost null leaf each consume one value level.
    // The remaining levels may be arrays; reaching the declared parser limit
    // must still proceed to the ordinary unknown-field phase.
    require(kMaxConfigNesting >= 3,
            "JSON nesting limit is too small for an object member");
    const std::string at_nesting_limit =
        host_document_with_array_nesting(kMaxConfigNesting - 2);
    require_unknown_field(ConfigDocument::kHost,
                          at_nesting_limit,
                          "/mystery",
                          "Host config exactly at the JSON nesting limit");

    const std::string over_nesting_limit =
        host_document_with_array_nesting(kMaxConfigNesting - 1);
    require_error(ConfigDocument::kHost,
                  over_nesting_limit,
                  ConfigErrorCode::kResourceLimit,
                  "",
                  "Host config over the JSON nesting limit");

    std::string bracket_text(kMaxConfigNesting * 2, '[');
    bracket_text.append(kMaxConfigNesting * 2, ']');
    const std::string app_with_brackets_in_string =
        R"json({"apiVersion":"capsid/app-v1","entry":")json" +
        bracket_text +
        R"json(","pool":{"minReady":1,"maxWorkers":1}})json";
    require_valid(ConfigDocument::kApplication,
                  app_with_brackets_in_string,
                  "brackets inside an App string");
}

}  // namespace

int main() {
    test_minimal_documents_are_valid();
    test_complete_v1_document_shapes_are_valid();
    test_complete_v1_shapes_remain_recursively_fail_closed();
    test_listener_public_authority_is_a_frozen_v1_shape();
    test_complete_v1_collections_and_limits_are_typed();
    test_environment_request_schema_is_fail_closed();
    test_network_namespace_is_not_in_either_schema();
    test_unrelated_unknown_fields_use_the_same_fail_closed_path();
    test_json_envelope_is_strict();
    test_api_versions_are_exact();
    test_unknown_fields_precede_value_validation();
    test_static_pool_is_explicit_positive_and_fixed_size();
    test_config_resource_limits_precede_json_dom_work();
    return 0;
}
