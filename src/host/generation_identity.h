#ifndef CAPSID_HOST_GENERATION_IDENTITY_H
#define CAPSID_HOST_GENERATION_IDENTITY_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {

inline constexpr std::string_view kBindingRuntimeCompatibilityVersion =
    "capsid-binding-runtime-v1";

// "sha256:" plus the lowercase hex SHA-256 of `bytes`. Shared by the
// generation digest, the binding manifest digest and the binding registry
// source digest.
std::string sha256_hex(std::span<const std::uint8_t> bytes);

// Canonical binding manifest digest: "sha256:" over the compact,
// key-sorted, ASCII-normalized serialization of the validated manifest,
// so key order in the file never changes the digest. Returns an empty
// string when `json` fails manifest validation.
std::string compute_binding_manifest_digest(std::string_view json);

enum class SelectedArtifactKind {
    kSource,
    kTrustedBytecode,
};

// The input owns all of its strings. The Host never passes expression-level
// temporaries into a view: a digest built from a temporary std::string would
// dangle before compute_generation_digest reads it (heap-use-after-free).
struct GenerationIdentityInput {
    std::string application_id;
    std::string source_digest;
    // Empty when no provenance-valid attestation was supplied.
    std::string bytecode_attestation_digest;
    SelectedArtifactKind selected_artifact = SelectedArtifactKind::kSource;
    std::string normalized_app_config_digest;
    std::string effective_policy_digest;
    std::string effective_resource_digest;
    std::string host_config_digest;
    std::string secret_revision;
    std::string runtime_compatibility_id;
    // Binding v1 §6: the binding-set digest over every committed Binding
    // (manifest, source, canonical config, effective permissions, sandbox
    // profiles, secret key ids and revisions). Empty for zero-binding
    // generations. Secret values never enter the digest.
    std::string binding_set_digest;
};

// Binding v1 §6: one committed Binding's digest entry. The digest is
// "sha256:" over the entries sorted by id, each as length-prefixed
// fields: id, manifest_digest, source_digest, canonical config digest,
// effective permission digest, sandbox profile digest, then the sorted
// secret key ids with the opaque secret revision. Secret values never
// enter the record.
struct BindingSetDigestEntry {
    std::string id;
    std::string manifest_digest;
    std::string source_digest;
    std::string config_digest;
    std::string permission_digest;
    std::string profile_digest;
    std::string binding_runtime_compatibility;
    std::vector<std::string> secret_key_ids;  // sorted
    std::string secret_revision;
};

std::string compute_binding_set_digest(
    const std::vector<BindingSetDigestEntry> &entries);

// Returns "sha256:" plus lowercase SHA-256 of this binary record:
// "capsid-generation-v1\0", followed by every field above in declaration
// order as 32-bit big-endian byte length + bytes. selected_artifact is the
// stable ASCII value "source" or "trusted-bytecode". The domain NUL is part
// of the hash input. This framing is locale-independent and unambiguous.
std::string compute_generation_digest(const GenerationIdentityInput& input);

}  // namespace capsid::host

#endif
