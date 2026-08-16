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

// Parses capsid.json (the authoritative capsid/app-v1 shape) into the
// AppRequest. The frozen schema boundary (validate_config_json) runs first
// in the deploy pipeline; this parse maps the schema's fields onto the
// request and stays fail-closed on any shape it cannot map. Shared with the
// local-capsid.json data planes (single-worker / static-pool) so one
// document grammar cannot diverge into two.
bool parse_app_request(const std::vector<std::uint8_t>& bytes,
                       AppRequest* app,
                       std::string* error);

}  // namespace capsid::host

#endif
