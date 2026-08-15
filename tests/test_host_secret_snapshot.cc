#include "host/generation_identity.h"
#include "host/secret_snapshot.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using capsid::host::EnvironmentRequest;
using capsid::host::EnvironmentValueSource;
using capsid::host::GenerationIdentityInput;
using capsid::host::ResolvedSecret;
using capsid::host::SecretSnapshotCompileInput;
using capsid::host::SecretSnapshotCompileResult;
using capsid::host::SecretSnapshotErrorCode;
using capsid::host::SelectedArtifactKind;
using capsid::host::compile_secret_snapshot;
using capsid::host::compute_generation_digest;
using capsid::host::kMaxEnvironmentEntries;
using capsid::host::kMaxEnvironmentSnapshotBytes;
using capsid::host::kMaxEnvironmentValueBytes;

constexpr std::string_view kSecretCanary =
    "CAPSID_SECRET_CANARY_7f3f190b_do_not_log";

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-secret-snapshot: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::string sha256_id(std::span<const std::uint8_t> bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), digest, &digest_size,
                       EVP_sha256(), nullptr) == 1 &&
                digest_size == 32,
            "OpenSSL SHA-256 fixture failure");
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result = "sha256:";
    for (unsigned int i = 0; i < digest_size; ++i) {
        result.push_back(kHex[digest[i] >> 4]);
        result.push_back(kHex[digest[i] & 0x0f]);
    }
    return result;
}

void append_length_prefixed(std::vector<std::uint8_t>& output,
                            std::string_view value) {
    require(value.size() <= UINT32_MAX, "fixture field exceeds uint32");
    const std::uint32_t size = static_cast<std::uint32_t>(value.size());
    output.push_back(static_cast<std::uint8_t>(size >> 24));
    output.push_back(static_cast<std::uint8_t>(size >> 16));
    output.push_back(static_cast<std::uint8_t>(size >> 8));
    output.push_back(static_cast<std::uint8_t>(size));
    output.insert(output.end(), value.begin(), value.end());
}

std::string expected_secret_revision(
    std::string_view application,
    std::span<const std::array<std::string_view, 3>> entries) {
    static constexpr char kDomain[] = "capsid-secret-revision-v1\0";
    std::vector<std::uint8_t> message(
        reinterpret_cast<const std::uint8_t*>(kDomain),
        reinterpret_cast<const std::uint8_t*>(kDomain) + sizeof(kDomain) - 1);
    append_length_prefixed(message, application);
    for (const auto& entry : entries) {
        append_length_prefixed(message, entry[0]);
        append_length_prefixed(message, entry[1]);
        append_length_prefixed(message, entry[2]);
    }
    return sha256_id(message);
}

std::string expected_generation_digest(const GenerationIdentityInput& input) {
    static constexpr char kDomain[] = "capsid-generation-v1\0";
    std::vector<std::uint8_t> message(
        reinterpret_cast<const std::uint8_t*>(kDomain),
        reinterpret_cast<const std::uint8_t*>(kDomain) + sizeof(kDomain) - 1);
    const std::string_view artifact =
        input.selected_artifact == SelectedArtifactKind::kSource
            ? "source"
            : "trusted-bytecode";
    const std::string_view fields[] = {
        input.application_id,
        input.source_digest,
        input.bytecode_attestation_digest,
        artifact,
        input.normalized_app_config_digest,
        input.effective_policy_digest,
        input.effective_resource_digest,
        input.host_config_digest,
        input.secret_revision,
        input.runtime_compatibility_id,
        input.binding_set_digest,
    };
    for (const std::string_view field : fields) {
        append_length_prefixed(message, field);
    }
    return sha256_id(message);
}

struct Fixture {
    std::string application = "orders";
    std::string literal_value = "production\n";
    std::string secret_value = std::string(kSecretCanary) + "\n";
    std::string secret_revision = "file-v1:11:22:41:1700000000";
    std::array<std::string_view, 2> host_names{"APP_MODE", "API_*"};
    std::array<EnvironmentRequest, 2> requests;
    std::array<ResolvedSecret, 1> secrets;
    SecretSnapshotCompileInput input;

    Fixture() {
        // Deliberately not sorted: successful output is canonical by name.
        requests[0] = EnvironmentRequest{
            "APP_MODE", std::string_view(literal_value), std::nullopt};
        requests[1] = EnvironmentRequest{
            "API_TOKEN", std::nullopt, "orders-api-token"};
        secrets[0] = ResolvedSecret{
            "orders-api-token", secret_value, secret_revision};
        input.application_id = application;
        input.host_allows_env_module = true;
        input.app_requests_env_module = true;
        input.host_environment_names = host_names;
        input.requests = requests;
        input.resolved_secrets = secrets;
    }
};

void require_error(const SecretSnapshotCompileResult& result,
                   SecretSnapshotErrorCode code,
                   std::string_view path,
                   std::string_view label) {
    require(!result.ok, std::string(label) + " was accepted");
    require(result.error.code == code,
            std::string(label) + " returned the wrong error code");
    require(result.error.path == path,
            std::string(label) + " reported path '" + result.error.path +
                "' instead of '" + std::string(path) + "'");
    require(!result.error.message.empty(),
            std::string(label) + " returned no safe diagnostic");
    require(result.error.path.find(kSecretCanary) == std::string::npos &&
                result.error.message.find(kSecretCanary) == std::string::npos,
            std::string(label) + " leaked the secret canary in diagnostics");
    require(result.snapshot.entries().empty() &&
                result.snapshot.metadata().empty() &&
                result.snapshot.secret_revision().empty() &&
                result.snapshot.effective_environment_json().empty(),
            std::string(label) + " partially published a failed snapshot");
}

void test_minimal_snapshot_is_owned_canonical_and_redacted() {
    Fixture fixture;
    const SecretSnapshotCompileResult result =
        compile_secret_snapshot(fixture.input);
    require(result.ok, "valid environment snapshot was rejected: " +
                           result.error.message);
    require(result.error.code == SecretSnapshotErrorCode::kNone &&
                result.error.path.empty() && result.error.message.empty(),
            "valid snapshot returned a stale error");

    const auto& entries = result.snapshot.entries();
    require(entries.size() == 2 && entries[0].name == "API_TOKEN" &&
                entries[0].value == fixture.secret_value &&
                entries[1].name == "APP_MODE" &&
                entries[1].value == fixture.literal_value,
            "snapshot entries were not sorted or copied exactly");

    const auto& metadata = result.snapshot.metadata();
    require(metadata.size() == 2 && metadata[0].name == "API_TOKEN" &&
                metadata[0].source == EnvironmentValueSource::kSecret &&
                metadata[0].secret_key_id == "orders-api-token" &&
                metadata[0].opaque_revision == fixture.secret_revision &&
                metadata[1].name == "APP_MODE" &&
                metadata[1].source == EnvironmentValueSource::kLiteral &&
                metadata[1].secret_key_id.empty() &&
                metadata[1].opaque_revision.empty(),
            "snapshot metadata is incomplete or contains the wrong source");

    const std::string expected_json =
        "{\"environment\":["
        "{\"name\":\"API_TOKEN\",\"source\":\"secret\","
        "\"keyId\":\"orders-api-token\","
        "\"revision\":\"file-v1:11:22:41:1700000000\"},"
        "{\"name\":\"APP_MODE\",\"source\":\"literal\"}]}";
    require(result.snapshot.effective_environment_json() == expected_json,
            "effective environment JSON is not canonical");
    require(result.snapshot.effective_environment_json().find(kSecretCanary) ==
                std::string::npos,
            "effective environment JSON leaked the secret canary");

    const std::array<std::array<std::string_view, 3>, 1> revision_entries{{
        {"API_TOKEN", "orders-api-token", fixture.secret_revision},
    }};
    require(result.snapshot.secret_revision() ==
                expected_secret_revision(fixture.application, revision_entries),
            "secret revision does not cover canonical safe metadata");
    require(result.snapshot.secret_revision().find(kSecretCanary) ==
                std::string::npos,
            "secret revision exposed the secret canary");

    const std::vector<capsid_env_entry> runtime_entries =
        result.snapshot.runtime_entries();
    require(runtime_entries.size() == 2,
            "Runtime descriptor count differs from the snapshot");
    for (std::size_t i = 0; i < runtime_entries.size(); ++i) {
        require(runtime_entries[i].struct_size == sizeof(capsid_env_entry) &&
                    runtime_entries[i].reserved == 0 &&
                    std::strcmp(runtime_entries[i].name,
                                entries[i].name.c_str()) == 0 &&
                    std::strcmp(runtime_entries[i].value,
                                entries[i].value.c_str()) == 0,
                "Runtime environment descriptor drifted from owned storage");
    }

    fixture.literal_value.assign("MUTATED");
    fixture.secret_value.assign("MUTATED");
    fixture.secret_revision.assign("MUTATED");
    fixture.requests[0].name = "MUTATED";
    require(entries[0].value == std::string(kSecretCanary) + "\n" &&
                entries[1].value == "production\n" &&
                metadata[0].opaque_revision ==
                    "file-v1:11:22:41:1700000000",
            "snapshot retained caller-owned storage");
}

void test_empty_snapshot_does_not_require_environment_capability() {
    SecretSnapshotCompileInput input;
    input.application_id = "orders";
    const SecretSnapshotCompileResult result = compile_secret_snapshot(input);
    require(result.ok && result.error.code == SecretSnapshotErrorCode::kNone,
            "App without env requests required capsid:env capability");
    require(result.snapshot.entries().empty() &&
                result.snapshot.metadata().empty() &&
                result.snapshot.runtime_entries().empty() &&
                result.snapshot.effective_environment_json() ==
                    "{\"environment\":[]}",
            "empty environment snapshot is not canonical");
    const std::span<const std::array<std::string_view, 3>> no_entries;
    require(result.snapshot.secret_revision() ==
                expected_secret_revision("orders", no_entries),
            "empty environment snapshot has the wrong revision");

    const std::array<ResolvedSecret, 1> unexpected_material{{
        {"unused-key", "unused-value", "file-v1:1:1:1:1"},
    }};
    input.resolved_secrets = unexpected_material;
    require_error(compile_secret_snapshot(input),
                  SecretSnapshotErrorCode::kUnexpectedResolvedSecret,
                  "/resolvedSecrets/unused-key",
                  "resolved secret for an empty environment request");

    const std::array<ResolvedSecret, 1> invalid_material{{
        {"unsafe\nkey", "unused-value", "file-v1:1:1:1:1"},
    }};
    input.resolved_secrets = invalid_material;
    require_error(compile_secret_snapshot(input),
                  SecretSnapshotErrorCode::kInvalidSecretKey,
                  "/resolvedSecrets/0/keyId",
                  "invalid resolved key for an empty environment request");
}

void test_safe_secret_metadata_punctuation_is_accepted() {
    Fixture fixture;
    fixture.requests[1].value_from = "orders.api_token-1";
    fixture.secrets[0].key_id = "orders.api_token-1";
    fixture.secret_revision = "file.v1:node_1@2+3-4";
    fixture.secrets[0].opaque_revision = fixture.secret_revision;
    const SecretSnapshotCompileResult result =
        compile_secret_snapshot(fixture.input);
    require(result.ok,
            "contract-safe secret metadata punctuation was rejected: " +
                result.error.message);
}

void test_request_order_does_not_change_snapshot_identity() {
    Fixture first;
    Fixture second;
    std::swap(second.requests[0], second.requests[1]);
    second.input.requests = second.requests;
    const SecretSnapshotCompileResult left =
        compile_secret_snapshot(first.input);
    const SecretSnapshotCompileResult right =
        compile_secret_snapshot(second.input);
    require(left.ok && right.ok &&
                left.snapshot.secret_revision() ==
                    right.snapshot.secret_revision() &&
                left.snapshot.effective_environment_json() ==
                    right.snapshot.effective_environment_json(),
            "request order changed canonical snapshot metadata");
}

void test_policy_and_provider_fail_closed_without_leaking_values() {
    Fixture denied_module;
    denied_module.input.app_requests_env_module = false;
    require_error(compile_secret_snapshot(denied_module.input),
                  SecretSnapshotErrorCode::kModuleDenied,
                  "/permissions/modules", "missing App capsid:env module");

    Fixture denied_name;
    const std::array<std::string_view, 1> literal_only{"APP_MODE"};
    denied_name.input.host_environment_names = literal_only;
    require_error(compile_secret_snapshot(denied_name.input),
                  SecretSnapshotErrorCode::kPermissionDenied,
                  "/permissions/env/API_TOKEN", "Host-denied environment name");

    Fixture invalid_pattern;
    const std::array<std::string_view, 1> patterns{"API-*"};
    invalid_pattern.input.host_environment_names = patterns;
    require_error(compile_secret_snapshot(invalid_pattern.input),
                  SecretSnapshotErrorCode::kInvalidEnvironmentPattern,
                  "/host/environmentNames/0", "invalid Host env pattern");

    Fixture duplicate_name;
    duplicate_name.requests[1].name = "APP_MODE";
    require_error(compile_secret_snapshot(duplicate_name.input),
                  SecretSnapshotErrorCode::kDuplicateEnvironmentName,
                  "/permissions/env/APP_MODE", "duplicate environment name");

    Fixture invalid_name;
    invalid_name.requests[1].name = "API-*";
    require_error(compile_secret_snapshot(invalid_name.input),
                  SecretSnapshotErrorCode::kInvalidEnvironmentName,
                  "/permissions/env/API-*", "invalid environment name");

    Fixture both_sources;
    both_sources.requests[0].value_from = "orders-api-token";
    require_error(compile_secret_snapshot(both_sources.input),
                  SecretSnapshotErrorCode::kInvalidEnvironmentRequest,
                  "/permissions/env/APP_MODE", "literal and valueFrom together");

    Fixture neither_source;
    neither_source.requests[0].value.reset();
    require_error(compile_secret_snapshot(neither_source.input),
                  SecretSnapshotErrorCode::kInvalidEnvironmentRequest,
                  "/permissions/env/APP_MODE", "environment entry without source");

    Fixture invalid_key;
    invalid_key.requests[1].value_from = "../orders-api-token";
    invalid_key.secrets[0].key_id = "../orders-api-token";
    require_error(compile_secret_snapshot(invalid_key.input),
                  SecretSnapshotErrorCode::kInvalidSecretKey,
                  "/permissions/env/API_TOKEN/valueFrom", "secret path escape");

    Fixture punctuation_key;
    punctuation_key.requests[1].value_from = "orders$token";
    punctuation_key.secrets[0].key_id = "orders$token";
    require_error(compile_secret_snapshot(punctuation_key.input),
                  SecretSnapshotErrorCode::kInvalidSecretKey,
                  "/permissions/env/API_TOKEN/valueFrom",
                  "secret key with non-contract punctuation");

    Fixture invalid_provider_key;
    invalid_provider_key.secrets[0].key_id = "unsafe\nkey";
    require_error(compile_secret_snapshot(invalid_provider_key.input),
                  SecretSnapshotErrorCode::kInvalidSecretKey,
                  "/resolvedSecrets/0/keyId",
                  "invalid provider secret key");

    Fixture missing_secret;
    missing_secret.input.resolved_secrets = {};
    require_error(compile_secret_snapshot(missing_secret.input),
                  SecretSnapshotErrorCode::kMissingResolvedSecret,
                  "/permissions/env/API_TOKEN/valueFrom", "missing secret");

    Fixture duplicate_secret;
    const std::array<ResolvedSecret, 2> duplicate_material{{
        duplicate_secret.secrets[0], duplicate_secret.secrets[0]}};
    duplicate_secret.input.resolved_secrets = duplicate_material;
    require_error(compile_secret_snapshot(duplicate_secret.input),
                  SecretSnapshotErrorCode::kDuplicateResolvedSecret,
                  "/resolvedSecrets/1", "duplicate resolved secret");

    Fixture unexpected_secret;
    const std::array<ResolvedSecret, 2> extra_material{{
        unexpected_secret.secrets[0],
        {"unused-key", "unused-value", "file-v1:1:1:1:1"}}};
    unexpected_secret.input.resolved_secrets = extra_material;
    require_error(compile_secret_snapshot(unexpected_secret.input),
                  SecretSnapshotErrorCode::kUnexpectedResolvedSecret,
                  "/resolvedSecrets/unused-key", "unexpected resolved secret");

    Fixture invalid_revision;
    invalid_revision.secret_revision = "unsafe\nrevision";
    invalid_revision.secrets[0].opaque_revision =
        invalid_revision.secret_revision;
    require_error(compile_secret_snapshot(invalid_revision.input),
                  SecretSnapshotErrorCode::kInvalidSecretRevision,
                  "/resolvedSecrets/orders-api-token/opaqueRevision",
                  "unsafe secret revision");

    Fixture punctuation_revision;
    punctuation_revision.secret_revision = "unsafe\"revision";
    punctuation_revision.secrets[0].opaque_revision =
        punctuation_revision.secret_revision;
    require_error(compile_secret_snapshot(punctuation_revision.input),
                  SecretSnapshotErrorCode::kInvalidSecretRevision,
                  "/resolvedSecrets/orders-api-token/opaqueRevision",
                  "secret revision with non-contract punctuation");
}

void test_snapshot_resource_and_text_limits_match_runtime() {
    Fixture oversized_value;
    std::string huge(kMaxEnvironmentValueBytes + 1, 'x');
    oversized_value.requests[0].value = huge;
    require_error(compile_secret_snapshot(oversized_value.input),
                  SecretSnapshotErrorCode::kValueTooLarge,
                  "/permissions/env/APP_MODE/value", "oversized literal value");

    Fixture nul_value;
    const std::string with_nul("prefix\0suffix", 13);
    nul_value.secrets[0].value = with_nul;
    require_error(compile_secret_snapshot(nul_value.input),
                  SecretSnapshotErrorCode::kInvalidValue,
                  "/permissions/env/API_TOKEN/valueFrom", "NUL secret value");

    Fixture invalid_utf8;
    const std::string bad_utf8("\xc3\x28", 2);
    invalid_utf8.secrets[0].value = bad_utf8;
    require_error(compile_secret_snapshot(invalid_utf8.input),
                  SecretSnapshotErrorCode::kInvalidValue,
                  "/permissions/env/API_TOKEN/valueFrom", "invalid UTF-8 secret");

    std::vector<std::string> names;
    std::vector<std::string> values;
    std::vector<EnvironmentRequest> too_many;
    names.reserve(kMaxEnvironmentEntries + 1);
    values.reserve(kMaxEnvironmentEntries + 1);
    too_many.reserve(kMaxEnvironmentEntries + 1);
    for (std::size_t i = 0; i <= kMaxEnvironmentEntries; ++i) {
        names.push_back("ENV_" + std::to_string(i));
        values.emplace_back("x");
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        too_many.push_back(
            EnvironmentRequest{names[i], values[i], std::nullopt});
    }
    Fixture count_limit;
    const std::array<std::string_view, 1> all_names{"*"};
    count_limit.input.host_environment_names = all_names;
    count_limit.input.requests = too_many;
    count_limit.input.resolved_secrets = {};
    require_error(compile_secret_snapshot(count_limit.input),
                  SecretSnapshotErrorCode::kTooManyEntries,
                  "/permissions/env", "too many environment entries");

    std::array<std::string, 4> total_names{
        "ENV_A", "ENV_B", "ENV_C", "ENV_D"};
    std::array<std::string, 4> total_values;
    std::array<EnvironmentRequest, 4> total_requests;
    for (std::size_t i = 0; i < total_values.size(); ++i) {
        total_values[i].assign(kMaxEnvironmentSnapshotBytes / 4, 'x');
        total_requests[i] = EnvironmentRequest{
            total_names[i], total_values[i], std::nullopt};
    }
    Fixture total_limit;
    total_limit.input.host_environment_names = all_names;
    total_limit.input.requests = total_requests;
    total_limit.input.resolved_secrets = {};
    require_error(compile_secret_snapshot(total_limit.input),
                  SecretSnapshotErrorCode::kSnapshotTooLarge,
                  "/permissions/env", "oversized environment snapshot");
}

struct GenerationFixture {
    std::string source_digest = "sha256:" + std::string(64, '1');
    std::string attestation_digest = "sha256:" + std::string(64, '2');
    std::string app_config_digest = "sha256:" + std::string(64, '3');
    std::string policy_digest = "sha256:" + std::string(64, '4');
    std::string resource_digest = "sha256:" + std::string(64, '5');
    std::string host_digest = "sha256:" + std::string(64, '6');
    std::string compatibility_id = "sha256:" + std::string(64, '7');
    GenerationIdentityInput input;

    explicit GenerationFixture(std::string_view secret_revision) {
        input.application_id = "orders";
        input.source_digest = source_digest;
        input.bytecode_attestation_digest = attestation_digest;
        input.selected_artifact = SelectedArtifactKind::kTrustedBytecode;
        input.normalized_app_config_digest = app_config_digest;
        input.effective_policy_digest = policy_digest;
        input.effective_resource_digest = resource_digest;
        input.host_config_digest = host_digest;
        input.secret_revision = secret_revision;
        input.runtime_compatibility_id = compatibility_id;
    }
};

void test_secret_revision_frozen_golden_digest() {
    // The aggregate revision is a FROZEN protocol. The digest below was
    // computed from the canonical message by hand and pins the framing
    // byte-for-byte:
    //   "capsid-secret-revision-v1\0"           (domain with embedded NUL)
    //   u32be(len) + "orders"                    (App ID)
    //   u32be(9)  + "API_TOKEN"                  (env name, sorted)
    //   u32be(16) + "orders-api-token"           (secret key ID)
    //   u32be(26) + "file-v1:11:22:41:1700000000" (opaque revision)
    // The literal APP_MODE entry contributes nothing to the message. Any
    // drift — prefix width, byte order, field order, domain text, NUL
    // handling, literal leakage, sort order — breaks this digest.
    Fixture fixture;
    const SecretSnapshotCompileResult result =
        compile_secret_snapshot(fixture.input);
    require(result.ok, "frozen-golden fixture was rejected: " +
                           result.error.message);
    require(result.snapshot.secret_revision() ==
                "sha256:70513d980eef126857ed555bb1183f3dd1e5f709a1b0cf23"
                "c4439d057b1e35ac",
            "secret revision framing drifted from the frozen golden record");
}

void test_secret_revision_multi_secret_sorted_golden() {
    // Two secret entries plus a literal, requested in the wrong order: the
    // revision message must carry the secrets sorted by env name
    // (API_TOKEN before DB_PASSWORD) and exclude the literal, matching
    // this frozen digest for that exact byte sequence.
    const std::string literal_value = "production\n";
    const std::array<EnvironmentRequest, 3> requests{{
        EnvironmentRequest{"DB_PASSWORD", std::nullopt, "db-password"},
        EnvironmentRequest{"APP_MODE", literal_value, std::nullopt},
        EnvironmentRequest{"API_TOKEN", std::nullopt, "orders-api-token"},
    }};
    const std::array<ResolvedSecret, 2> secrets{{
        {"db-password", "db-value", "file-v1:100:200:7:1800000000"},
        {"orders-api-token", "api-value", "file-v1:11:22:41:1700000000"},
    }};
    const std::array<std::string_view, 3> host_names{
        "API_*", "APP_*", "DB_*"};
    SecretSnapshotCompileInput input;
    input.application_id = "orders";
    input.host_allows_env_module = true;
    input.app_requests_env_module = true;
    input.host_environment_names = host_names;
    input.requests = requests;
    input.resolved_secrets = secrets;
    const SecretSnapshotCompileResult result =
        compile_secret_snapshot(input);
    require(result.ok,
            "multi-secret golden fixture was rejected: " +
                result.error.message);
    require(result.snapshot.secret_revision() ==
                "sha256:0160fc5ac1f7d4628bdf62df2c106a4d4ba89d5e6c88951f2"
                "05916fdd051e147",
            "multi-secret revision order or framing drifted from the "
            "frozen golden");
    // Cross-check against the independent framing rebuild: sorted order,
    // literals excluded.
    const std::array<std::array<std::string_view, 3>, 2> entries{{
        {"API_TOKEN", "orders-api-token", "file-v1:11:22:41:1700000000"},
        {"DB_PASSWORD", "db-password", "file-v1:100:200:7:1800000000"},
    }};
    require(result.snapshot.secret_revision() ==
                expected_secret_revision("orders", entries),
            "multi-secret revision is not the canonical sorted message");
}

void test_rotation_changes_generation_without_mutating_old_snapshot() {
    Fixture old_fixture;
    Fixture new_fixture;
    new_fixture.secret_value = "rotated-value";
    new_fixture.secret_revision = "file-v1:11:23:13:1700000100";
    new_fixture.secrets[0].value = new_fixture.secret_value;
    new_fixture.secrets[0].opaque_revision = new_fixture.secret_revision;

    const SecretSnapshotCompileResult old_result =
        compile_secret_snapshot(old_fixture.input);
    const SecretSnapshotCompileResult new_result =
        compile_secret_snapshot(new_fixture.input);
    require(old_result.ok && new_result.ok,
            "rotation fixtures did not compile");
    require(old_result.snapshot.entries()[0].value ==
                std::string(kSecretCanary) + "\n" &&
                new_result.snapshot.entries()[0].value == "rotated-value",
            "rotation mutated the old snapshot or lost the new value");
    require(old_result.snapshot.secret_revision() !=
                new_result.snapshot.secret_revision(),
            "opaque provider revision did not change snapshot identity");

    GenerationFixture old_generation(
        old_result.snapshot.secret_revision());
    GenerationFixture new_generation(
        new_result.snapshot.secret_revision());
    const std::string old_digest =
        compute_generation_digest(old_generation.input);
    const std::string new_digest =
        compute_generation_digest(new_generation.input);
    require(old_digest == expected_generation_digest(old_generation.input),
            "generation digest does not match the frozen binary record");
    require(old_digest != new_digest,
            "secret rotation reused the prior generation identity");
    require(old_digest.find(kSecretCanary) == std::string::npos &&
                new_digest.find(kSecretCanary) == std::string::npos,
            "generation identity leaked the secret canary");

    GenerationIdentityInput source_generation = old_generation.input;
    source_generation.selected_artifact = SelectedArtifactKind::kSource;
    require(compute_generation_digest(source_generation) != old_digest,
            "selected artifact kind is absent from generation identity");
}

}  // namespace

// Binding v1 §7.8: the binding-set digest is part of the Generation
// Identity; manifest/source/config/policy/profile or secret-revision
// changes alter it, entry order does not, and secret VALUES never
// enter the record.
void test_binding_set_digest() {
    const auto entry = [](const std::string &id) {
        capsid::host::BindingSetDigestEntry value;
        value.id = id;
        value.manifest_digest = "sha256:" + std::string(64, 'a');
        value.source_digest = "sha256:" + std::string(64, 'b');
        value.config_digest = "sha256:" + std::string(64, 'c');
        value.permission_digest = "sha256:" + std::string(64, 'd');
        value.profile_digest = "sha256:" + std::string(64, 'e');
        value.secret_key_ids = {"password", "username"};
        value.secret_revision = "rev-1";
        return value;
    };

    const std::string empty =
        capsid::host::compute_binding_set_digest({});
    require(!empty.empty() && empty.rfind("sha256:", 0) == 0,
            "empty binding set digest is not a sha256 digest");

    const capsid::host::BindingSetDigestEntry mongo = entry("mongo");
    const capsid::host::BindingSetDigestEntry redis = entry("redis");
    const std::string forward =
        capsid::host::compute_binding_set_digest({mongo, redis});
    const std::string reversed =
        capsid::host::compute_binding_set_digest({redis, mongo});
    require(forward == reversed,
            "binding set digest depends on entry order");
    require(forward != empty,
            "binding set digest equals the zero-binding digest");

    // Every identity component is sensitive.
    capsid::host::BindingSetDigestEntry changed = mongo;
    changed.manifest_digest = "sha256:" + std::string(64, 'f');
    require(capsid::host::compute_binding_set_digest({mongo}) !=
                capsid::host::compute_binding_set_digest({changed}),
            "manifest digest change did not alter the set digest");
    changed = mongo;
    changed.source_digest = "sha256:" + std::string(64, 'f');
    require(capsid::host::compute_binding_set_digest({mongo}) !=
                capsid::host::compute_binding_set_digest({changed}),
            "source digest change did not alter the set digest");
    changed = mongo;
    changed.config_digest = "sha256:" + std::string(64, 'f');
    require(capsid::host::compute_binding_set_digest({mongo}) !=
                capsid::host::compute_binding_set_digest({changed}),
            "config digest change did not alter the set digest");
    changed = mongo;
    changed.permission_digest = "sha256:" + std::string(64, 'f');
    require(capsid::host::compute_binding_set_digest({mongo}) !=
                capsid::host::compute_binding_set_digest({changed}),
            "permission digest change did not alter the set digest");
    changed = mongo;
    changed.profile_digest = "sha256:" + std::string(64, 'f');
    require(capsid::host::compute_binding_set_digest({mongo}) !=
                capsid::host::compute_binding_set_digest({changed}),
            "profile digest change did not alter the set digest");
    changed = mongo;
    changed.secret_revision = "rev-2";
    require(capsid::host::compute_binding_set_digest({mongo}) !=
                capsid::host::compute_binding_set_digest({changed}),
            "secret revision change did not alter the set digest");

    // Secret key ordering is canonical; secret values are not in the
    // record at all (the entry type has no value field).
    capsid::host::BindingSetDigestEntry reordered = mongo;
    reordered.secret_key_ids = {"username", "password"};
    require(capsid::host::compute_binding_set_digest({mongo}) ==
                capsid::host::compute_binding_set_digest({reordered}),
            "secret key order changed the set digest");
}


int main() {
    test_binding_set_digest();
    test_minimal_snapshot_is_owned_canonical_and_redacted();
    test_empty_snapshot_does_not_require_environment_capability();
    test_safe_secret_metadata_punctuation_is_accepted();
    test_request_order_does_not_change_snapshot_identity();
    test_policy_and_provider_fail_closed_without_leaking_values();
    test_snapshot_resource_and_text_limits_match_runtime();
    test_secret_revision_frozen_golden_digest();
    test_secret_revision_multi_secret_sorted_golden();
    test_rotation_changes_generation_without_mutating_old_snapshot();
    return 0;
}
