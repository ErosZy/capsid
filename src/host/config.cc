// Capsid Host configuration validation.
//
// Fail-closed, pure validation of host.json / capsid.json documents before
// any resource is touched. The schema is a recursive whitelist: a field is
// accepted only when this code declares it for its document kind, and every
// rejected field reports an RFC 6901 JSON Pointer to the offending value.
//
// Schema kinds: fixed objects (declared members), arrays (one element
// schema), dynamic maps (any key, one value schema), strings (optional
// exact value) and integers (optional lower bound). An empty fixed object
// accepts no members at all.
//
// Validation is phased so that the fail-closed decision does not depend on
// member iteration order:
//
//   Phase 0  reject documents larger than kMaxConfigBytes before parsing,
//            then parse with the vendored Jansson parser,
//            JSON_REJECT_DUPLICATES and the tightened JSON_PARSER_MAX_DEPTH
//            (= kMaxConfigNesting); duplicate keys, comments, trailing
//            commas, NaN/Infinity, trailing input and excessive nesting are
//            parse errors (nesting maps to kResourceLimit);
//   Phase 1  recursively reject every unknown field (pre-order, document
//            order), descending into arrays by index and dynamic maps by
//            escaped key. An unknown field wins over any type/value error
//            in a neighboring known field. Container type mismatches are
//            not descended into and are left to phase 2;
//   Phase 2  check types, required members, exact values, integer bounds
//            and cross-member equality. Array errors point at /index,
//            dynamic-map errors at the escaped key.
//
// Validation never inspects JSON text by keyword or string matching; the
// vendored Jansson parser is the only JSON front end.

#include "host/config.h"
#include "host/secret_snapshot.h"

#include <jansson.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace capsid::host {
namespace {

struct Schema;

struct Member {
    std::string_view name;
    const Schema* schema;
    bool required;
};

struct MemberPair {
    std::string_view first;
    std::string_view second;
};

struct Schema {
    enum class Kind { kObject, kString, kInteger, kArray, kDynamicMap };

    Kind kind;
    // kObject only: the allowed member table. An empty table is a strict
    // object: every field inside it is unknown and rejected.
    std::span<const Member> members{};
    bool required = false;
    // kString only: when non-empty, the value must equal this exact string.
    std::string_view expected_string{};
    // kInteger only: when set, the value must be at least this bound.
    std::optional<int64_t> min_value{};
    // kObject only: cross-member constraints. Both members of each pair must
    // hold equal integer values; a violation reports the object's own path.
    std::span<const MemberPair> equal_members{};
    // kArray only: the schema applied to every element.
    const Schema* element_schema = nullptr;
    // kDynamicMap only: the schema applied to every value; keys are free.
    const Schema* dynamic_value_schema = nullptr;
    // Named content grammar for kString values and kDynamicMap keys; empty
    // means unconstrained. Unknown grammar names fail closed.
    std::string_view grammar{};
    // kObject only: exactly one of the two members must be present; a
    // violation reports the object's own path.
    std::span<const MemberPair> exactly_one_pairs{};
    // kString only: inclusive byte-length cap (0 = unbounded); violations
    // map to kResourceLimit at the value path.
    std::size_t max_bytes = 0;
    // kDynamicMap only: inclusive member-count cap (0 = unbounded);
    // violations map to kResourceLimit at the map path.
    std::size_t max_members = 0;
};

// --- Leaf schemas ---

constexpr Schema kStringSchema{Schema::Kind::kString};
constexpr Schema kIntegerSchema{Schema::Kind::kInteger};
constexpr Schema kPositiveIntegerSchema{
    Schema::Kind::kInteger, {}, false, {}, 1};

// Host/App apiVersion values are the only exact-value contract so far.
constexpr Schema kHostApiVersionSchema{
    Schema::Kind::kString, {}, false, "capsid/host-v1"};
constexpr Schema kAppApiVersionSchema{
    Schema::Kind::kString, {}, false, "capsid/app-v1"};

// array<string>: the most common collection in both documents.
constexpr Schema kStringArraySchema{
    .kind = Schema::Kind::kArray,
    .element_schema = &kStringSchema,
};

// --- Host schemas ---

constexpr std::array kAdminMembers{
    Member{"unix", &kStringSchema, false},
    Member{"mode", &kStringSchema, false},
};
constexpr Schema kAdminSchema{
    Schema::Kind::kObject, std::span<const Member>(kAdminMembers)};

constexpr std::array kRoutingMembers{
    Member{"mode", &kStringSchema, false},
    Member{"suffix", &kStringSchema, false},
};
constexpr Schema kRoutingSchema{
    Schema::Kind::kObject, std::span<const Member>(kRoutingMembers)};

constexpr std::array kListenerLimitMembers{
    Member{"connections", &kIntegerSchema, false},
    Member{"headerBytes", &kStringSchema, false},
    Member{"headerTimeout", &kStringSchema, false},
    Member{"bodyIdleTimeout", &kStringSchema, false},
    Member{"streamIdleTimeout", &kStringSchema, false},
};
constexpr Schema kListenerLimitSchema{
    Schema::Kind::kObject, std::span<const Member>(kListenerLimitMembers)};

constexpr std::array kListenerMembers{
    Member{"name", &kStringSchema, false},
    Member{"tcp", &kStringSchema, false},
    Member{"publicScheme", &kStringSchema, false},
    Member{"publicAuthority", &kStringSchema, false},
    Member{"routing", &kRoutingSchema, false},
    Member{"limits", &kListenerLimitSchema, false},
};
constexpr Schema kListenerSchema{
    Schema::Kind::kObject, std::span<const Member>(kListenerMembers)};

constexpr Schema kListenerArraySchema{
    .kind = Schema::Kind::kArray,
    .element_schema = &kListenerSchema,
};

// Host allowlist entries use the Runtime's exact/one-trailing-'*' grammar.
constexpr Schema kEnvironmentPatternSchema{
    .kind = Schema::Kind::kString,
    .grammar = "environment-pattern",
};
constexpr Schema kEnvironmentPatternArraySchema{
    .kind = Schema::Kind::kArray,
    .element_schema = &kEnvironmentPatternSchema,
};

constexpr std::array kHostPermissionsMembers{
    Member{"modules", &kStringArraySchema, false},
    Member{"environmentNames", &kEnvironmentPatternArraySchema, false},
    Member{"fsReadRoots", &kStringArraySchema, false},
    Member{"fetchTargets", &kStringArraySchema, false},
    Member{"storageNamespaces", &kStringArraySchema, false},
    Member{"stdioStreams", &kStringArraySchema, false},
};
constexpr Schema kHostPermissionsSchema{
    Schema::Kind::kObject, std::span<const Member>(kHostPermissionsMembers)};

constexpr std::array kIsolationMembers{
    Member{"mode", &kStringSchema, false},
    Member{"required", &kStringArraySchema, false},
    Member{"cgroupRoot", &kStringSchema, false},
};
constexpr Schema kIsolationSchema{
    Schema::Kind::kObject, std::span<const Member>(kIsolationMembers)};

// Key names are free (release identifiers); values are public key paths.
constexpr Schema kTrustedBytecodeKeysSchema{
    .kind = Schema::Kind::kDynamicMap,
    .dynamic_value_schema = &kStringSchema,
};

// Worker and request shapes are shared verbatim between Host
// defaults/maximums and the App document.
constexpr std::array kWorkerMembers{
    Member{"jsHeap", &kStringSchema, false},
    Member{"processAddressSpace", &kStringSchema, false},
    Member{"memoryMax", &kStringSchema, false},
    Member{"fileDescriptors", &kPositiveIntegerSchema, false},
    Member{"pidsMax", &kIntegerSchema, false},
};
constexpr Schema kWorkerSchema{
    Schema::Kind::kObject, std::span<const Member>(kWorkerMembers)};

constexpr std::array kRequestMembers{
    Member{"timeout", &kStringSchema, false},
    Member{"maxInflightPerWorker", &kPositiveIntegerSchema, false},
    Member{"maxStreamingInflightPerWorker", &kIntegerSchema, false},
    Member{"streamIdleTimeoutMs", &kIntegerSchema, false},
};
constexpr Schema kRequestSchema{
    Schema::Kind::kObject, std::span<const Member>(kRequestMembers)};

// Host-side pool config carries only the queue fields; the App pool adds
// the required fixed-size members and equality below.
constexpr std::array kPoolQueueMembers{
    Member{"queueRequests", &kIntegerSchema, false},
    Member{"queueHeaderBytes", &kStringSchema, false},
    Member{"queueTimeout", &kStringSchema, false},
};
constexpr Schema kHostPoolSchema{
    Schema::Kind::kObject, std::span<const Member>(kPoolQueueMembers)};

constexpr std::array kTierMembers{
    Member{"worker", &kWorkerSchema, false},
    Member{"request", &kRequestSchema, false},
    Member{"pool", &kHostPoolSchema, false},
};
constexpr Schema kTierSchema{
    Schema::Kind::kObject, std::span<const Member>(kTierMembers)};

constexpr std::array kCapacityMembers{
    Member{"workersTotal", &kPositiveIntegerSchema, false},
    Member{"startupsConcurrent", &kIntegerSchema, false},
    Member{"queuedRequestsTotal", &kIntegerSchema, false},
    Member{"queuedHeaderBytesTotal", &kStringSchema, false},
    Member{"workerMemoryCommitTotal", &kStringSchema, false},
};
constexpr Schema kCapacitySchema{
    Schema::Kind::kObject, std::span<const Member>(kCapacityMembers)};

constexpr std::array kCrashBudgetMembers{
    Member{"maxEvents", &kIntegerSchema, false},
    Member{"window", &kStringSchema, false},
};
constexpr Schema kCrashBudgetSchema{
    Schema::Kind::kObject, std::span<const Member>(kCrashBudgetMembers)};

constexpr std::array kRestartBackoffMembers{
    Member{"initial", &kStringSchema, false},
    Member{"maximum", &kStringSchema, false},
    Member{"jitter", &kStringSchema, false},
};
constexpr Schema kRestartBackoffSchema{
    Schema::Kind::kObject, std::span<const Member>(kRestartBackoffMembers)};

constexpr std::array kRecoveryMembers{
    Member{"crashBudget", &kCrashBudgetSchema, false},
    Member{"restartBackoff", &kRestartBackoffSchema, false},
    Member{"replacementsConcurrentPerApp", &kIntegerSchema, false},
    Member{"activeHealthInterval", &kStringSchema, false},
    Member{"activeHealthFailures", &kIntegerSchema, false},
};
constexpr Schema kRecoverySchema{
    Schema::Kind::kObject, std::span<const Member>(kRecoveryMembers)};

constexpr std::array kHostMembers{
    Member{"apiVersion", &kHostApiVersionSchema, true},
    Member{"applicationsRoot", &kStringSchema, false},
    Member{"stateRoot", &kStringSchema, false},
    Member{"secretRootTemplate", &kStringSchema, false},
    Member{"admin", &kAdminSchema, false},
    Member{"listeners", &kListenerArraySchema, false},
    Member{"permissions", &kHostPermissionsSchema, false},
    Member{"isolation", &kIsolationSchema, false},
    Member{"trustedBytecodeKeys", &kTrustedBytecodeKeysSchema, false},
    Member{"defaults", &kTierSchema, false},
    Member{"maximums", &kTierSchema, false},
    Member{"capacity", &kCapacitySchema, false},
    Member{"recovery", &kRecoverySchema, false},
};
constexpr Schema kHostSchema{
    Schema::Kind::kObject, std::span<const Member>(kHostMembers)};

// --- Environment request schemas ---
//
// Grammar rules mirror capsid::capability_policy::valid_env_pattern /
// valid_env_name (src/capability_policy.cc), the Runtime authority.
// Environment values and secret keys are additionally bounded here so the
// App document cannot request what the Runtime snapshot compiler rejects.

constexpr Schema kEnvironmentValueSchema{
    .kind = Schema::Kind::kString,
    .grammar = "environment-value",
    .max_bytes = kMaxEnvironmentValueBytes,
};
constexpr Schema kSecretKeyIdSchema{
    .kind = Schema::Kind::kString,
    .grammar = "secret-key",
};

// env entries: exactly one of value/valueFrom must be present.
constexpr std::array kEnvEntryMembers{
    Member{"value", &kEnvironmentValueSchema, false},
    Member{"valueFrom", &kSecretKeyIdSchema, false},
};
constexpr std::array kEnvEntryExclusivePairs{
    MemberPair{"value", "valueFrom"},
};
constexpr Schema kEnvEntrySchema{
    .kind = Schema::Kind::kObject,
    .members = kEnvEntryMembers,
    .exactly_one_pairs = kEnvEntryExclusivePairs,
};

constexpr Schema kEnvMapSchema{
    .kind = Schema::Kind::kDynamicMap,
    .dynamic_value_schema = &kEnvEntrySchema,
    .grammar = "environment-name",
    .max_members = kMaxEnvironmentEntries,
};

constexpr std::array kFsReadMembers{
    Member{"allow", &kStringArraySchema, false},
    Member{"deny", &kStringArraySchema, false},
};
constexpr Schema kFsReadSchema{
    Schema::Kind::kObject, std::span<const Member>(kFsReadMembers)};

constexpr std::array kFsMembers{
    Member{"read", &kFsReadSchema, false},
};
constexpr Schema kFsSchema{
    Schema::Kind::kObject, std::span<const Member>(kFsMembers)};

constexpr std::array kFetchMembers{
    Member{"allow", &kStringArraySchema, false},
};
constexpr Schema kFetchSchema{
    Schema::Kind::kObject, std::span<const Member>(kFetchMembers)};

constexpr std::array kStorageMembers{
    Member{"namespaces", &kStringArraySchema, false},
};
constexpr Schema kStorageSchema{
    Schema::Kind::kObject, std::span<const Member>(kStorageMembers)};

constexpr std::array kAppPermissionsMembers{
    Member{"modules", &kStringArraySchema, false},
    Member{"env", &kEnvMapSchema, false},
    Member{"fs", &kFsSchema, false},
    Member{"fetch", &kFetchSchema, false},
    Member{"storage", &kStorageSchema, false},
    Member{"stdio", &kStringArraySchema, false},
};
constexpr Schema kAppPermissionsSchema{
    Schema::Kind::kObject, std::span<const Member>(kAppPermissionsMembers)};

// App pool: required fixed size plus the shared queue fields.
constexpr std::array kAppPoolMembers{
    Member{"minReady", &kPositiveIntegerSchema, true},
    Member{"maxWorkers", &kPositiveIntegerSchema, true},
    Member{"queueRequests", &kIntegerSchema, false},
    Member{"queueHeaderBytes", &kStringSchema, false},
    Member{"queueTimeout", &kStringSchema, false},
};
constexpr std::array kAppPoolEqualityPairs{
    MemberPair{"minReady", "maxWorkers"},
};
constexpr Schema kAppPoolSchema{
    Schema::Kind::kObject,
    std::span<const Member>(kAppPoolMembers),
    false,
    {},
    {},
    std::span<const MemberPair>(kAppPoolEqualityPairs),
};

constexpr std::array kHealthCheckMembers{
    Member{"path", &kStringSchema, false},
    Member{"timeout", &kStringSchema, false},
};
constexpr Schema kHealthCheckSchema{
    Schema::Kind::kObject, std::span<const Member>(kHealthCheckMembers)};

constexpr std::array kApplicationMembers{
    Member{"apiVersion", &kAppApiVersionSchema, true},
    Member{"entry", &kStringSchema, false},
    Member{"permissions", &kAppPermissionsSchema, false},
    Member{"worker", &kWorkerSchema, false},
    Member{"request", &kRequestSchema, false},
    Member{"pool", &kAppPoolSchema, true},
    Member{"healthCheck", &kHealthCheckSchema, false},
};
constexpr Schema kApplicationSchema{
    Schema::Kind::kObject, std::span<const Member>(kApplicationMembers)};

const Schema& root_schema(ConfigDocument document) {
    return document == ConfigDocument::kHost ? kHostSchema : kApplicationSchema;
}

const Member* find_member(const Schema& schema, std::string_view key) {
    for (const Member& member : schema.members) {
        if (member.name == key) {
            return &member;
        }
    }
    return nullptr;
}

// RFC 6901 JSON Pointer escaping: ~ -> ~0 and / -> ~1. Keys are
// NUL-terminated, UTF-8 validated strings produced by the parser.
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

// Environment name/pattern grammars mirror
// capsid::capability_policy::valid_env_pattern / valid_env_name
// (src/capability_policy.cc) — the Runtime is the authority for what a Host
// allowlist entry may look like. Any divergence here would let the Host
// accept a pattern the Runtime cannot honor.
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

// Environment values may be any UTF-8 text without embedded NUL bytes.
bool valid_environment_value(std::string_view bytes) {
    return bytes.find('\0') == std::string_view::npos;
}

// Dispatches the named grammar; an unknown grammar name fails closed.
bool matches_grammar(std::string_view grammar, std::string_view value) {
    if (grammar == "environment-name") {
        return valid_environment_name(value);
    }
    if (grammar == "environment-pattern") {
        return valid_environment_pattern(value);
    }
    if (grammar == "secret-key") {
        return valid_secret_key_id(value);
    }
    if (grammar == "environment-value") {
        return valid_environment_value(value);
    }
    return false;
}

// Whether phase 1 should descend into `node` as a child of `schema`.
bool can_descend(const Schema& schema, json_t* node) {
    return (schema.kind == Schema::Kind::kObject && json_is_object(node)) ||
           (schema.kind == Schema::Kind::kArray && json_is_array(node)) ||
           (schema.kind == Schema::Kind::kDynamicMap && json_is_object(node));
}

// Phase 1: reject the first unknown field in pre-order document order.
// Recursion descends into arrays by index and dynamic maps by escaped key;
// values with deferred type errors are skipped here and handled in phase 2.
// Recursion depth is bounded by the schema depth, never by the input.
bool check_unknown_fields(const Schema& schema,
                          json_t* node,
                          const std::string& path,
                          ConfigError& error) {
    switch (schema.kind) {
    case Schema::Kind::kObject:
        if (!json_is_object(node)) {
            return true;
        }
        for (void* iter = json_object_iter(node); iter != nullptr;
             iter = json_object_iter_next(node, iter)) {
            // Explicit-length key handling is defensive. The upstream parser
            // currently rejects NUL object keys even with JSON_ALLOW_NUL; keep
            // the check below so a future parser-policy change cannot cause
            // silent C-string truncation.
            const std::string_view key(json_object_iter_key(iter),
                                       json_object_iter_key_len(iter));
            if (key.find('\0') != std::string_view::npos) {
                error.code = ConfigErrorCode::kInvalidValue;
                error.path = path;
                error.message = "object key contains a NUL byte";
                return false;
            }
            const Member* member = find_member(schema, key);
            if (member == nullptr) {
                error.code = ConfigErrorCode::kUnknownField;
                error.path = path + "/" + escape_pointer_component(key);
                error.message = "unknown configuration field";
                return false;
            }
            if (can_descend(*member->schema, json_object_iter_value(iter))) {
                const std::string child_path =
                    path + "/" + escape_pointer_component(member->name);
                if (!check_unknown_fields(*member->schema,
                                          json_object_iter_value(iter),
                                          child_path, error)) {
                    return false;
                }
            }
        }
        return true;
    case Schema::Kind::kArray:
        if (!json_is_array(node)) {
            return true;
        }
        for (size_t i = 0; i < json_array_size(node); ++i) {
            const std::string child_path = path + "/" + std::to_string(i);
            if (!check_unknown_fields(*schema.element_schema,
                                      json_array_get(node, i), child_path,
                                      error)) {
                return false;
            }
        }
        return true;
    case Schema::Kind::kDynamicMap:
        if (!json_is_object(node)) {
            return true;
        }
        for (void* iter = json_object_iter(node); iter != nullptr;
             iter = json_object_iter_next(node, iter)) {
            const std::string_view key(json_object_iter_key(iter),
                                       json_object_iter_key_len(iter));
            if (key.find('\0') != std::string_view::npos) {
                error.code = ConfigErrorCode::kInvalidValue;
                error.path = path;
                error.message = "object key contains a NUL byte";
                return false;
            }
            const std::string child_path =
                path + "/" + escape_pointer_component(key);
            if (!check_unknown_fields(*schema.dynamic_value_schema,
                                      json_object_iter_value(iter), child_path,
                                      error)) {
                return false;
            }
        }
        return true;
    case Schema::Kind::kString:
    case Schema::Kind::kInteger:
        return true;
    }
    return true;
}

// Phase 2: types, required members, exact values, integer bounds and
// cross-member equality. The unknown-field sweep guarantees every fixed
// object key here is declared, so member lookup cannot fail; the
// fail-closed branch below is defensive only.
bool validate_values(const Schema& schema,
                     json_t* node,
                     const std::string& path,
                     ConfigError& error) {
    switch (schema.kind) {
    case Schema::Kind::kObject: {
        if (!json_is_object(node)) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "expected a JSON object";
            return false;
        }
        for (void* iter = json_object_iter(node); iter != nullptr;
             iter = json_object_iter_next(node, iter)) {
            const std::string_view key(json_object_iter_key(iter),
                                       json_object_iter_key_len(iter));
            if (key.find('\0') != std::string_view::npos) {
                error.code = ConfigErrorCode::kInvalidValue;
                error.path = path;
                error.message = "object key contains a NUL byte";
                return false;
            }
            const Member* member = find_member(schema, key);
            if (member == nullptr) {
                error.code = ConfigErrorCode::kUnknownField;
                error.path = path + "/" + escape_pointer_component(key);
                error.message = "unknown configuration field";
                return false;
            }
            const std::string child_path =
                path + "/" + escape_pointer_component(member->name);
            if (!validate_values(*member->schema, json_object_iter_value(iter),
                                 child_path, error)) {
                return false;
            }
        }
        for (const Member& member : schema.members) {
            if (member.required &&
                !json_object_get(node, std::string(member.name).c_str())) {
                error.code = ConfigErrorCode::kInvalidValue;
                error.path = path + "/" + escape_pointer_component(member.name);
                error.message = "missing required field";
                return false;
            }
        }
        for (const MemberPair& pair : schema.equal_members) {
            const json_t* first_value =
                json_object_get(node, std::string(pair.first).c_str());
            const json_t* second_value =
                json_object_get(node, std::string(pair.second).c_str());
            if (first_value != nullptr && second_value != nullptr &&
                json_integer_value(first_value) !=
                    json_integer_value(second_value)) {
                error.code = ConfigErrorCode::kInvalidValue;
                error.path = path;
                error.message = "cross-field values must be equal";
                return false;
            }
        }
        for (const MemberPair& pair : schema.exactly_one_pairs) {
            const bool first_present =
                json_object_get(node, std::string(pair.first).c_str()) !=
                nullptr;
            const bool second_present =
                json_object_get(node, std::string(pair.second).c_str()) !=
                nullptr;
            if (first_present == second_present) {
                error.code = ConfigErrorCode::kInvalidValue;
                error.path = path;
                error.message = "exactly one of the two fields is required";
                return false;
            }
        }
        return true;
    }
    case Schema::Kind::kArray:
        if (!json_is_array(node)) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "expected a JSON array";
            return false;
        }
        if (schema.element_schema == nullptr) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "internal schema error";
            return false;
        }
        for (size_t i = 0; i < json_array_size(node); ++i) {
            const std::string child_path = path + "/" + std::to_string(i);
            if (!validate_values(*schema.element_schema,
                                 json_array_get(node, i), child_path, error)) {
                return false;
            }
        }
        return true;
    case Schema::Kind::kDynamicMap:
        if (!json_is_object(node)) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "expected a JSON object";
            return false;
        }
        if (schema.dynamic_value_schema == nullptr) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "internal schema error";
            return false;
        }
        if (schema.max_members != 0 &&
            json_object_size(node) > schema.max_members) {
            error.code = ConfigErrorCode::kResourceLimit;
            error.path = path;
            error.message = "too many entries";
            return false;
        }
        for (void* iter = json_object_iter(node); iter != nullptr;
             iter = json_object_iter_next(node, iter)) {
            const std::string_view key(json_object_iter_key(iter),
                                       json_object_iter_key_len(iter));
            if (key.find('\0') != std::string_view::npos) {
                error.code = ConfigErrorCode::kInvalidValue;
                error.path = path;
                error.message = "object key contains a NUL byte";
                return false;
            }
            if (!schema.grammar.empty() &&
                !matches_grammar(schema.grammar, key)) {
                error.code = ConfigErrorCode::kInvalidValue;
                error.path = path + "/" + escape_pointer_component(key);
                error.message = "invalid member name";
                return false;
            }
            const std::string child_path =
                path + "/" + escape_pointer_component(key);
            if (!validate_values(*schema.dynamic_value_schema,
                                 json_object_iter_value(iter), child_path,
                                 error)) {
                return false;
            }
        }
        return true;
    case Schema::Kind::kString: {
        if (!json_is_string(node)) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "expected a JSON string";
            return false;
        }
        // Byte-aware view: json_string_length() preserves embedded NULs that
        // a C-string comparison would silently truncate. Every string value
        // rejects NUL bytes first with the field path; NUL object keys never
        // reach this code because the upstream parser rejects them.
        const std::string_view value =
            std::string_view(json_string_value(node), json_string_length(node));
        if (value.find('\0') != std::string_view::npos) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "value contains a NUL byte";
            return false;
        }
        if (!schema.expected_string.empty() && value != schema.expected_string) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "unsupported value";
            return false;
        }
        if (!schema.grammar.empty() && !matches_grammar(schema.grammar, value)) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "unsupported value";
            return false;
        }
        if (schema.max_bytes != 0 && value.size() > schema.max_bytes) {
            error.code = ConfigErrorCode::kResourceLimit;
            error.path = path;
            error.message = "value exceeds the size limit";
            return false;
        }
        return true;
    }
    case Schema::Kind::kInteger:
        if (!json_is_integer(node)) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "expected a JSON integer";
            return false;
        }
        if (schema.min_value.has_value() &&
            json_integer_value(node) < *schema.min_value) {
            error.code = ConfigErrorCode::kInvalidValue;
            error.path = path;
            error.message = "value below minimum";
            return false;
        }
        return true;
    }
    error.code = ConfigErrorCode::kInvalidValue;
    error.path = path;
    error.message = "internal schema error";
    return false;
}

}  // namespace

ConfigValidationResult validate_config_json(ConfigDocument document,
                                            std::string_view json) {
    ConfigValidationResult result;

    // The byte limit is inclusive and applies before any parsing.
    if (json.size() > kMaxConfigBytes) {
        result.ok = false;
        result.error.code = ConfigErrorCode::kResourceLimit;
        result.error.message = "document exceeds the size limit";
        return result;
    }

    // JSON_ALLOW_NUL lets escaped \u0000 reach the value validators (which
    // reject it with a precise field path) instead of surfacing as a bare
    // parse error. NUL object keys are still rejected by the upstream
    // parser (json_error_null_byte_in_key -> kInvalidJson), and raw NUL
    // bytes in the input remain rejected by the lexer.
    json_error_t parse_error;
    json_t* root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES | JSON_ALLOW_NUL,
                              &parse_error);
    if (root == nullptr) {
        result.ok = false;
        const enum json_error_code parse_code = json_error_code(&parse_error);
        ConfigErrorCode code = ConfigErrorCode::kInvalidJson;
        const char* reason = "invalid JSON";
        if (parse_code == json_error_duplicate_key) {
            code = ConfigErrorCode::kDuplicateKey;
            reason = "duplicate object key";
        } else if (parse_code == json_error_stack_overflow) {
            code = ConfigErrorCode::kResourceLimit;
            reason = "document exceeds the nesting limit";
        }
        result.error.code = code;
        // A parse failure has no JSON value to point at; the pointer is the
        // document root. The message carries only positions, never input.
        result.error.message =
            std::string(reason) + " at line " + std::to_string(parse_error.line) +
            " column " + std::to_string(parse_error.column);
        return result;
    }

    const std::string root_path;
    const Schema& schema = root_schema(document);
    if (!check_unknown_fields(schema, root, root_path, result.error)) {
        result.ok = false;
        json_decref(root);
        return result;
    }
    result.ok = validate_values(schema, root, root_path, result.error);
    json_decref(root);
    return result;
}

}  // namespace capsid::host
