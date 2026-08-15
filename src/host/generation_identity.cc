// Canonical generation identity.
//
// Binary record: "capsid-generation-v1\0" (the NUL is part of the hash
// input), followed by every input field in declaration order as a 32-bit
// big-endian byte length plus UTF-8 bytes. selected_artifact contributes
// the stable ASCII value "source" or "trusted-bytecode". The result is
// "sha256:" plus the lowercase hex SHA-256 of exactly those bytes. This
// framing is locale-independent and unambiguous; see the Host design review
// §6.3.

#include "host/config.h"
#include "host/generation_identity.h"

#include <jansson.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &digest_size,
                   EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(71);
    for (unsigned int i = 0; i < digest_size; ++i) {
        result.push_back(kHex[digest[i] >> 4]);
        result.push_back(kHex[digest[i] & 0x0f]);
    }
    return result;
}

std::string compute_binding_manifest_digest(std::string_view json) {
    // The schema validation is the gate; the digest is computed over the
    // canonical serialization so key order in the file never changes it.
    const ConfigValidationResult validation = validate_binding_manifest(json);
    if (!validation.ok) {
        return {};
    }
    json_error_t parse_error;
    json_t* root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr) {
        return {};
    }
    char* canonical = json_dumps(
        root, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
    json_decref(root);
    if (canonical == nullptr) {
        return {};
    }
    const std::string digest = sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(canonical),
        std::strlen(canonical)));
    std::free(canonical);
    return digest;
}

namespace {

void append_length_prefixed(std::vector<std::uint8_t> &output,
                            std::string_view value) {
    const std::uint32_t size = static_cast<std::uint32_t>(value.size());
    output.push_back(static_cast<std::uint8_t>(size >> 24));
    output.push_back(static_cast<std::uint8_t>(size >> 16));
    output.push_back(static_cast<std::uint8_t>(size >> 8));
    output.push_back(static_cast<std::uint8_t>(size));
    output.insert(output.end(), value.begin(), value.end());
}

}  // namespace

std::string compute_generation_digest(const GenerationIdentityInput &input) {
    static constexpr char kDomain[] = "capsid-generation-v1\0";
    std::vector<std::uint8_t> message(
        reinterpret_cast<const std::uint8_t *>(kDomain),
        reinterpret_cast<const std::uint8_t *>(kDomain) + sizeof(kDomain) - 1);

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
    return sha256_hex(message);

}

std::string compute_binding_set_digest(
    const std::vector<BindingSetDigestEntry> &entries) {
    std::vector<BindingSetDigestEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const BindingSetDigestEntry &a,
                 const BindingSetDigestEntry &b) {
                  return a.id < b.id;
              });
    std::vector<std::uint8_t> message;
    static constexpr char kDomain[] = "capsid-binding-set-v1\0";
    message.insert(
        message.end(),
        reinterpret_cast<const std::uint8_t *>(kDomain),
        reinterpret_cast<const std::uint8_t *>(kDomain) + sizeof(kDomain) - 1);
    for (const BindingSetDigestEntry &entry : sorted) {
        const std::string_view fields[] = {
            entry.id,
            entry.manifest_digest,
            entry.source_digest,
            entry.config_digest,
            entry.permission_digest,
            entry.profile_digest,
        };
        for (const std::string_view field : fields) {
            append_length_prefixed(message, field);
        }
        std::vector<std::string> keys = entry.secret_key_ids;
        std::sort(keys.begin(), keys.end());
        for (const std::string &key : keys) {
            append_length_prefixed(message, key);
        }
        append_length_prefixed(message, entry.secret_revision);
    }
    return sha256_hex(message);
}

}  // namespace capsid::host
