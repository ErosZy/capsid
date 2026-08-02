// Host-side trusted-bytecode attestation.
//
// Fail-closed artifact selection and Ed25519 provenance verification for
// the capsid-bytecode-attestation-v1 format (see the Host design review
// §3.5). The signed message is the fixed domain string (including its
// terminating NUL), followed by each claim as a 32-bit big-endian length
// and UTF-8 bytes, in the schema/application/version/sourceName/
// sourceSha256/bytecodeSha256/compatibilityId/keyId order. The signature is
// verified BEFORE expected-claim and digest checks, and the compatibility
// fallback is only reachable for provenance-valid attestations: an unsigned
// identity tamper is kInvalidSignature, never a silent fallback to source.

#include "host/bytecode_attestation.h"

#include <jansson.h>
#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {
namespace {

using ErrorCode = BytecodeAttestationErrorCode;

void set_error(BytecodeAttestationResult &result,
               ErrorCode code,
               std::string path,
               std::string message) {
    result.selection = BytecodeArtifactSelection::kReject;
    result.code = code;
    result.path = std::move(path);
    result.message = std::move(message);
}

bool sha256_hex(std::span<const std::uint8_t> bytes, std::string *out) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &digest_size,
                   EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
        return false;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    out->clear();
    out->reserve(64);
    for (unsigned int i = 0; i < digest_size; ++i) {
        out->push_back(kHex[digest[i] >> 4]);
        out->push_back(kHex[digest[i] & 0x0f]);
    }
    return true;
}

// RFC 6901 JSON Pointer escaping: ~ -> ~0 and / -> ~1.
std::string escape_pointer_component(std::string_view component) {
    std::string escaped;
    escaped.reserve(component.size());
    for (const char c : component) {
        if (c == '~') {
            escaped += "~0";
        } else if (c == '/') {
            escaped += "~1";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

// Claim values must be exactly "sha256:" plus 64 lowercase hex digits.
bool is_sha256_id(std::string_view value) {
    if (value.size() != 7 + 64 || value.substr(0, 7) != "sha256:") {
        return false;
    }
    for (const char c : value.substr(7)) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

struct ParsedClaims {
    std::string schema;
    std::string application;
    std::string version;
    std::string source_name;
    std::string source_sha256;
    std::string bytecode_sha256;
    std::string compatibility_id;
    std::string key_id;
};

// Strict JSON parsing and claim extraction for bytecode.json. Rejects
// oversized documents, duplicate keys, unknown fields, missing fields and
// non-string values; every failure is kReject with an RFC 6901 pointer.
bool parse_attestation_json(std::string_view json,
                            ParsedClaims *claims,
                            BytecodeAttestationResult *error) {
    if (json.size() > kMaxBytecodeAttestationBytes) {
        set_error(*error, ErrorCode::kResourceLimit, "",
                  "attestation exceeds the size limit");
        return false;
    }
    json_error_t parse_error;
    json_t *root =
        json_loadb(json.data(), json.size(), JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr) {
        const enum json_error_code parse_code = json_error_code(&parse_error);
        set_error(*error,
                  parse_code == json_error_duplicate_key
                      ? ErrorCode::kDuplicateKey
                      : ErrorCode::kInvalidJson,
                  "",
                  parse_code == json_error_duplicate_key
                      ? "duplicate attestation field"
                      : "invalid attestation JSON");
        return false;
    }
    if (!json_is_object(root)) {
        set_error(*error, ErrorCode::kInvalidJson, "",
                  "attestation must be a JSON object");
        json_decref(root);
        return false;
    }

    static constexpr std::string_view kClaimNames[] = {
        "schema",          "application",    "version",       "sourceName",
        "sourceSha256",    "bytecodeSha256", "compatibilityId", "keyId",
    };
    std::string values[8];

    for (void *iter = json_object_iter(root); iter != nullptr;
         iter = json_object_iter_next(root, iter)) {
        const std::string_view key = json_object_iter_key(iter);
        std::size_t index = 0;
        for (; index < 8; ++index) {
            if (key == kClaimNames[index]) {
                break;
            }
        }
        if (index == 8) {
            set_error(*error, ErrorCode::kUnknownField,
                      "/" + escape_pointer_component(key),
                      "unknown attestation field");
            json_decref(root);
            return false;
        }
        json_t *value = json_object_iter_value(iter);
        if (!json_is_string(value)) {
            set_error(*error, ErrorCode::kInvalidValue,
                      "/" + escape_pointer_component(key),
                      "attestation claim must be a string");
            json_decref(root);
            return false;
        }
        values[index] = json_string_value(value);
    }
    json_decref(root);

    for (std::size_t index = 0; index < 8; ++index) {
        if (values[index].empty()) {
            set_error(*error, ErrorCode::kInvalidValue,
                      "/" + escape_pointer_component(kClaimNames[index]),
                      "missing attestation claim");
            return false;
        }
        if (values[index].size() > kMaxBytecodeAttestationClaimBytes) {
            set_error(*error, ErrorCode::kResourceLimit,
                      "/" + escape_pointer_component(kClaimNames[index]),
                      "attestation claim exceeds the size limit");
            return false;
        }
    }
    claims->schema = std::move(values[0]);
    claims->application = std::move(values[1]);
    claims->version = std::move(values[2]);
    claims->source_name = std::move(values[3]);
    claims->source_sha256 = std::move(values[4]);
    claims->bytecode_sha256 = std::move(values[5]);
    claims->compatibility_id = std::move(values[6]);
    claims->key_id = std::move(values[7]);

    // Syntax checks run before any key lookup or signature work.
    if (claims->schema != kBytecodeAttestationSchema) {
        set_error(*error, ErrorCode::kInvalidValue, "/schema",
                  "unsupported attestation schema");
        return false;
    }
    // The three SHA-256 claims must be exactly "sha256:" plus 64 lowercase
    // hex digits; a malformed identity is rejected, never compared loosely.
    if (!is_sha256_id(claims->source_sha256)) {
        set_error(*error, ErrorCode::kInvalidValue, "/sourceSha256",
                  "malformed source digest claim");
        return false;
    }
    if (!is_sha256_id(claims->bytecode_sha256)) {
        set_error(*error, ErrorCode::kInvalidValue, "/bytecodeSha256",
                  "malformed bytecode digest claim");
        return false;
    }
    if (!is_sha256_id(claims->compatibility_id)) {
        set_error(*error, ErrorCode::kInvalidValue, "/compatibilityId",
                  "malformed compatibility identity");
        return false;
    }
    return true;
}

std::vector<std::uint8_t> build_signed_message(const ParsedClaims &claims) {
    // The domain includes its terminating NUL: sizeof - 1 is the domain
    // length, matching the format pinned by the design review.
    static constexpr char kDomain[] = "capsid-bytecode-attestation-v1\0";
    std::vector<std::uint8_t> message(
        reinterpret_cast<const std::uint8_t *>(kDomain),
        reinterpret_cast<const std::uint8_t *>(kDomain) + sizeof(kDomain) - 1);
    const std::string *fields[] = {
        &claims.schema,          &claims.application,
        &claims.version,         &claims.source_name,
        &claims.source_sha256,   &claims.bytecode_sha256,
        &claims.compatibility_id, &claims.key_id,
    };
    for (const std::string *field : fields) {
        const std::uint32_t size = static_cast<std::uint32_t>(field->size());
        message.push_back(static_cast<std::uint8_t>(size >> 24));
        message.push_back(static_cast<std::uint8_t>(size >> 16));
        message.push_back(static_cast<std::uint8_t>(size >> 8));
        message.push_back(static_cast<std::uint8_t>(size));
        message.insert(message.end(), field->begin(), field->end());
    }
    return message;
}

bool ed25519_verify(std::span<const std::uint8_t> public_key,
                    std::span<const std::uint8_t> message,
                    std::span<const std::uint8_t> signature) {
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size());
    if (key == nullptr) {
        return false;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    const bool valid =
        context != nullptr &&
        EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(context, signature.data(), signature.size(),
                         message.data(), message.size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return valid;
}

}  // namespace

BytecodeAttestationResult verify_bytecode_attestation(
    const BytecodeAttestationInput &input) {
    BytecodeAttestationResult result;

    // Artifacts are all-or-none: bytecode, attestation and signature must
    // travel together.
    const bool has_bytecode = input.bytecode.has_value();
    const bool has_json = input.attestation_json.has_value();
    const bool has_signature = input.signature.has_value();
    const std::size_t present_count =
        (has_bytecode ? 1U : 0U) + (has_json ? 1U : 0U) +
        (has_signature ? 1U : 0U);
    if (present_count != 0 && present_count != 3) {
        set_error(result, ErrorCode::kIncompleteArtifactSet, "",
                  "bytecode artifacts are all-or-none");
        return result;
    }

    // The semantic source is always mandatory.
    if (input.source.empty()) {
        set_error(result, ErrorCode::kInvalidValue, "/source",
                  "semantic source is required");
        return result;
    }
    // A present bytecode artifact must not be empty: an empty artifact is
    // invalid, never equivalent to absence.
    if (has_bytecode && input.bytecode->empty()) {
        set_error(result, ErrorCode::kInvalidValue, "/bytecode",
                  "bytecode artifact must not be empty");
        return result;
    }

    // No artifacts: ordinary source deployment.
    if (present_count == 0) {
        result.selection = BytecodeArtifactSelection::kSource;
        result.code = ErrorCode::kNone;
        return result;
    }

    ParsedClaims claims;
    if (!parse_attestation_json(*input.attestation_json, &claims, &result)) {
        return result;
    }

    if (input.signature->size() != kEd25519SignatureBytes) {
        set_error(result, ErrorCode::kInvalidValue, "/signature",
                  "signature must be exactly 64 bytes");
        return result;
    }

    // Key lookup by keyId before any cryptographic work.
    const TrustedBytecodeKey *trusted_key = nullptr;
    for (const TrustedBytecodeKey &candidate : input.trusted_keys) {
        if (candidate.key_id == claims.key_id) {
            trusted_key = &candidate;
            break;
        }
    }
    if (trusted_key == nullptr) {
        set_error(result, ErrorCode::kUnknownKey, "/keyId",
                  "unknown signing key");
        return result;
    }
    if (trusted_key->public_key.size() != kEd25519PublicKeyBytes) {
        set_error(result, ErrorCode::kInvalidValue, "/keyId",
                  "trusted key must be exactly 32 bytes");
        return result;
    }

    // Signature verification precedes every claim, digest and compatibility
    // decision: an unsigned identity tamper must never fall back to source.
    const std::vector<std::uint8_t> message = build_signed_message(claims);
    if (!ed25519_verify(trusted_key->public_key, message, *input.signature)) {
        set_error(result, ErrorCode::kInvalidSignature, "/signature",
                  "attestation signature is invalid");
        return result;
    }

    // Expected claims.
    if (claims.application != input.expected_application) {
        set_error(result, ErrorCode::kClaimMismatch, "/application",
                  "application claim mismatch");
        return result;
    }
    if (claims.version != input.expected_version) {
        set_error(result, ErrorCode::kClaimMismatch, "/version",
                  "version claim mismatch");
        return result;
    }
    if (claims.source_name != input.expected_source_name) {
        set_error(result, ErrorCode::kClaimMismatch, "/sourceName",
                  "source name claim mismatch");
        return result;
    }

    // Content digests. Claims carry the "sha256:"-prefixed form; the bare
    // hex produced here must be prefixed before comparison.
    std::string source_digest;
    if (!sha256_hex(input.source, &source_digest) ||
        "sha256:" + source_digest != claims.source_sha256) {
        set_error(result, ErrorCode::kDigestMismatch, "/sourceSha256",
                  "source digest mismatch");
        return result;
    }
    std::string bytecode_digest;
    if (!sha256_hex(*input.bytecode, &bytecode_digest) ||
        "sha256:" + bytecode_digest != claims.bytecode_sha256) {
        set_error(result, ErrorCode::kDigestMismatch, "/bytecodeSha256",
                  "bytecode digest mismatch");
        return result;
    }

    // Provenance is valid; retain safe metadata and decide compatibility.
    result.key_id = claims.key_id;
    if (!sha256_hex(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(
                    input.attestation_json->data()),
                input.attestation_json->size()),
            &result.attestation_sha256)) {
        set_error(result, ErrorCode::kInvalidValue, "",
                  "could not digest the attestation");
        return result;
    }
    result.attestation_sha256 = "sha256:" + result.attestation_sha256;
    if (claims.compatibility_id != input.runtime_compatibility_id) {
        result.selection = BytecodeArtifactSelection::kSource;
        result.code = ErrorCode::kCompatibilityMismatch;
        result.path = "/compatibilityId";
        result.message = "attestation is for an incompatible runtime build";
        return result;
    }

    result.selection = BytecodeArtifactSelection::kTrustedBytecode;
    result.code = ErrorCode::kNone;
    return result;
}

}  // namespace capsid::host
