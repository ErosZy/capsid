#ifndef CAPSID_HOST_GENERATION_IDENTITY_H
#define CAPSID_HOST_GENERATION_IDENTITY_H

#include <string>

namespace capsid::host {

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
};

// Returns "sha256:" plus lowercase SHA-256 of this binary record:
// "capsid-generation-v1\0", followed by every field above in declaration
// order as 32-bit big-endian byte length + bytes. selected_artifact is the
// stable ASCII value "source" or "trusted-bytecode". The domain NUL is part
// of the hash input. This framing is locale-independent and unambiguous.
std::string compute_generation_digest(const GenerationIdentityInput& input);

}  // namespace capsid::host

#endif
