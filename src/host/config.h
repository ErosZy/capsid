#ifndef CAPSID_HOST_CONFIG_H
#define CAPSID_HOST_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {

// Defined in policy_compiler.h; declared here so the parse boundary below
// can reference it without pulling the policy compiler into the schema.
struct AppRequest;

// Raw host.json / capsid.json resource limits. The byte limit is inclusive
// and applies before parsing. JSON nesting counts the root value as depth 1.
inline constexpr std::size_t kMaxConfigBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxConfigNesting = 64U;

// Binding v1 artifact limits (docs/binding-technical-design.md §2.1/§2.3):
// manifest.json, index.js and one binding's opaque config member.
inline constexpr std::size_t kMaxBindingManifestBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxBindingSourceBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxBindingGenerationSourceBytes =
    64U * 1024U * 1024U;
inline constexpr std::size_t kMaxBindingConfigBytes = 256U * 1024U;

enum class ConfigDocument {
    kHost,
    kApplication,
};

enum class ConfigErrorCode {
    kNone,
    kInvalidJson,
    kDuplicateKey,
    kUnknownField,
    kInvalidValue,
    kResourceLimit,
};

struct ConfigError {
    ConfigErrorCode code = ConfigErrorCode::kNone;
    std::string path;
    std::string message;
};

struct ConfigValidationResult {
    bool ok = false;
    ConfigError error;
};

// Pure, fail-closed validation boundary. On success, error must be empty and
// carry kNone. On failure, path is an RFC 6901 JSON Pointer to the rejected
// value and message is safe for an operator-facing diagnostic.
ConfigValidationResult validate_config_json(
    ConfigDocument document,
    std::string_view json);

// Binding manifest validation (docs/binding-technical-design.md §2.2/§4.1):
// the capsid/binding-v1 document with fixed sandbox profiles, the build's
// grantable module set, typed net/fs/env/stdio permissions and the
// profile-permission consistency rule. The byte limit (kMaxBindingManifest
// bytes) applies before parsing, matching the registry scan limit.
ConfigValidationResult validate_binding_manifest(std::string_view json);

// Binding ID grammar: exactly [a-z][a-z0-9-]{0,62}, 1..63 bytes (§2.1).
// Shared by the configuration schema (bindings map keys) and the
// bindingsRoot scanner (package directory names).
bool valid_binding_id(std::string_view value);

// Parses capsid.json (the authoritative capsid/app-v1 shape) into the
// AppRequest. The frozen schema boundary (validate_config_json) runs first
// in the deploy pipeline; this parse maps the schema's fields onto the
// request and stays fail-closed on any shape it cannot map. Shared with the
// local-capsid.json data planes (single-worker / static-pool) so one
// document grammar cannot diverge into two.
bool parse_app_request(const std::vector<std::uint8_t>& bytes,
                       AppRequest* app,
                       std::string* error);

// Shared byte-size and duration grammars (worker.memoryMax, request
// timeouts, pool.queueTimeout). Exported for the local-capsid.json data
// planes so both modes reject the same malformed values.
bool parse_size_bytes(const std::string& text, std::uint64_t* out);
bool parse_duration_ms(const std::string& text, std::uint64_t* out);

}  // namespace capsid::host

#endif
