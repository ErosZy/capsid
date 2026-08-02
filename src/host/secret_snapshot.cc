// Host-side environment snapshot compiler.
//
// Pure policy/snapshot compilation: Host allowlist x App request x resolved
// secret material. Successful output is deep-copied, sorted by environment
// name and canonical; every failure returns an atomically empty snapshot.
// Diagnostics, the effective environment JSON and the secret revision never
// contain literal or secret values — only names, key IDs and opaque
// revisions.
//
// The secret revision framing (NUL-terminated domain, 32-bit big-endian
// length prefixes) and the environment grammar mirror the Runtime authority
// in src/capability_policy.cc.

#include "host/secret_snapshot.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {
namespace {

using ErrorCode = SecretSnapshotErrorCode;

void set_error(SecretSnapshotCompileResult &result,
               ErrorCode code,
               std::string path,
               std::string message) {
    result.ok = false;
    result.error.code = code;
    result.error.path = std::move(path);
    result.error.message = std::move(message);
}

// RFC 6901 JSON Pointer escaping for dynamic member names.
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

void append_length_prefixed(std::vector<std::uint8_t> &output,
                            std::string_view value) {
    const std::uint32_t size = static_cast<std::uint32_t>(value.size());
    output.push_back(static_cast<std::uint8_t>(size >> 24));
    output.push_back(static_cast<std::uint8_t>(size >> 16));
    output.push_back(static_cast<std::uint8_t>(size >> 8));
    output.push_back(static_cast<std::uint8_t>(size));
    output.insert(output.end(), value.begin(), value.end());
}

// Environment name/pattern grammar mirrors
// capsid::capability_policy::valid_env_pattern / valid_env_name.
bool valid_environment_pattern(std::string_view value) {
    if (value.empty() || value.size() > kMaxEnvironmentNameBytes) {
        return false;
    }
    std::size_t limit = value.size();
    if (value.back() == '*') {
        --limit;
    }
    if (limit == 0) {
        return value == "*";
    }
    const unsigned char first = static_cast<unsigned char>(value[0]);
    if (!(std::isalpha(first) || first == '_')) {
        return false;
    }
    for (std::size_t index = 0; index < limit; ++index) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if (!(std::isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return value.find('*') == std::string_view::npos ||
           value.find('*') == value.size() - 1;
}

bool valid_environment_name(std::string_view value) {
    return value.find('*') == std::string_view::npos &&
           valid_environment_pattern(value);
}

// Secret key IDs: exactly [A-Za-z0-9._-], no empty or dot component
// (".."), 1..kMaxSecretKeyBytes. No other punctuation is allowed.
bool valid_secret_key_id(std::string_view value) {
    if (value.empty() || value.size() > kMaxSecretKeyBytes) {
        return false;
    }
    for (const char c : value) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return value.find("..") == std::string_view::npos;
}

// Opaque revisions are safe metadata: exactly [A-Za-z0-9._:@+-],
// 1..kMaxSecretRevisionBytes. No whitespace, quotes, backslashes or other
// punctuation.
bool valid_opaque_revision(std::string_view value) {
    if (value.empty() || value.size() > kMaxSecretRevisionBytes) {
        return false;
    }
    for (const char c : value) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == ':' ||
              ch == '@' || ch == '+' || ch == '-')) {
            return false;
        }
    }
    return true;
}

bool has_nul_byte(std::string_view bytes) {
    return bytes.find('\0') != std::string_view::npos;
}

// Minimal strict UTF-8 validation: overlong forms, surrogates and
// out-of-range code points are rejected.
bool valid_utf8(std::string_view bytes) {
    std::size_t index = 0;
    while (index < bytes.size()) {
        const unsigned char lead = static_cast<unsigned char>(bytes[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }
        std::size_t continuation = 0;
        std::uint32_t code_point = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuation = 1;
            code_point = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuation = 2;
            code_point = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuation = 3;
            code_point = lead & 0x07;
        } else {
            return false;
        }
        if (index + continuation >= bytes.size()) {
            return false;
        }
        for (std::size_t step = 1; step <= continuation; ++step) {
            const unsigned char ch =
                static_cast<unsigned char>(bytes[index + step]);
            if ((ch & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (ch & 0x3f);
        }
        // Overlong and surrogate encodings.
        if ((continuation == 1 && code_point < 0x80) ||
            (continuation == 2 && code_point < 0x800) ||
            (continuation == 3 && code_point < 0x10000) ||
            (code_point >= 0xd800 && code_point <= 0xdfff) ||
            code_point > 0x10ffff) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

// Whether an environment name matches a validated Host allowlist entry
// (exact match or one-trailing-'*' prefix).
bool host_allows_name(std::span<const std::string_view> patterns,
                      std::string_view name) {
    for (const std::string_view pattern : patterns) {
        if (pattern == "*") {
            return true;
        }
        if (pattern == name) {
            return true;
        }
        if (pattern.size() > 1 && pattern.back() == '*' &&
            name.size() >= pattern.size() - 1 &&
            name.substr(0, pattern.size() - 1) ==
                pattern.substr(0, pattern.size() - 1)) {
            return true;
        }
    }
    return false;
}

// Minimal JSON string escaping for metadata fields (names are grammar-
// restricted, but key IDs and revisions may contain quotes or backslashes).
std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

}  // namespace

SecretSnapshotCompileResult compile_secret_snapshot(
    const SecretSnapshotCompileInput &input) {
    SecretSnapshotCompileResult result;

    // The module gate only applies when the App actually requests
    // environment entries; an App with no requests flows through the same
    // provider validation, revision and canonical JSON construction below.
    if (!input.requests.empty() &&
        (!input.host_allows_env_module || !input.app_requests_env_module)) {
        set_error(result, ErrorCode::kModuleDenied, "/permissions/modules",
                  "capsid:env is not enabled on both sides");
        return result;
    }
    // Entry-count budget is enforced before any per-request processing.
    if (input.requests.size() > kMaxEnvironmentEntries) {
        set_error(result, ErrorCode::kTooManyEntries, "/permissions/env",
                  "too many environment entries");
        return result;
    }

    // Host allowlist entries must honor the Runtime grammar.
    for (std::size_t index = 0; index < input.host_environment_names.size();
         ++index) {
        if (!valid_environment_pattern(input.host_environment_names[index])) {
            set_error(result, ErrorCode::kInvalidEnvironmentPattern,
                      "/host/environmentNames/" + std::to_string(index),
                      "invalid Host environment pattern");
            return result;
        }
    }

    // Request validation: name grammar, duplicates, value sources and
    // per-source content limits. Paths point into /permissions/env.
    std::vector<std::string_view> request_names;
    request_names.reserve(input.requests.size());
    for (const EnvironmentRequest &request : input.requests) {
        const std::string entry_path =
            "/permissions/env/" + escape_pointer_component(request.name);
        if (!valid_environment_name(request.name)) {
            set_error(result, ErrorCode::kInvalidEnvironmentName, entry_path,
                      "invalid environment name");
            return result;
        }
        if (std::find(request_names.begin(), request_names.end(),
                      request.name) != request_names.end()) {
            set_error(result, ErrorCode::kDuplicateEnvironmentName,
                      entry_path, "duplicate environment name");
            return result;
        }
        request_names.push_back(request.name);

        const bool has_literal = request.value.has_value();
        const bool has_secret = request.value_from.has_value();
        if (has_literal == has_secret) {
            set_error(result, ErrorCode::kInvalidEnvironmentRequest,
                      entry_path,
                      "environment entry needs exactly one value source");
            return result;
        }
        if (has_literal) {
            if (has_nul_byte(*request.value)) {
                set_error(result, ErrorCode::kInvalidValue,
                          entry_path + "/value",
                          "environment value contains a NUL byte");
                return result;
            }
            if (!valid_utf8(*request.value)) {
                set_error(result, ErrorCode::kInvalidValue,
                          entry_path + "/value",
                          "environment value is not valid UTF-8");
                return result;
            }
            if (request.value->size() > kMaxEnvironmentValueBytes) {
                set_error(result, ErrorCode::kValueTooLarge,
                          entry_path + "/value",
                          "environment value exceeds the size limit");
                return result;
            }
        } else {
            if (!valid_secret_key_id(*request.value_from)) {
                set_error(result, ErrorCode::kInvalidSecretKey,
                          entry_path + "/valueFrom",
                          "invalid secret key ID");
                return result;
            }
        }
    }

    // Host permission matching.
    for (const std::string_view name : request_names) {
        if (!host_allows_name(input.host_environment_names, name)) {
            set_error(result, ErrorCode::kPermissionDenied,
                      "/permissions/env/" + escape_pointer_component(name),
                      "environment name is not allowed by the Host");
            return result;
        }
    }

    // Resolved secret validation: the resolved set must contain exactly the
    // distinct requested keys, with safe key IDs, values and revisions.
    // Value content errors point at the requesting request.
    std::map<std::string_view, const EnvironmentRequest *> requested_keys;
    for (const EnvironmentRequest &request : input.requests) {
        if (request.value_from.has_value()) {
            requested_keys.emplace(*request.value_from, &request);
        }
    }
    std::map<std::string_view, const ResolvedSecret *> resolved_by_key;
    for (std::size_t index = 0; index < input.resolved_secrets.size();
         ++index) {
        const ResolvedSecret &secret = input.resolved_secrets[index];
        // Key validity is established before any key-derived path is built,
        // so an invalid key is reported with a fixed index-based path and
        // never spliced into the diagnostic.
        if (!valid_secret_key_id(secret.key_id)) {
            set_error(result, ErrorCode::kInvalidSecretKey,
                      "/resolvedSecrets/" + std::to_string(index) + "/keyId",
                      "invalid resolved secret key ID");
            return result;
        }
        const std::string resolved_path =
            "/resolvedSecrets/" + escape_pointer_component(secret.key_id);
        if (resolved_by_key.find(secret.key_id) != resolved_by_key.end()) {
            set_error(result, ErrorCode::kDuplicateResolvedSecret,
                      "/resolvedSecrets/" + std::to_string(index),
                      "duplicate resolved secret");
            return result;
        }
        const auto requested = requested_keys.find(secret.key_id);
        if (requested == requested_keys.end()) {
            set_error(result, ErrorCode::kUnexpectedResolvedSecret,
                      resolved_path, "resolved secret was not requested");
            return result;
        }
        const std::string request_path =
            "/permissions/env/" +
            escape_pointer_component(requested->second->name) + "/valueFrom";
        if (has_nul_byte(secret.value)) {
            set_error(result, ErrorCode::kInvalidValue, request_path,
                      "secret value contains a NUL byte");
            return result;
        }
        if (!valid_utf8(secret.value)) {
            set_error(result, ErrorCode::kInvalidValue, request_path,
                      "secret value is not valid UTF-8");
            return result;
        }
        if (secret.value.size() > kMaxEnvironmentValueBytes) {
            set_error(result, ErrorCode::kValueTooLarge, request_path,
                      "secret value exceeds the size limit");
            return result;
        }
        if (!valid_opaque_revision(secret.opaque_revision)) {
            set_error(result, ErrorCode::kInvalidSecretRevision,
                      resolved_path + "/opaqueRevision",
                      "unsafe secret revision");
            return result;
        }
        resolved_by_key.emplace(secret.key_id, &secret);
    }
    for (const auto &[key, request] : requested_keys) {
        if (resolved_by_key.find(key) == resolved_by_key.end()) {
            set_error(result, ErrorCode::kMissingResolvedSecret,
                      "/permissions/env/" +
                          escape_pointer_component(request->name) +
                          "/valueFrom",
                      "resolved secret is missing");
            return result;
        }
    }

    // Build the snapshot: total byte budget first, then sort by environment
    // name and construct owned storage. (The entry-count budget was already
    // enforced before per-request processing.)
    std::size_t total_bytes = 0;
    for (const EnvironmentRequest &request : input.requests) {
        const std::size_t value_size =
            request.value.has_value() ? request.value->size()
                                      : resolved_by_key[*request.value_from]
                                            ->value.size();
        total_bytes += request.name.size() + value_size;
    }
    if (total_bytes > kMaxEnvironmentSnapshotBytes) {
        set_error(result, ErrorCode::kSnapshotTooLarge, "/permissions/env",
                  "environment snapshot exceeds the size limit");
        return result;
    }

    struct SortedEntry {
        std::string name;
        std::string value;
        EnvironmentValueSource source;
        std::string secret_key_id;
        std::string opaque_revision;
    };
    std::vector<SortedEntry> sorted;
    sorted.reserve(input.requests.size());
    for (const EnvironmentRequest &request : input.requests) {
        SortedEntry entry;
        entry.name = std::string(request.name);
        if (request.value.has_value()) {
            entry.value = std::string(*request.value);
            entry.source = EnvironmentValueSource::kLiteral;
        } else {
            const ResolvedSecret &secret = *resolved_by_key[*request.value_from];
            entry.value = std::string(secret.value);
            entry.source = EnvironmentValueSource::kSecret;
            entry.secret_key_id = std::string(secret.key_id);
            entry.opaque_revision = std::string(secret.opaque_revision);
        }
        sorted.push_back(std::move(entry));
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const SortedEntry &left, const SortedEntry &right) {
                  return left.name < right.name;
              });

    result.snapshot.entries_.reserve(sorted.size());
    result.snapshot.metadata_.reserve(sorted.size());
    std::string environment_json = "{\"environment\":[";
    bool first_entry = true;
    std::vector<std::uint8_t> revision_message;
    {
        static constexpr char kRevisionDomain[] =
            "capsid-secret-revision-v1\0";
        revision_message.assign(
            reinterpret_cast<const std::uint8_t *>(kRevisionDomain),
            reinterpret_cast<const std::uint8_t *>(kRevisionDomain) +
                sizeof(kRevisionDomain) - 1);
        append_length_prefixed(revision_message, input.application_id);
    }
    for (const SortedEntry &entry : sorted) {
        result.snapshot.entries_.push_back(
            EnvironmentSnapshotEntry{entry.name, entry.value});
        EnvironmentSnapshotMetadata metadata;
        metadata.name = entry.name;
        metadata.source = entry.source;
        metadata.secret_key_id = entry.secret_key_id;
        metadata.opaque_revision = entry.opaque_revision;
        result.snapshot.metadata_.push_back(std::move(metadata));

        if (!first_entry) {
            environment_json += ',';
        }
        first_entry = false;
        environment_json += "{\"name\":\"" + json_escape(entry.name) +
                            "\",\"source\":\"" +
                            (entry.source == EnvironmentValueSource::kSecret
                                 ? "secret"
                                 : "literal") +
                            "\"";
        if (entry.source == EnvironmentValueSource::kSecret) {
            environment_json += ",\"keyId\":\"" +
                                json_escape(entry.secret_key_id) +
                                "\",\"revision\":\"" +
                                json_escape(entry.opaque_revision) + "\"";
            append_length_prefixed(revision_message, entry.name);
            append_length_prefixed(revision_message, entry.secret_key_id);
            append_length_prefixed(revision_message, entry.opaque_revision);
        }
        environment_json += '}';
    }
    environment_json += "]}";
    result.snapshot.effective_environment_json_ = std::move(environment_json);
    result.snapshot.secret_revision_ = sha256_hex(revision_message);

    result.ok = true;
    return result;
}

std::vector<capsid_env_entry> SecretSnapshot::runtime_entries() const {
    std::vector<capsid_env_entry> result;
    result.reserve(entries_.size());
    for (const EnvironmentSnapshotEntry &entry : entries_) {
        capsid_env_entry descriptor;
        descriptor.struct_size = sizeof(capsid_env_entry);
        descriptor.name = entry.name.c_str();
        descriptor.value = entry.value.c_str();
        descriptor.reserved = 0;
        result.push_back(descriptor);
    }
    return result;
}

}  // namespace capsid::host
