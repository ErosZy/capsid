#ifndef CAPSID_HOST_SECRET_SNAPSHOT_H
#define CAPSID_HOST_SECRET_SNAPSHOT_H

#include "capsid/runtime.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {

// These limits deliberately match the Runtime capability-policy boundary.
inline constexpr std::size_t kMaxEnvironmentEntries = 256U;
inline constexpr std::size_t kMaxEnvironmentNameBytes = 256U;
inline constexpr std::size_t kMaxEnvironmentValueBytes = 16U * 1024U;
// Budget for the actual env name/value bytes entering the Runtime snapshot.
inline constexpr std::size_t kMaxEnvironmentSnapshotBytes = 48U * 1024U;
// Independent budget for the committed metadata document
// (effective_environment_json: names, source kinds, key IDs and opaque
// revisions). The metadata carries identifier material that can legitimately
// exceed the value budget, so it gets its own ceiling; values never enter
// this document.
inline constexpr std::size_t kMaxEnvironmentMetadataJsonBytes =
    256U * 1024U;
inline constexpr std::size_t kMaxSecretKeyBytes = 128U;
inline constexpr std::size_t kMaxSecretRevisionBytes = 256U;

enum class EnvironmentValueSource {
    kLiteral,
    kSecret,
};

// Parsed App request. Exactly one of value/value_from must be present.
struct EnvironmentRequest {
    std::string_view name;
    std::optional<std::string_view> value;
    std::optional<std::string_view> value_from;
};

// Output of the later safe-read provider boundary. key_id is not a path and
// uses only [A-Za-z0-9._-] without "..". opaque_revision uses only
// [A-Za-z0-9._:@+-] and must contain no secret value or bare digest of it.
// The value may enter only the Runtime snapshot.
struct ResolvedSecret {
    std::string_view key_id;
    std::string_view value;
    std::string_view opaque_revision;
};

struct SecretSnapshotCompileInput {
    std::string_view application_id;
    bool host_allows_env_module = false;
    bool app_requests_env_module = false;
    // Exact names and one-trailing-'*' patterns accepted by the Runtime.
    std::span<const std::string_view> host_environment_names;
    std::span<const EnvironmentRequest> requests;
    // Must contain exactly the distinct keys referenced through value_from:
    // no missing, duplicate or unused material.
    std::span<const ResolvedSecret> resolved_secrets;
};

enum class SecretSnapshotErrorCode {
    kNone,
    kInvalidEnvironmentName,
    kInvalidEnvironmentPattern,
    kInvalidEnvironmentRequest,
    kInvalidSecretKey,
    kInvalidSecretRevision,
    kModuleDenied,
    kPermissionDenied,
    kDuplicateEnvironmentName,
    kDuplicateResolvedSecret,
    kMissingResolvedSecret,
    kUnexpectedResolvedSecret,
    kInvalidValue,
    kValueTooLarge,
    kTooManyEntries,
    kSnapshotTooLarge,
};

struct SecretSnapshotError {
    SecretSnapshotErrorCode code = SecretSnapshotErrorCode::kNone;
    // RFC 6901 pointer into the App environment request or the synthetic
    // /resolvedSecrets provider input. Diagnostics must never contain literal
    // or resolved values.
    std::string path;
    std::string message;
};

struct EnvironmentSnapshotEntry {
    std::string name;
    std::string value;
};

struct EnvironmentSnapshotMetadata {
    std::string name;
    EnvironmentValueSource source = EnvironmentValueSource::kLiteral;
    // Empty for literal values. Secret values never appear here.
    std::string secret_key_id;
    std::string opaque_revision;
};

struct SecretSnapshotCompileResult;

class SecretSnapshot {
public:
    SecretSnapshot() = default;

    const std::vector<EnvironmentSnapshotEntry>& entries() const {
        return entries_;
    }
    const std::vector<EnvironmentSnapshotMetadata>& metadata() const {
        return metadata_;
    }
    const std::string& secret_revision() const {
        return secret_revision_;
    }
    // Canonical one-line JSON. It contains names, source kind, secret key IDs
    // and opaque revisions, but no literal or secret values.
    const std::string& effective_environment_json() const {
        return effective_environment_json_;
    }

    // Descriptors point into this snapshot and remain valid until it is moved,
    // assigned or destroyed. capsid_worker_spawn() copies them synchronously.
    std::vector<capsid_env_entry> runtime_entries() const;

private:
    friend struct SecretSnapshotCompileResult;
    friend SecretSnapshotCompileResult compile_secret_snapshot(
        const SecretSnapshotCompileInput& input);

    std::vector<EnvironmentSnapshotEntry> entries_;
    std::vector<EnvironmentSnapshotMetadata> metadata_;
    std::string secret_revision_;
    std::string effective_environment_json_;
};

struct SecretSnapshotCompileResult {
    bool ok = false;
    SecretSnapshot snapshot;
    SecretSnapshotError error;
};

// Pure policy/snapshot compiler. It deep-copies successful values, sorts
// entries by environment name and fails atomically with an empty snapshot.
SecretSnapshotCompileResult compile_secret_snapshot(
    const SecretSnapshotCompileInput& input);

}  // namespace capsid::host

#endif
