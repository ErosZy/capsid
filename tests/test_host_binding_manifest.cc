// Binding manifest schema (docs/binding-technical-design.md §2.2, §4).
//
// The manifest declares the package's maximum permission: fixed sandbox
// profiles, a required module list from the build's grantable set, and
// net/fs/env/stdio resource permissions. Validation is fail-closed and
// includes the §4.1 profile-permission consistency rule. The digest is
// computed from the canonical (key-sorted, compact) serialization so key
// order in the file never changes the Generation Identity.

#include "host/config.h"
#include "host/generation_identity.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using capsid::host::ConfigErrorCode;
using capsid::host::ConfigValidationResult;
using capsid::host::compute_binding_manifest_digest;
using capsid::host::validate_binding_manifest;

[[noreturn]] void fail(const std::string &message) {
    std::cerr << "test-host-binding-manifest: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

void require_valid(std::string_view json, const char *label) {
    const ConfigValidationResult result = validate_binding_manifest(json);
    require(result.ok, std::string(label) + " was rejected at '" +
                           result.error.path + "': " + result.error.message);
    require(result.error.code == ConfigErrorCode::kNone,
            std::string(label) + " succeeded with a non-empty error code");
    require(result.error.path.empty(),
            std::string(label) + " succeeded with an error path");
    require(result.error.message.empty(),
            std::string(label) + " succeeded with an error message");
}

void require_error(std::string_view json,
                   ConfigErrorCode expected_code,
                   const char *expected_path,
                   const char *label) {
    const ConfigValidationResult result = validate_binding_manifest(json);
    require(!result.ok, std::string(label) + " was accepted");
    require(result.error.code == expected_code,
            std::string(label) + " reported the wrong error code");
    require(result.error.path == expected_path,
            std::string(label) + " reported path '" + result.error.path +
                "' instead of '" + expected_path + "'");
    require(!result.error.message.empty(),
            std::string(label) + " did not provide a safe diagnostic");
}

std::string manifest_with_modules(std::string_view modules_json) {
    return "{\"apiVersion\":\"capsid/binding-v1\",\"permissions\":{\"modules\":" +
           std::string(modules_json) + "}}";
}

void test_minimal_and_full_shapes_are_valid() {
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"]}})json",
        "minimal binding manifest");
    require_valid(
        R"json({
          "apiVersion":"capsid/binding-v1",
          "sandbox":{
            "requires":["network-client","filesystem-read"]
          },
          "permissions":{
            "modules":["capsid:internal/core","capsid:ipaddr","capsid:utils"],
            "net":{"allow":["*:27017"]},
            "fs":{
              "read":["/etc/capsid/mongo"],
              "write":[]
            },
            "env":[],
            "stdio":[]
          }
        })json",
        "design's full binding manifest");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":[]}})json",
        "manifest with an empty module list");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"net":{"allow":[]}}})json",
        "manifest with an empty net allow list");
}

void test_manifest_envelope_is_exact() {
    require_error(
        R"json({"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "manifest without apiVersion");
    require_error(
        R"json({"apiVersion":"capsid/binding-v2","permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "unsupported manifest apiVersion");
    require_error(
        R"json({"apiVersion":7,"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/apiVersion",
        "non-string manifest apiVersion");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1"})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions",
        "manifest without permissions");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/modules",
        "manifest without the module list");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":"capsid:utils"}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/modules",
        "non-array manifest module list");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"extra":true}})json",
        ConfigErrorCode::kUnknownField,
        "/permissions/extra",
        "unknown manifest permission");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"extra":true},"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kUnknownField,
        "/sandbox/extra",
        "unknown manifest sandbox field");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","mystery":true,"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kUnknownField,
        "/mystery",
        "unknown manifest root field");
}

void test_manifest_modules_are_required_and_known() {
    require_valid(
                  R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["sqlite","wasi"]},"permissions":{"modules":["capsid:internal/core","capsid:utils","capsid:assert","capsid:getopts","capsid:hashing","capsid:internal/path","capsid:ipaddr","capsid:path","capsid:readline","capsid:sqlite","capsid:uuid","capsid:wasi"]}})json",
                  "every grantable module");

    require_error(manifest_with_modules(R"json(["capsid:posix-socket"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "unsafe posix socket module");
    require_error(manifest_with_modules(R"json(["capsid:ffi"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "permanently forbidden ffi module");
    require_error(manifest_with_modules(R"json(["capsid:worker"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "permanently forbidden worker module");
    require_error(manifest_with_modules(R"json(["capsid:http-server"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "permanently forbidden http-server module");
    require_error(manifest_with_modules(R"json(["capsid:process"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "permanently forbidden process module");
    require_error(manifest_with_modules(R"json(["tjs:nope"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "unknown tjs module");
    require_error(manifest_with_modules(R"json(["tjs:utils"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "private implementation namespace");
    require_error(manifest_with_modules(R"json(["http:evil"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "http import as a module");
    require_error(manifest_with_modules(R"json(["node:x"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "node import as a module");
    require_error(manifest_with_modules(R"json(["capsid:fs"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/0",
                  "user-facade module in a binding manifest");
    require_error(manifest_with_modules(R"json(["capsid:utils",7])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/1",
                  "non-string module entry");
    require_error(manifest_with_modules(R"json(["capsid:utils","capsid:utils"])json"),
                  ConfigErrorCode::kInvalidValue,
                  "/permissions/modules/1",
                  "duplicate module entry");
}

void test_manifest_sandbox_profiles_are_fixed_and_unique() {
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client","filesystem-read","filesystem-write","filesystem-watch","sqlite","wasi"]},"permissions":{"modules":["capsid:utils"]}})json",
        "every defined sandbox profile");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":[]},"permissions":{"modules":["capsid:utils"]}})json",
        "empty sandbox requires list");

    require_error(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-server"]},"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/sandbox/requires/0",
        "unknown sandbox profile");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client","network-client"]},"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/sandbox/requires/1",
        "duplicate sandbox profile");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":"network-client"},"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/sandbox/requires",
        "non-array sandbox requires");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":7,"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/sandbox",
        "non-object sandbox block");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":[7]},"permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/sandbox/requires/0",
        "non-string sandbox profile");
}

void test_manifest_permission_shapes_are_typed() {
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client"]},"permissions":{"modules":["capsid:utils"],"net":{"allow":["127.0.0.1:6379"]}}})json",
        "manifest net permission");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["filesystem-read"]},"permissions":{"modules":["capsid:utils"],"fs":{"read":["/etc/capsid/mongo"],"write":[]},"env":["APP_MODE"],"stdio":["stdout"]}})json",
        "manifest fs/env/stdio permissions");

    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"net":{"allow":["noport"]}}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/net/allow/0",
        "invalid manifest net target");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"net":{"deny":[]}}})json",
        ConfigErrorCode::kUnknownField,
        "/permissions/net/deny",
        "manifest net deny field");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"net":[]}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/net",
        "non-object manifest net block");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"fs":{"allow":[]}}})json",
        ConfigErrorCode::kUnknownField,
        "/permissions/fs/allow",
        "manifest fs allow field");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"fs":{"read":[7]}}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/fs/read/0",
        "non-string manifest fs read entry");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"env":["1BAD"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/env/0",
        "invalid manifest environment name");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"stdio":[7]}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/stdio/0",
        "non-string manifest stdio entry");
}

void test_manifest_profile_permission_consistency() {
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"net":{"allow":["*:27017"]}}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/net/allow",
        "net permission without the network-client profile");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client"]},"permissions":{"modules":["capsid:utils"],"net":{"allow":["*:27017"]}}})json",
        "net permission with the network-client profile");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"fs":{"read":["/etc/capsid/mongo"],"write":[]}}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/fs/read",
        "fs read permission without the filesystem-read profile");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["filesystem-read"]},"permissions":{"modules":["capsid:utils"],"fs":{"read":[],"write":["/var/lib/capsid/mongo"]}}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/fs/write",
        "fs write permission without the filesystem-write profile");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["filesystem-read","filesystem-write"]},"permissions":{"modules":["capsid:utils"],"fs":{"read":["/etc/capsid/mongo"],"write":["/var/lib/capsid/mongo"]}}})json",
        "fs permissions with matching profiles");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"],"fs":{"read":[],"write":[]},"net":{"allow":[]}}})json",
        "empty resource permissions need no profiles");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:sqlite"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/modules",
        "sqlite module without the sqlite profile");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["sqlite"]},"permissions":{"modules":["capsid:sqlite"]}})json",
        "sqlite module with the sqlite profile");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:wasi"]}})json",
        ConfigErrorCode::kInvalidValue,
        "/permissions/modules",
        "wasi module without the wasi profile");
    require_valid(
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["wasi"]},"permissions":{"modules":["capsid:wasi"]}})json",
        "wasi module with the wasi profile");
}

void test_manifest_json_envelope_is_strict() {
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"]}})json",
        ConfigErrorCode::kDuplicateKey,
        "",
        "duplicate manifest key");
    require_error(
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"]},})json",
        ConfigErrorCode::kInvalidJson,
        "",
        "manifest trailing comma");

    constexpr std::size_t kLimit = 1024U * 1024U;
    std::string over_limit =
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["capsid:utils"]}})json";
    over_limit.resize(kLimit + 1, ' ');
    require_error(over_limit,
                  ConfigErrorCode::kResourceLimit,
                  "",
                  "manifest over the byte limit");
}

void test_manifest_digest_is_canonical_and_deterministic() {
    const std::string first =
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client","sqlite"]},"permissions":{"modules":["capsid:utils","capsid:sqlite"],"net":{"allow":["*:27017"]}}})json";
    const std::string reordered =
        R"json({"permissions":{"net":{"allow":["*:27017"]},"modules":["capsid:utils","capsid:sqlite"]},"sandbox":{"requires":["network-client","sqlite"]},"apiVersion":"capsid/binding-v1"})json";
    const std::string different =
        R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client"]},"permissions":{"modules":["capsid:utils"],"net":{"allow":["*:27017"]}}})json";

    const std::string first_digest =
        compute_binding_manifest_digest(first);
    const std::string reordered_digest =
        compute_binding_manifest_digest(reordered);
    const std::string first_again =
        compute_binding_manifest_digest(first);
    const std::string different_digest =
        compute_binding_manifest_digest(different);

    require(first_digest.size() == 64 + std::string("sha256:").size(),
            "manifest digest is not a sha256 hex digest");
    require(first_digest.rfind("sha256:", 0) == 0,
            "manifest digest does not carry the sha256 prefix");
    require(first_digest == reordered_digest,
            "manifest digest depends on JSON key order");
    require(first_digest == first_again,
            "manifest digest is not deterministic");
    require(first_digest != different_digest,
            "manifest digest does not change with the content");
    require(compute_binding_manifest_digest("not json").empty(),
            "invalid manifest digest is not empty");
}

}  // namespace

int main() {
    test_minimal_and_full_shapes_are_valid();
    test_manifest_envelope_is_exact();
    test_manifest_modules_are_required_and_known();
    test_manifest_sandbox_profiles_are_fixed_and_unique();
    test_manifest_permission_shapes_are_typed();
    test_manifest_profile_permission_consistency();
    test_manifest_json_envelope_is_strict();
    test_manifest_digest_is_canonical_and_deterministic();
    return 0;
}
