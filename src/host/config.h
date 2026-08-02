#ifndef CAPSID_HOST_CONFIG_H
#define CAPSID_HOST_CONFIG_H

#include <cstddef>
#include <string>
#include <string_view>

namespace capsid::host {

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

}  // namespace capsid::host

#endif
