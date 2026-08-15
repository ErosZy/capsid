// Binding v1 configuration schema (docs/binding-technical-design.md §2).
//
// The v2 apiVersion values enable Binding: capsid/host-v2 adds the optional
// bindingsRoot, capsid/app-v2 adds the bindings map keyed by the Binding ID.
// The v1 documents keep their frozen shape, and every alias-style field
// (provider/alias/instance) stays strictly unknown. All v1 error paths in
// test_host_config.cc are untouched except the two apiVersion-exactness
// cases that used v2 as the "unsupported" example.

#include "host/config.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using capsid::host::ConfigDocument;
using capsid::host::ConfigErrorCode;
using capsid::host::ConfigValidationResult;
using capsid::host::validate_config_json;

[[noreturn]] void fail(const std::string &message) {
    std::cerr << "test-host-binding-config: " << message << std::endl;
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

std::string app_with_binding(std::string_view bindings_json) {
    return "{\"apiVersion\":\"capsid/app-v2\",\"bindings\":" +
           std::string(bindings_json) +
           ",\"pool\":{\"minReady\":1,\"maxWorkers\":1}}";
}

std::string app_with_binding_entry(std::string_view id,
                                   std::string_view entry_json) {
    return app_with_binding(
        "{\"" + std::string(id) + "\":" + std::string(entry_json) + "}");
}

std::string app_with_binding_net(std::string_view target) {
    return app_with_binding_entry(
        "mongo",
        R"json({"permissions":{"net":{"allow":[")json" +
            std::string(target) + R"json("]}}})json");
}

void test_v2_minimal_documents_are_valid() {
    require_valid(ConfigDocument::kHost,
                  R"json({"apiVersion":"capsid/host-v2"})json",
                  "minimal Host v2 config");
    require_valid(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v2","bindingsRoot":"/etc/capsid/bindings"})json",
        "Host v2 config with a bindingsRoot");
    require_valid(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v2","entry":"bundle.mjs","pool":{"minReady":1,"maxWorkers":1}})json",
        "minimal App v2 config without bindings");
    require_valid(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v2","pool":{"minReady":1,"maxWorkers":1},"bindings":{}})json",
        "App v2 config with an empty bindings map");
}

void test_v1_documents_reject_binding_fields() {
    require_unknown_field(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v1","bindingsRoot":"/etc/capsid/bindings"})json",
        "/bindingsRoot",
        "Host v1 bindingsRoot");
    require_unknown_field(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v1","bindings":{},"pool":{"minReady":1,"maxWorkers":1}})json",
        "/bindings",
        "App v1 bindings");
    require_unknown_field(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v2","bindings":{}})json",
        "/bindings",
        "Host v2 app-only bindings field");
    require_unknown_field(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v2","bindingsRoot":"/x","pool":{"minReady":1,"maxWorkers":1}})json",
        "/bindingsRoot",
        "App v2 host-only bindingsRoot field");
}

void test_v2_document_shapes_are_valid() {
    require_valid(
        ConfigDocument::kApplication,
        R"json({
          "apiVersion":"capsid/app-v2",
          "entry":"bundle.mjs",
          "bindings":{
            "mongo":{
              "permissions":{
                "net":{"allow":["127.0.0.1:27017"]},
                "fs":{
                  "read":["/etc/capsid/mongo/ca.pem"],
                  "write":[]
                },
                "env":[],
                "stdio":[]
              },
              "config":{"database":"orders","tls":true},
              "secrets":{"password":{"valueFrom":"mongo-password"}}
            }
          },
          "pool":{"minReady":1,"maxWorkers":1}
        })json",
        "App v2 with the design's mongo binding");
    require_valid(
        ConfigDocument::kApplication,
        R"json({"apiVersion":"capsid/app-v2","pool":{"minReady":1,"maxWorkers":1},"bindings":{"redis":{}}})json",
        "App v2 with an all-deny binding entry");
}

void test_binding_alias_fields_are_strictly_rejected() {
    require_unknown_field(
        ConfigDocument::kApplication,
        app_with_binding_entry("mongo", R"json({"provider":"mongo"})json"),
        "/bindings/mongo/provider",
        "binding provider alias");
    require_unknown_field(
        ConfigDocument::kApplication,
        app_with_binding_entry("mongo", R"json({"alias":"db"})json"),
        "/bindings/mongo/alias",
        "binding alias");
    require_unknown_field(
        ConfigDocument::kApplication,
        app_with_binding_entry("mongo", R"json({"instance":"orders"})json"),
        "/bindings/mongo/instance",
        "binding instance");
    require_unknown_field(
        ConfigDocument::kApplication,
        app_with_binding_entry(
            "mongo", R"json({"permissions":{"provider":"mongo"}})json"),
        "/bindings/mongo/permissions/provider",
        "binding permissions provider alias");
}

void test_binding_id_grammar_is_exact() {
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo", "{}"),
                  "plain binding id");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_entry("a", "{}"),
                  "single-character binding id");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_entry("redis-cache", "{}"),
                  "hyphenated binding id");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_entry("m1-2-3", "{}"),
                  "alphanumeric binding id");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_entry(std::string(63, 'a'), "{}"),
                  "63-character binding id");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo-", "{}"),
                  "trailing-hyphen binding id per the frozen grammar");

    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("Mongo", "{}"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/Mongo",
                  "uppercase binding id");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo_1", "{}"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo_1",
                  "underscore binding id");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo.1", "{}"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo.1",
                  "dotted binding id");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("1mongo", "{}"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/1mongo",
                  "digit-leading binding id");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("-mongo", "{}"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/-mongo",
                  "hyphen-leading binding id");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("", "{}"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/",
                  "empty binding id");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo$", "{}"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo$",
                  "punctuated binding id");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(std::string(64, 'a'), "{}"),
                  ConfigErrorCode::kInvalidValue,
                  std::string("/bindings/" + std::string(64, 'a')).c_str(),
                  "64-character binding id");
}

void test_binding_secrets_are_valuefrom_only() {
    require_valid(
        ConfigDocument::kApplication,
        app_with_binding_entry(
            "mongo",
            R"json({"secrets":{"password":{"valueFrom":"mongo-password"}}})json"),
        "secret reference");
    require_unknown_field(
        ConfigDocument::kApplication,
        app_with_binding_entry(
            "mongo", R"json({"secrets":{"password":{"value":"inline"}}})json"),
        "/bindings/mongo/secrets/password/value",
        "inline binding secret");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo",
                      R"json({"secrets":{"password":{"valueFrom":""}}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/secrets/password/valueFrom",
                  "empty secret key id");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo",
                      R"json({"secrets":{"password":{"valueFrom":"../x"}}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/secrets/password/valueFrom",
                  "secret key path escape");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo",
                      R"json({"secrets":{"password":{}}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/secrets/password/valueFrom",
                  "secret entry without valueFrom");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo", R"json({"secrets":{"bad$key":{"valueFrom":"x"}}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/secrets/bad$key",
                  "secret entry with an invalid key");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo", R"json({"secrets":{"password":7}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/secrets/password",
                  "non-object secret entry");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo", R"json({"secrets":[]})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/secrets",
                  "non-object secrets map");
}

void test_binding_config_is_opaque_and_bounded() {
    require_valid(
        ConfigDocument::kApplication,
        app_with_binding_entry(
            "mongo",
            R"json({"config":{"database":"orders","tls":true,"nested":{"a":[1,2,{"b":null}]}}})json"),
        "opaque nested binding config");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo", R"json({"config":{}})json"),
                  "empty binding config");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo", R"json({"config":42})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/config",
                  "non-object binding config");

    // The config member is bounded by its compact serialized byte size
    // (256 KiB); the pad string below makes the member exactly at and
    // just over the limit. The config value {"pad":""} serializes to
    // 10 bytes.
    constexpr std::size_t kLimit = 256U * 1024U;
    const std::string at_limit =
        "{\"config\":{\"pad\":\"" + std::string(kLimit - 10, 'x') + "\"}}";
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo", at_limit),
                  "binding config exactly at the size limit");
    const std::string over_limit =
        "{\"config\":{\"pad\":\"" + std::string(kLimit - 9, 'x') + "\"}}";
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo", over_limit),
                  ConfigErrorCode::kResourceLimit,
                  "/bindings/mongo/config",
                  "binding config over the size limit");
}

void test_binding_net_target_grammar_is_exact() {
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("db.example.com:27017"),
                  "hostname net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("*.internal.example.com:443"),
                  "wildcard hostname net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("127.0.0.1:6379"),
                  "IPv4 net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("10.0.0.0/8:3306"),
                  "IPv4 CIDR net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("127.0.0.1/24:80"),
                  "IPv4 CIDR with host bits");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("[::1]:6379"),
                  "IPv6 net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("[2001:db8::/32]:443"),
                  "IPv6 CIDR net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("[::1/128]:80"),
                  "IPv6 full-prefix net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("*:27017"),
                  "any-host net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("localhost:1234"),
                  "single-label net target");
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net("255.255.255.255:65535"),
                  "maximum port net target");

    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("example.com"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "net target without a port");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("example.com:0"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "zero port");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("example.com:65536"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "port above 65535");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("example.com:abc"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "non-numeric port");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("example.com:"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "empty port");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net(":80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "empty host");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("example.com:80:90"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "port range");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("mongo://host:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "scheme net target");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("1.2.3.999:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "IPv4-looking host with an invalid octet");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("10.0.0.0/33:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "IPv4 prefix above 32");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("[::1]:80:90"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "bracketed IPv6 with a range");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("[::1]80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "bracketed IPv6 without the port separator");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("[not-v6]:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "bracketed non-IPv6 host");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("[::1/129]:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "IPv6 prefix above 128");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("*:0"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "any-host with port zero");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("host:*"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "wildcard port");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("*.bad..com:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "wildcard host with an empty label");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("-lead.example.com:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "hostname with a hyphen-leading label");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("trail-.example.com:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "hostname with a hyphen-trailing label");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net("exa mple.com:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "hostname with a space");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net(std::string(64, 'a') + ".com:80"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "hostname with a 64-character label");

    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net(std::string(63, 'a') + ".com:80"),
                  "hostname with a 63-character label");
    const std::string at_hostname_limit =
        std::string(63, 'a') + "." + std::string(63, 'b') + "." +
        std::string(63, 'c') + "." + std::string(61, 'd') + ":80";
    require_valid(ConfigDocument::kApplication,
                  app_with_binding_net(at_hostname_limit),
                  "253-byte hostname");
    const std::string over_hostname_limit =
        std::string(63, 'a') + "." + std::string(63, 'b') + "." +
        std::string(63, 'c') + "." + std::string(62, 'd') + ":80";
    require_error(ConfigDocument::kApplication,
                  app_with_binding_net(over_hostname_limit),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/net/allow/0",
                  "254-byte hostname");
}

void test_binding_permission_shapes_are_typed() {
    require_valid(
        ConfigDocument::kApplication,
        app_with_binding_entry(
            "mongo",
            R"json({"permissions":{"env":["APP_MODE"],"stdio":["stdout","stderr"],"fs":{"read":["/etc/capsid/mongo/ca.pem"],"write":[]}}})json"),
        "binding fs/env/stdio permissions");
    require_unknown_field(
        ConfigDocument::kApplication,
        app_with_binding_entry("mongo", R"json({"permissions":{"module":[]}})json"),
        "/bindings/mongo/permissions/module",
        "unknown binding permission");
    require_unknown_field(
        ConfigDocument::kApplication,
        app_with_binding_entry(
            "mongo", R"json({"permissions":{"net":{"deny":[]}}})json"),
        "/bindings/mongo/permissions/net/deny",
        "binding net deny field");
    require_unknown_field(
        ConfigDocument::kApplication,
        app_with_binding_entry(
            "mongo", R"json({"permissions":{"fs":{"allow":[]}}})json"),
        "/bindings/mongo/permissions/fs/allow",
        "binding fs allow field");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo", R"json({"permissions":{"fs":{"read":"x"}}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/fs/read",
                  "non-array binding fs read");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo", R"json({"permissions":{"fs":{"write":[7]}}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/fs/write/0",
                  "non-string binding fs write entry");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo", R"json({"permissions":{"env":["1BAD"]}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/env/0",
                  "invalid binding environment name");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry(
                      "mongo", R"json({"permissions":{"stdio":[7]}})json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo/permissions/stdio/0",
                  "non-string binding stdio entry");
    require_error(ConfigDocument::kApplication,
                  app_with_binding(R"json([])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings",
                  "non-object bindings map");
    require_error(ConfigDocument::kApplication,
                  app_with_binding_entry("mongo", "7"),
                  ConfigErrorCode::kInvalidValue,
                  "/bindings/mongo",
                  "non-object binding entry");
}

void test_v2_api_version_matrix_is_exact() {
    require_error(ConfigDocument::kApplication,
                  R"json({"apiVersion":"capsid/app-v3","pool":{"minReady":1,"maxWorkers":1}})json",
                  ConfigErrorCode::kInvalidValue,
                  "/apiVersion",
                  "unsupported App v3 apiVersion");
    require_error(ConfigDocument::kHost,
                  R"json({"apiVersion":"capsid/host-v3"})json",
                  ConfigErrorCode::kInvalidValue,
                  "/apiVersion",
                  "unsupported Host v3 apiVersion");
    require_error(ConfigDocument::kHost,
                  R"json({"apiVersion":"capsid/app-v2"})json",
                  ConfigErrorCode::kInvalidValue,
                  "/apiVersion",
                  "App v2 apiVersion used as Host config");
    require_error(ConfigDocument::kApplication,
                  R"json({"apiVersion":"capsid/host-v2","pool":{"minReady":1,"maxWorkers":1}})json",
                  ConfigErrorCode::kInvalidValue,
                  "/apiVersion",
                  "Host v2 apiVersion used as App config");
    require_error(ConfigDocument::kHost,
                  R"json({"apiVersion":"capsid/host-v2","bindingsRoot":7})json",
                  ConfigErrorCode::kInvalidValue,
                  "/bindingsRoot",
                  "non-string Host bindingsRoot");
    require_error(ConfigDocument::kApplication,
                  R"json({"apiVersion":"capsid/app-v2","bindings":{"mongo":{}}})json",
                  ConfigErrorCode::kInvalidValue,
                  "/pool",
                  "App v2 without an explicit pool");
    require_unknown_field(
        ConfigDocument::kHost,
        R"json({"apiVersion":"capsid/host-v2","mystery":true})json",
        "/mystery",
        "Host v2 unrelated unknown field");
}

}  // namespace

int main() {
    test_v2_minimal_documents_are_valid();
    test_v1_documents_reject_binding_fields();
    test_v2_document_shapes_are_valid();
    test_binding_alias_fields_are_strictly_rejected();
    test_binding_id_grammar_is_exact();
    test_binding_secrets_are_valuefrom_only();
    test_binding_config_is_opaque_and_bounded();
    test_binding_net_target_grammar_is_exact();
    test_binding_permission_shapes_are_typed();
    test_v2_api_version_matrix_is_exact();
    return 0;
}
