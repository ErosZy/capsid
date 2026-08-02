#include "host/bytecode_attestation.h"

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using capsid::host::BytecodeArtifactSelection;
using capsid::host::BytecodeAttestationErrorCode;
using capsid::host::BytecodeAttestationInput;
using capsid::host::BytecodeAttestationResult;
using capsid::host::TrustedBytecodeKey;
using capsid::host::kEd25519PublicKeyBytes;
using capsid::host::kEd25519SignatureBytes;
using capsid::host::kMaxBytecodeAttestationBytes;
using capsid::host::kMaxBytecodeAttestationClaimBytes;
using capsid::host::verify_bytecode_attestation;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-bytecode-attestation: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::vector<std::uint8_t> hex_bytes(std::string_view hex) {
    require(hex.size() % 2 == 0, "odd-length hex fixture");
    auto nibble = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') {
            return static_cast<std::uint8_t>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<std::uint8_t>(c - 'a' + 10);
        }
        fail("invalid hex fixture");
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(
            (nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return bytes;
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

std::string sha256_id(std::string_view bytes) {
    return sha256_id(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

struct Claims {
    std::string schema = "capsid-bytecode-v1";
    std::string application = "orders";
    std::string version = "2026-08-01-001";
    std::string source_name = "bundle.mjs";
    std::string source_sha256;
    std::string bytecode_sha256;
    std::string compatibility_id = "sha256:" + std::string(64, 'a');
    std::string key_id = "release-2026";
};

void append_u32_be(std::vector<std::uint8_t>& output, std::size_t value) {
    require(value <= UINT32_MAX, "attestation fixture field too large");
    const std::uint32_t size = static_cast<std::uint32_t>(value);
    output.push_back(static_cast<std::uint8_t>(size >> 24));
    output.push_back(static_cast<std::uint8_t>(size >> 16));
    output.push_back(static_cast<std::uint8_t>(size >> 8));
    output.push_back(static_cast<std::uint8_t>(size));
}

std::vector<std::uint8_t> signed_message(const Claims& claims) {
    static constexpr char kDomain[] =
        "capsid-bytecode-attestation-v1\0";
    std::vector<std::uint8_t> message(
        reinterpret_cast<const std::uint8_t*>(kDomain),
        reinterpret_cast<const std::uint8_t*>(kDomain) + sizeof(kDomain) - 1);
    const std::string* fields[] = {
        &claims.schema,
        &claims.application,
        &claims.version,
        &claims.source_name,
        &claims.source_sha256,
        &claims.bytecode_sha256,
        &claims.compatibility_id,
        &claims.key_id,
    };
    for (const std::string* field : fields) {
        append_u32_be(message, field->size());
        message.insert(message.end(), field->begin(), field->end());
    }
    return message;
}

std::string attestation_json(const Claims& claims, bool reverse = false) {
    if (reverse) {
        return std::string("{") +
               "\"keyId\":\"" + claims.key_id + "\"," +
               "\"compatibilityId\":\"" + claims.compatibility_id +
               "\",\"bytecodeSha256\":\"" + claims.bytecode_sha256 +
               "\",\"sourceSha256\":\"" + claims.source_sha256 +
               "\",\"sourceName\":\"" + claims.source_name +
               "\",\"version\":\"" + claims.version +
               "\",\"application\":\"" + claims.application +
               "\",\"schema\":\"" + claims.schema + "\"}";
    }
    return std::string("{") +
           "\"schema\":\"" + claims.schema + "\"," +
           "\"application\":\"" + claims.application + "\"," +
           "\"version\":\"" + claims.version + "\"," +
           "\"sourceName\":\"" + claims.source_name + "\"," +
           "\"sourceSha256\":\"" + claims.source_sha256 + "\"," +
           "\"bytecodeSha256\":\"" + claims.bytecode_sha256 + "\"," +
           "\"compatibilityId\":\"" + claims.compatibility_id + "\"," +
           "\"keyId\":\"" + claims.key_id + "\"}";
}

std::vector<std::uint8_t> sign(
    std::span<const std::uint8_t> private_key,
    std::span<const std::uint8_t> message) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, private_key.data(), private_key.size());
    require(key != nullptr, "could not create Ed25519 private key");
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    require(context != nullptr &&
                EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1,
            "could not initialize Ed25519 signing fixture");
    std::vector<std::uint8_t> signature(kEd25519SignatureBytes);
    std::size_t signature_size = signature.size();
    require(EVP_DigestSign(context, signature.data(), &signature_size,
                           message.data(), message.size()) == 1 &&
                signature_size == kEd25519SignatureBytes,
            "could not create Ed25519 signature fixture");
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return signature;
}

struct Fixture {
    std::vector<std::uint8_t> source{
        'e', 'x', 'p', 'o', 'r', 't', ' ', 'd', 'e', 'f', 'a', 'u', 'l', 't'};
    std::vector<std::uint8_t> bytecode{0x02, 0x17, 0x42, 0xa5, 0x00};
    std::vector<std::uint8_t> private_key = hex_bytes(
        "9d61b19deffd5a60ba844af492ec2cc4"
        "4449c5697b326919703bac031cae7f60");
    std::vector<std::uint8_t> public_key = hex_bytes(
        "d75a980182b10ab7d54bfed3c964073a"
        "0ee172f3daa62325af021a68f707511a");
    Claims claims;
    std::string json;
    std::vector<std::uint8_t> signature;
    TrustedBytecodeKey key;

    Fixture() {
        require(public_key.size() == kEd25519PublicKeyBytes,
                "bad Ed25519 public-key fixture");
        claims.source_sha256 = sha256_id(source);
        claims.bytecode_sha256 = sha256_id(bytecode);
        json = attestation_json(claims);
        signature = sign(private_key, signed_message(claims));
        key = TrustedBytecodeKey{claims.key_id, public_key};
    }

    void rebuild_and_resign() {
        json = attestation_json(claims);
        signature = sign(private_key, signed_message(claims));
        key = TrustedBytecodeKey{claims.key_id, public_key};
    }

    BytecodeAttestationInput input() const {
        BytecodeAttestationInput value;
        value.source = source;
        value.bytecode = std::span<const std::uint8_t>(bytecode);
        value.attestation_json = json;
        value.signature = std::span<const std::uint8_t>(signature);
        value.expected_application = claims.application;
        value.expected_version = claims.version;
        value.expected_source_name = claims.source_name;
        value.runtime_compatibility_id = claims.compatibility_id;
        value.trusted_keys = std::span<const TrustedBytecodeKey>(&key, 1);
        return value;
    }
};

void require_result(const BytecodeAttestationResult& result,
                    BytecodeArtifactSelection selection,
                    BytecodeAttestationErrorCode code,
                    std::string_view path,
                    std::string_view label) {
    require(result.selection == selection,
            std::string(label) + " returned the wrong artifact selection");
    require(result.code == code,
            std::string(label) + " returned the wrong error code");
    require(result.path == path,
            std::string(label) + " reported path '" + result.path +
                "' instead of '" + std::string(path) + "'");
    if (selection == BytecodeArtifactSelection::kReject ||
        code != BytecodeAttestationErrorCode::kNone) {
        require(!result.message.empty(),
                std::string(label) + " returned no safe diagnostic");
    }
}

void test_artifact_set_is_all_or_none() {
    Fixture fixture;
    BytecodeAttestationInput source_only = fixture.input();
    source_only.bytecode.reset();
    source_only.attestation_json.reset();
    source_only.signature.reset();
    require_result(verify_bytecode_attestation(source_only),
                   BytecodeArtifactSelection::kSource,
                   BytecodeAttestationErrorCode::kNone, "", "source only");

    BytecodeAttestationInput partial = fixture.input();
    partial.signature.reset();
    require_result(verify_bytecode_attestation(partial),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kIncompleteArtifactSet, "",
                   "partial bytecode artifact set");

    BytecodeAttestationInput empty_source = fixture.input();
    empty_source.source = {};
    require_result(verify_bytecode_attestation(empty_source),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidValue, "/source",
                   "missing semantic source");

    BytecodeAttestationInput empty_bytecode = fixture.input();
    empty_bytecode.bytecode = std::span<const std::uint8_t>();
    require_result(verify_bytecode_attestation(empty_bytecode),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidValue, "/bytecode",
                   "present empty bytecode");
}

void test_valid_attestation_selects_trusted_bytecode() {
    Fixture fixture;
    const BytecodeAttestationResult result =
        verify_bytecode_attestation(fixture.input());
    require_result(result, BytecodeArtifactSelection::kTrustedBytecode,
                   BytecodeAttestationErrorCode::kNone, "",
                   "valid bytecode attestation");
    require(result.key_id == fixture.claims.key_id &&
                result.attestation_sha256 == sha256_id(fixture.json),
            "valid attestation did not retain safe provenance metadata");

    fixture.json = attestation_json(fixture.claims, true);
    require_result(verify_bytecode_attestation(fixture.input()),
                   BytecodeArtifactSelection::kTrustedBytecode,
                   BytecodeAttestationErrorCode::kNone, "",
                   "reordered attestation JSON");
}

void test_compatibility_fallback_requires_valid_provenance() {
    Fixture fixture;
    BytecodeAttestationInput mismatch = fixture.input();
    const std::string other_identity = "sha256:" + std::string(64, 'b');
    mismatch.runtime_compatibility_id = other_identity;
    const BytecodeAttestationResult fallback =
        verify_bytecode_attestation(mismatch);
    require_result(fallback, BytecodeArtifactSelection::kSource,
                   BytecodeAttestationErrorCode::kCompatibilityMismatch,
                   "/compatibilityId", "signed compatibility mismatch");
    require(fallback.key_id == fixture.claims.key_id &&
                fallback.attestation_sha256 == sha256_id(fixture.json),
            "compatibility fallback lost verified provenance metadata");

    fixture.claims.compatibility_id[7] = 'b';
    fixture.json = attestation_json(fixture.claims);
    // Signature remains over the original claims: this must be rejection,
    // never the compatibility fallback used for a valid old build.
    require_result(verify_bytecode_attestation(fixture.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidSignature,
                   "/signature", "unsigned compatibility tamper");
}

void test_signature_claims_and_digests_fail_closed() {
    Fixture fixture;
    fixture.signature[0] ^= 1U;
    require_result(verify_bytecode_attestation(fixture.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidSignature,
                   "/signature", "one-bit signature tamper");

    Fixture source_tamper;
    source_tamper.source[0] ^= 1U;
    require_result(verify_bytecode_attestation(source_tamper.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kDigestMismatch,
                   "/sourceSha256", "source digest mismatch");

    Fixture bytecode_tamper;
    bytecode_tamper.bytecode[0] ^= 1U;
    require_result(verify_bytecode_attestation(bytecode_tamper.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kDigestMismatch,
                   "/bytecodeSha256", "bytecode digest mismatch");

    Fixture wrong_application;
    BytecodeAttestationInput input = wrong_application.input();
    input.expected_application = "billing";
    require_result(verify_bytecode_attestation(input),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kClaimMismatch,
                   "/application", "wrong application claim");

    Fixture unknown_key;
    TrustedBytecodeKey other{"other-key", unknown_key.public_key};
    input = unknown_key.input();
    input.trusted_keys = std::span<const TrustedBytecodeKey>(&other, 1);
    require_result(verify_bytecode_attestation(input),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kUnknownKey, "/keyId",
                   "unknown signing key");
}

void test_attestation_json_is_strict_and_bounded() {
    Fixture fixture;
    fixture.json.insert(fixture.json.size() - 1, ",\"mystery\":true");
    require_result(verify_bytecode_attestation(fixture.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kUnknownField, "/mystery",
                   "unknown attestation field");

    Fixture escaped_unknown;
    escaped_unknown.json.insert(
        escaped_unknown.json.size() - 1, ",\"mystery/~field\":true");
    require_result(verify_bytecode_attestation(escaped_unknown.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kUnknownField,
                   "/mystery~1~0field", "escaped unknown attestation field");

    Fixture duplicate;
    duplicate.json.insert(
        duplicate.json.size() - 1,
        ",\"keyId\":\"release-2026\"");
    require_result(verify_bytecode_attestation(duplicate.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kDuplicateKey, "",
                   "duplicate attestation field");

    Fixture oversized;
    oversized.json.resize(kMaxBytecodeAttestationBytes + 1, ' ');
    require_result(verify_bytecode_attestation(oversized.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kResourceLimit, "",
                   "oversized attestation");

    Fixture short_signature;
    short_signature.signature.pop_back();
    require_result(verify_bytecode_attestation(short_signature.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidValue, "/signature",
                   "short Ed25519 signature");
}

void test_schema_and_claim_syntax_are_validated_before_signature() {
    Fixture wrong_schema;
    wrong_schema.claims.schema = "capsid-bytecode-v2";
    wrong_schema.rebuild_and_resign();
    require_result(verify_bytecode_attestation(wrong_schema.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidValue, "/schema",
                   "unsupported attestation schema");

    Fixture malformed_source_digest;
    malformed_source_digest.claims.source_sha256 =
        "sha256:" + std::string(63, 'a') + "G";
    malformed_source_digest.rebuild_and_resign();
    require_result(verify_bytecode_attestation(
                       malformed_source_digest.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidValue,
                   "/sourceSha256", "malformed source digest claim");

    Fixture malformed_bytecode_digest;
    malformed_bytecode_digest.claims.bytecode_sha256 = "not-a-sha256";
    malformed_bytecode_digest.rebuild_and_resign();
    require_result(verify_bytecode_attestation(
                       malformed_bytecode_digest.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidValue,
                   "/bytecodeSha256", "malformed bytecode digest claim");

    Fixture malformed_identity;
    malformed_identity.claims.compatibility_id =
        "sha256:" + std::string(65, 'a');
    malformed_identity.rebuild_and_resign();
    require_result(verify_bytecode_attestation(malformed_identity.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kInvalidValue,
                   "/compatibilityId", "malformed compatibility identity");

    Fixture oversized_claim;
    oversized_claim.claims.application =
        std::string(kMaxBytecodeAttestationClaimBytes + 1, 'a');
    oversized_claim.rebuild_and_resign();
    require_result(verify_bytecode_attestation(oversized_claim.input()),
                   BytecodeArtifactSelection::kReject,
                   BytecodeAttestationErrorCode::kResourceLimit,
                   "/application", "oversized attestation claim");
}

}  // namespace

int main() {
    test_artifact_set_is_all_or_none();
    test_valid_attestation_selects_trusted_bytecode();
    test_compatibility_fallback_requires_valid_provenance();
    test_signature_claims_and_digests_fail_closed();
    test_attestation_json_is_strict_and_bounded();
    test_schema_and_claim_syntax_are_validated_before_signature();
    return 0;
}
