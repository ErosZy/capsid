// Active App state: strict parse/encode of active.json, the frozen atomic
// persistence order, and crash-safe recovery.
//
// The document grammar is locked by src/host/active_state.h; ID checks use
// hand-written ASCII ranges so validation is locale-independent. Parsing
// reuses the vendored Jansson parser (JSON_REJECT_DUPLICATES and
// JSON_ALLOW_NUL, mirroring the config front end): duplicate keys and NUL
// object keys are parse errors, while NUL bytes inside values reach the
// ID validators and fail closed there.
//
// The POSIX filesystem adapter is intentionally out of M0 scope; the
// ActiveStateFilesystem interface is the only boundary, so ordering and
// crash/failure injection are deterministic.

#include "host/active_state.h"

#include <jansson.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {
namespace {

using ErrorCode = ActiveStateErrorCode;

void set_error(ActiveStateError &error,
               ErrorCode code,
               std::string path,
               std::string message) {
    error.code = code;
    error.path = std::move(path);
    error.message = std::move(message);
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

// Hand-written ASCII range checks: never locale-dependent isalnum.
bool ascii_digit(char c) {
    return c >= '0' && c <= '9';
}
bool ascii_lower(char c) {
    return c >= 'a' && c <= 'z';
}
bool ascii_upper(char c) {
    return c >= 'A' && c <= 'Z';
}
bool ascii_hex_lower(char c) {
    return ascii_digit(c) || (c >= 'a' && c <= 'f');
}

// Application IDs: [a-z0-9][a-z0-9._-]{0,62}.
bool valid_application_id(std::string_view value) {
    if (value.empty() || value.size() > 63) {
        return false;
    }
    if (!(ascii_lower(value[0]) || ascii_digit(value[0]))) {
        return false;
    }
    for (const char c : value) {
        if (!(ascii_lower(c) || ascii_digit(c) || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

// Version IDs: [A-Za-z0-9][A-Za-z0-9._-]{0,127}.
bool valid_version_id(std::string_view value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    if (!(ascii_lower(value[0]) || ascii_upper(value[0]) ||
          ascii_digit(value[0]))) {
        return false;
    }
    for (const char c : value) {
        if (!(ascii_lower(c) || ascii_upper(c) || ascii_digit(c) ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// Generation digests: "sha256:" plus 64 lowercase hex digits.
bool valid_generation(std::string_view value) {
    if (value.size() != 71 || value.substr(0, 7) != "sha256:") {
        return false;
    }
    for (const char c : value.substr(7)) {
        if (!ascii_hex_lower(c)) {
            return false;
        }
    }
    return true;
}

// Operation IDs: [A-Za-z0-9][A-Za-z0-9_-]{0,63}.
bool valid_operation_id(std::string_view value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    if (!(ascii_lower(value[0]) || ascii_upper(value[0]) ||
          ascii_digit(value[0]))) {
        return false;
    }
    for (const char c : value) {
        if (!(ascii_lower(c) || ascii_upper(c) || ascii_digit(c) ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// The JSON member names in canonical field order.
constexpr std::string_view kFieldNames[] = {
    "schema", "app", "state", "version",
    "generation", "previousVersion", "previousGeneration", "reason",
};
constexpr std::size_t kSchemaField = 0;
constexpr std::size_t kAppField = 1;
constexpr std::size_t kStateField = 2;
constexpr std::size_t kVersionField = 3;
constexpr std::size_t kGenerationField = 4;
constexpr std::size_t kPreviousVersionField = 5;
constexpr std::size_t kPreviousGenerationField = 6;
constexpr std::size_t kReasonField = 7;
constexpr std::size_t kFieldCount = 8;

std::string_view state_name(ActiveServiceState state) {
    switch (state) {
    case ActiveServiceState::kActive:
        return "active";
    case ActiveServiceState::kRetired:
        return "retired";
    case ActiveServiceState::kQuarantined:
        return "quarantined";
    }
    return "";
}

// Validates the document struct and reports the first failing field. Used
// by both the encoder and the state checks of the parser.
bool validate_document(const ActiveStateDocument &document,
                       ActiveStateError &error) {
    if (!valid_application_id(document.application)) {
        set_error(error, ErrorCode::kInvalidValue, "/app",
                  "invalid App ID");
        return false;
    }
    switch (document.state) {
    case ActiveServiceState::kActive:
        if (!valid_version_id(document.version)) {
            set_error(error, ErrorCode::kInvalidValue, "/version",
                      "invalid Version ID");
            return false;
        }
        if (!valid_generation(document.generation)) {
            set_error(error, ErrorCode::kInvalidValue, "/generation",
                      "invalid generation digest");
            return false;
        }
        if (!document.previous_version.empty()) {
            set_error(error, ErrorCode::kInvalidValue, "/previousVersion",
                      "tombstone field on an active state");
            return false;
        }
        if (!document.previous_generation.empty()) {
            set_error(error, ErrorCode::kInvalidValue,
                      "/previousGeneration",
                      "tombstone field on an active state");
            return false;
        }
        if (!document.reason.empty()) {
            set_error(error, ErrorCode::kInvalidValue, "/reason",
                      "reason on an active state");
            return false;
        }
        return true;
    case ActiveServiceState::kRetired:
        if (!valid_version_id(document.previous_version)) {
            set_error(error, ErrorCode::kInvalidValue, "/previousVersion",
                      "invalid previous Version ID");
            return false;
        }
        if (!valid_generation(document.previous_generation)) {
            set_error(error, ErrorCode::kInvalidValue,
                      "/previousGeneration",
                      "invalid previous generation digest");
            return false;
        }
        if (!document.version.empty()) {
            set_error(error, ErrorCode::kInvalidValue, "/version",
                      "version on a retired state");
            return false;
        }
        if (!document.generation.empty()) {
            set_error(error, ErrorCode::kInvalidValue, "/generation",
                      "generation on a retired state");
            return false;
        }
        if (!document.reason.empty()) {
            set_error(error, ErrorCode::kInvalidValue, "/reason",
                      "reason on a retired state");
            return false;
        }
        return true;
    case ActiveServiceState::kQuarantined:
        if (!valid_version_id(document.version)) {
            set_error(error, ErrorCode::kInvalidValue, "/version",
                      "invalid Version ID");
            return false;
        }
        if (!valid_generation(document.generation)) {
            set_error(error, ErrorCode::kInvalidValue, "/generation",
                      "invalid generation digest");
            return false;
        }
        if (document.reason != kCrashBudgetExceededReason) {
            set_error(error, ErrorCode::kInvalidValue, "/reason",
                      "unknown quarantine reason");
            return false;
        }
        if (!document.previous_version.empty()) {
            set_error(error, ErrorCode::kInvalidValue, "/previousVersion",
                      "tombstone field on a quarantined state");
            return false;
        }
        if (!document.previous_generation.empty()) {
            set_error(error, ErrorCode::kInvalidValue,
                      "/previousGeneration",
                      "tombstone field on a quarantined state");
            return false;
        }
        return true;
    }
    set_error(error, ErrorCode::kInvalidValue, "/state",
              "unknown service state");
    return false;
}

// Whether a field is state-inapplicable and must therefore be absent from
// the JSON document, regardless of its value (an empty string is still a
// forbidden appearance).
bool field_forbidden_for_state(ActiveServiceState state, std::size_t field) {
    switch (state) {
    case ActiveServiceState::kActive:
        return field == kPreviousVersionField ||
               field == kPreviousGenerationField || field == kReasonField;
    case ActiveServiceState::kRetired:
        return field == kVersionField || field == kGenerationField ||
               field == kReasonField;
    case ActiveServiceState::kQuarantined:
        return field == kPreviousVersionField ||
               field == kPreviousGenerationField;
    }
    return false;
}

std::string build_canonical_json(const ActiveStateDocument &document) {
    // All fields are grammar-restricted, so no JSON escaping is needed.
    std::string json = "{\"schema\":\"";
    json += kActiveStateSchema;
    json += "\",\"app\":\"";
    json += document.application;
    json += "\",\"state\":\"";
    json += state_name(document.state);
    switch (document.state) {
    case ActiveServiceState::kActive:
        json += "\",\"version\":\"" + document.version +
                "\",\"generation\":\"" + document.generation + "\"}";
        break;
    case ActiveServiceState::kRetired:
        json += "\",\"previousVersion\":\"" + document.previous_version +
                "\",\"previousGeneration\":\"" +
                document.previous_generation + "\"}";
        break;
    case ActiveServiceState::kQuarantined:
        json += "\",\"version\":\"" + document.version +
                "\",\"generation\":\"" + document.generation +
                "\",\"reason\":\"" + document.reason + "\"}";
        break;
    }
    return json;
}

}  // namespace

ActiveStateDocumentResult parse_active_state_json(
    std::string_view expected_application,
    std::string_view json) {
    ActiveStateDocumentResult result;

    if (json.size() > kMaxActiveStateBytes) {
        set_error(result.error, ErrorCode::kResourceLimit, "",
                  "active state exceeds the size limit");
        return result;
    }

    json_error_t parse_error;
    json_t *root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES | JSON_ALLOW_NUL,
                              &parse_error);
    if (root == nullptr) {
        const enum json_error_code parse_code = json_error_code(&parse_error);
        set_error(result.error,
                  parse_code == json_error_duplicate_key
                      ? ErrorCode::kDuplicateKey
                      : ErrorCode::kInvalidJson,
                  "", "invalid active state document");
        return result;
    }
    if (!json_is_object(root)) {
        set_error(result.error, ErrorCode::kInvalidJson, "",
                  "active state must be a JSON object");
        json_decref(root);
        return result;
    }

    // Presence is tracked separately from values: a state-inapplicable
    // field is rejected by appearance, even when its value is the empty
    // string, which must not be confused with the field being absent.
    std::array<bool, kFieldCount> present{};
    std::string values[kFieldCount];
    for (void *iter = json_object_iter(root); iter != nullptr;
         iter = json_object_iter_next(root, iter)) {
        const std::string_view key(json_object_iter_key(iter),
                                   json_object_iter_key_len(iter));
        // NUL bytes in keys are rejected by the upstream parser; this check
        // is defensive and must never splice the key into the path.
        if (key.find('\0') != std::string_view::npos) {
            set_error(result.error, ErrorCode::kInvalidValue, "",
                      "object key contains a NUL byte");
            json_decref(root);
            return result;
        }
        std::size_t index = 0;
        for (; index < kFieldCount; ++index) {
            if (key == kFieldNames[index]) {
                break;
            }
        }
        if (index == kFieldCount) {
            set_error(result.error, ErrorCode::kUnknownField,
                      "/" + escape_pointer_component(key),
                      "unknown active state field");
            json_decref(root);
            return result;
        }
        json_t *value = json_object_iter_value(iter);
        if (!json_is_string(value)) {
            set_error(result.error, ErrorCode::kInvalidValue,
                      "/" + std::string(kFieldNames[index]),
                      "active state field must be a string");
            json_decref(root);
            return result;
        }
        present[index] = true;
        values[index].assign(json_string_value(value),
                             json_string_length(value));
    }
    json_decref(root);

    if (values[kSchemaField] != kActiveStateSchema) {
        set_error(result.error, ErrorCode::kInvalidValue, "/schema",
                  "unsupported active state schema");
        return result;
    }
    if (values[kAppField] != expected_application ||
        !valid_application_id(values[kAppField])) {
        set_error(result.error, ErrorCode::kInvalidValue, "/app",
                  "invalid App ID");
        return result;
    }
    ActiveServiceState state;
    if (values[kStateField] == "active") {
        state = ActiveServiceState::kActive;
    } else if (values[kStateField] == "retired") {
        state = ActiveServiceState::kRetired;
    } else if (values[kStateField] == "quarantined") {
        state = ActiveServiceState::kQuarantined;
    } else {
        set_error(result.error, ErrorCode::kInvalidValue, "/state",
                  "unknown service state");
        return result;
    }

    // Field exclusivity is decided by presence, not by string emptiness: a
    // state-inapplicable field that appears with an empty value is still a
    // forbidden appearance.
    for (std::size_t field = 0; field < kFieldCount; ++field) {
        if (present[field] && field_forbidden_for_state(state, field)) {
            set_error(result.error, ErrorCode::kInvalidValue,
                      "/" + std::string(kFieldNames[field]),
                      "state-inapplicable field");
            return result;
        }
    }

    ActiveStateDocument document;
    document.state = state;
    document.application = values[kAppField];
    document.version = values[kVersionField];
    document.generation = values[kGenerationField];
    document.previous_version = values[kPreviousVersionField];
    document.previous_generation = values[kPreviousGenerationField];
    document.reason = values[kReasonField];
    if (!validate_document(document, result.error)) {
        return result;
    }

    result.ok = true;
    result.document = std::move(document);
    result.canonical_json = build_canonical_json(result.document);
    return result;
}

ActiveStateDocumentResult encode_active_state_json(
    const ActiveStateDocument &document) {
    ActiveStateDocumentResult result;
    if (!validate_document(document, result.error)) {
        return result;
    }
    result.ok = true;
    result.document = document;
    result.canonical_json = build_canonical_json(document);
    return result;
}

ActiveStatePersistResult persist_active_state(
    const ActiveStateDocument &document,
    std::string_view operation_id,
    ActiveStateFilesystem &filesystem) {
    ActiveStatePersistResult result;

    if (!valid_operation_id(operation_id)) {
        result.error.code = ErrorCode::kInvalidValue;
        result.error.path = "/operationId";
        result.error.message = "invalid operation ID";
        return result;
    }

    const ActiveStateDocumentResult encoded =
        encode_active_state_json(document);
    if (!encoded.ok) {
        result.error = encoded.error;
        return result;
    }

    // Only an active pointer is checked against generation completeness;
    // retired and quarantined tombstones never inspect a generation.
    if (document.state == ActiveServiceState::kActive) {
        const GenerationCompleteness completeness =
            filesystem.inspect_generation(document.generation);
        if (completeness != GenerationCompleteness::kComplete) {
            result.error.code =
                completeness == GenerationCompleteness::kError
                    ? ErrorCode::kStorageError
                    : ErrorCode::kGenerationNotComplete;
            result.error.path = "/generation";
            result.error.message =
                completeness == GenerationCompleteness::kError
                    ? "generation inspection failed"
                    : "generation is not complete";
            return result;
        }
    }

    result.temp_name = "active.json.tmp." + std::string(operation_id);
    const ActiveStateIoStatus created =
        filesystem.create_active_temp_exclusive(result.temp_name);
    if (created == ActiveStateIoStatus::kAlreadyExists) {
        result.error.code = ErrorCode::kTemporaryFileExists;
        result.error.path = "/" + result.temp_name;
        result.error.message = "exclusive active-state temp already exists";
        return result;
    }
    if (created != ActiveStateIoStatus::kOk) {
        result.error.code = ErrorCode::kStorageError;
        result.error.path = "/" + result.temp_name;
        result.error.message = "could not create the active-state temp";
        return result;
    }
    if (filesystem.write_active_temp(result.temp_name, encoded.canonical_json) !=
        ActiveStateIoStatus::kOk) {
        result.error.code = ErrorCode::kStorageError;
        result.error.path = "/" + result.temp_name;
        result.error.message = "could not write the active-state temp";
        return result;
    }
    if (filesystem.sync_active_temp(result.temp_name) != ActiveStateIoStatus::kOk) {
        result.error.code = ErrorCode::kStorageError;
        result.error.path = "/" + result.temp_name;
        result.error.message = "could not sync the active-state temp";
        return result;
    }
    if (filesystem.rename_temp_over_active(result.temp_name) !=
        ActiveStateIoStatus::kOk) {
        result.error.code = ErrorCode::kStorageError;
        result.error.path = "/" + result.temp_name;
        result.error.message = "could not rename the active-state temp";
        return result;
    }
    result.active_name_replaced = true;

    if (filesystem.sync_app_directory() != ActiveStateIoStatus::kOk) {
        // The rename completed but durability of the directory entry is
        // unknown: the caller must reconcile instead of publishing the new
        // in-memory Registry snapshot.
        result.error.code = ErrorCode::kDurabilityUncertain;
        result.error.path = "";
        result.error.message =
            "active.json was replaced but the directory sync failed";
        return result;
    }

    result.ok = true;
    result.error.code = ErrorCode::kNone;
    return result;
}

ActiveStateRecoveryResult recover_active_state(
    std::string_view expected_application,
    ActiveStateFilesystem &filesystem) {
    ActiveStateRecoveryResult result;

    // Stale temp cleanup is best effort and never changes the selected
    // state; a failure is surfaced as an operator warning only.
    if (filesystem.cleanup_stale_active_temps() != ActiveStateIoStatus::kOk) {
        result.stale_temp_cleanup_failed = true;
    }

    const ActiveStateReadResult read = filesystem.read_active_file();
    if (read.status == ActiveStateIoStatus::kNotFound) {
        result.ok = true;
        result.action = ActiveStateRecoveryAction::kNone;
        result.error.code = ErrorCode::kNone;
        return result;
    }
    if (read.status != ActiveStateIoStatus::kOk) {
        result.error.code = ErrorCode::kStorageError;
        result.error.path = "";
        result.error.message = "could not read active.json";
        return result;
    }

    const ActiveStateDocumentResult parsed =
        parse_active_state_json(expected_application, read.bytes);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }
    result.document = parsed.document;

    switch (parsed.document.state) {
    case ActiveServiceState::kActive: {
        // Only a COMPLETE generation may be activated; nothing scans for a
        // "latest" generation because the interface exposes no listing API.
        const GenerationCompleteness completeness =
            filesystem.inspect_generation(parsed.document.generation);
        if (completeness != GenerationCompleteness::kComplete) {
            result.error.code =
                completeness == GenerationCompleteness::kError
                    ? ErrorCode::kStorageError
                    : ErrorCode::kGenerationNotComplete;
            result.error.path = "/generation";
            result.error.message =
                completeness == GenerationCompleteness::kError
                    ? "generation inspection failed"
                    : "active generation is not complete";
            return result;
        }
        result.ok = true;
        result.action = ActiveStateRecoveryAction::kActivate;
        result.error.code = ErrorCode::kNone;
        return result;
    }
    case ActiveServiceState::kRetired:
        result.ok = true;
        result.action = ActiveStateRecoveryAction::kKeepRetired;
        result.error.code = ErrorCode::kNone;
        return result;
    case ActiveServiceState::kQuarantined:
        result.ok = true;
        result.action = ActiveStateRecoveryAction::kKeepQuarantined;
        result.error.code = ErrorCode::kNone;
        return result;
    }
    result.error.code = ErrorCode::kInvalidValue;
    result.error.message = "unknown service state";
    return result;
}

}  // namespace capsid::host
