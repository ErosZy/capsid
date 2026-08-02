#ifndef CAPSID_HOST_BYTECODE_ATTESTATION_H
#define CAPSID_HOST_BYTECODE_ATTESTATION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace capsid::host {

inline constexpr std::size_t kMaxBytecodeAttestationBytes = 64U * 1024U;
inline constexpr std::size_t kMaxBytecodeAttestationClaimBytes = 1024U;
inline constexpr std::size_t kEd25519PublicKeyBytes = 32U;
inline constexpr std::size_t kEd25519SignatureBytes = 64U;
inline constexpr std::string_view kBytecodeAttestationSchema =
    "capsid-bytecode-v1";

enum class BytecodeArtifactSelection {
    kReject,
    kSource,
    kTrustedBytecode,
};

enum class BytecodeAttestationErrorCode {
    kNone,
    kIncompleteArtifactSet,
    kInvalidJson,
    kDuplicateKey,
    kUnknownField,
    kInvalidValue,
    kUnknownKey,
    kInvalidSignature,
    kDigestMismatch,
    kClaimMismatch,
    kCompatibilityMismatch,
    kResourceLimit,
};

struct TrustedBytecodeKey {
    std::string_view key_id;
    // Raw Ed25519 public key. It must be exactly kEd25519PublicKeyBytes.
    std::span<const std::uint8_t> public_key;
};

struct BytecodeAttestationInput {
    // Source is mandatory even when trusted bytecode is selected.
    std::span<const std::uint8_t> source;

    // The three optional bytecode artifacts are all-or-none. A present empty
    // artifact is invalid, not equivalent to absence.
    std::optional<std::span<const std::uint8_t>> bytecode;
    std::optional<std::string_view> attestation_json;
    std::optional<std::span<const std::uint8_t>> signature;

    std::string_view expected_application;
    std::string_view expected_version;
    std::string_view expected_source_name;
    std::string_view runtime_compatibility_id;
    std::span<const TrustedBytecodeKey> trusted_keys;
};

struct BytecodeAttestationResult {
    BytecodeArtifactSelection selection = BytecodeArtifactSelection::kReject;
    BytecodeAttestationErrorCode code = BytecodeAttestationErrorCode::kNone;
    // RFC 6901 pointer. Non-JSON artifacts use /signature or /bytecode.
    std::string path;
    std::string message;
    // Populated for a provenance-valid trusted or compatibility-fallback
    // result. Values are safe metadata and contain no secret material.
    std::string key_id;
    std::string attestation_sha256;
};

// Fail-closed artifact selection and Ed25519 provenance verification.
//
// - no bytecode artifacts: kSource + kNone;
// - complete, valid, compatible set: kTrustedBytecode + kNone;
// - complete and provenance-valid but compatibility-mismatched set:
//   kSource + kCompatibilityMismatch;
// - every other condition: kReject with a stable non-kNone code.
//
// Before signature verification, the verifier rejects a schema other than
// kBytecodeAttestationSchema, a claim longer than
// kMaxBytecodeAttestationClaimBytes, and SHA-256 claims that are not exactly
// "sha256:" plus 64 lowercase hexadecimal digits. A present empty bytecode
// artifact is invalid. JSON-member paths are RFC 6901 escaped.
BytecodeAttestationResult verify_bytecode_attestation(
    const BytecodeAttestationInput& input);

}  // namespace capsid::host

#endif
