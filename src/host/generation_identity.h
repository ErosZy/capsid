#ifndef CAPSID_HOST_GENERATION_IDENTITY_H
#define CAPSID_HOST_GENERATION_IDENTITY_H

#include <string>
#include <string_view>

namespace capsid::host {

enum class SelectedArtifactKind {
    kSource,
    kTrustedBytecode,
};

struct GenerationIdentityInput {
    std::string_view application_id;
    std::string_view source_digest;
    // Empty when no provenance-valid attestation was supplied.
    std::string_view bytecode_attestation_digest;
    SelectedArtifactKind selected_artifact = SelectedArtifactKind::kSource;
    std::string_view normalized_app_config_digest;
    std::string_view effective_policy_digest;
    std::string_view effective_resource_digest;
    std::string_view host_config_digest;
    std::string_view secret_revision;
    std::string_view runtime_compatibility_id;
};

// Returns "sha256:" plus lowercase SHA-256 of this binary record:
// "capsid-generation-v1\0", followed by every field above in declaration
// order as 32-bit big-endian byte length + bytes. selected_artifact is the
// stable ASCII value "source" or "trusted-bytecode". The domain NUL is part
// of the hash input. This framing is locale-independent and unambiguous.
std::string compute_generation_digest(const GenerationIdentityInput& input);

}  // namespace capsid::host

#endif
