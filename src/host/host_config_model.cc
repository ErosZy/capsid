// Typed host.json model implementation — see host_config_model.h.
//
// This parser runs AFTER validate_config_json; every field the schema
// accepts must be mapped here or the document is rejected. The schema's
// unconstrained strings (size/duration/jitter texts) are validated with the
// frozen grammars and fail closed. The three coordinator semantic gates stay
// here: admin.mode == 0600, isolation.mode == "strict", capacity.workersTotal
// positive and within the Host limit.

#include "host/host_config_model.h"

#include "host/config.h"
#include "host/listener_cors.h"
#include "host/request_normalization.h"

#include <jansson.h>

#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

namespace capsid::host {

namespace {

// ---- jansson helpers ------------------------------------------------------

std::string json_string_field(json_t* object, const char* key) {
    json_t* value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : std::string();
}

bool parse_string_array(json_t* parent, const char* key,
                        std::vector<std::string>* out) {
    json_t* value = json_object_get(parent, key);
    if (value == nullptr) {
        return true;
    }
    if (!json_is_array(value)) {
        return false;
    }
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(value, index, item) {
        if (!json_is_string(item)) {
            return false;
        }
        out->push_back(json_string_value(item));
    }
    return true;
}

// ---- frozen text grammars (error-returning; no fail()/exit) ---------------

// "host:port" / "host" (any port); decimal ports only.
bool parse_fetch_target_text(const std::string& text, FetchTarget* out) {
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos) {
        if (text.empty()) {
            return false;
        }
        out->host = text;
        return true;
    }
    out->host = text.substr(0, colon);
    if (out->host.empty()) {
        return false;
    }
    const std::string ports = text.substr(colon + 1);
    std::size_t begin = 0;
    while (begin <= ports.size()) {
        const std::size_t comma = ports.find(',', begin);
        const std::string part = ports.substr(
            begin, comma == std::string::npos ? std::string::npos
                                              : comma - begin);
        if (part.empty()) {
            return false;
        }
        char* end = nullptr;
        const long port = std::strtol(part.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || port <= 0 || port > 65535) {
            return false;
        }
        out->ports.push_back(static_cast<std::uint16_t>(port));
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return !out->ports.empty();
}

// "256MiB" style size with explicit suffix (the frozen size grammar).
bool parse_size_bytes_text(const std::string& text, std::uint64_t* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long base = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str()) {
        return false;
    }
    std::uint64_t multiplier = 0;
    const std::string suffix(end);
    if (suffix == "KiB") {
        multiplier = 1024ULL;
    } else if (suffix == "MiB") {
        multiplier = 1024ULL * 1024ULL;
    } else if (suffix == "GiB") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (suffix == "KB") {
        multiplier = 1000ULL;
    } else if (suffix == "MB") {
        multiplier = 1000ULL * 1000ULL;
    } else if (suffix == "GB") {
        multiplier = 1000ULL * 1000ULL * 1000ULL;
    } else {
        return false;
    }
    if (base > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return false;
    }
    *out = static_cast<std::uint64_t>(base) * multiplier;
    return true;
}

// "250ms" / "5s" / "1m" duration (the frozen duration grammar); unit is
// required and the value must be a positive integer.
bool parse_duration_ms_text(const std::string& text, std::uint64_t* out) {
    std::string::size_type number_end = 0;
    for (std::string::size_type index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            break;
        }
        number_end = index + 1;
    }
    if (number_end == 0 || number_end == text.size()) {
        return false;
    }
    std::uint64_t multiplier = 0;
    const std::string unit = text.substr(number_end);
    if (unit == "ms") {
        multiplier = 1;
    } else if (unit == "s") {
        multiplier = 1000;
    } else if (unit == "m") {
        multiplier = 60U * 1000U;
    } else {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    // The strtoull end-pointer must not outlive the string it points into;
    // a temporary from substr() would be destroyed at the end of the full
    // expression, leaving *end a stack-use-after-scope read (ASAN caught
    // this). Keep the substring alive for the duration of the check.
    const std::string numeric_text = text.substr(0, number_end);
    const unsigned long long parsed =
        std::strtoull(numeric_text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(
                      std::numeric_limits<std::uint64_t>::max() /
                      multiplier)) {
        return false;
    }
    *out = static_cast<std::uint64_t>(parsed) * multiplier;
    return true;
}

// "jitter": a percent text like "20%" (the design's contract, §5.1) or a
// plain decimal basis-points text like "1000" (the pool's contract); both
// resolve to basis points in 0..10000 ("20%" = 2000). A bare percent
// (no digits) or a value over the range is rejected.
bool parse_jitter_basis_points_text(const std::string& text,
                                    std::uint64_t* out) {
    std::string::size_type digits_end = 0;
    for (std::string::size_type index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            break;
        }
        digits_end = index + 1;
    }
    if (digits_end == 0) {
        return false;
    }
    const std::string suffix = text.substr(digits_end);
    if (suffix != "" && suffix != "%") {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const std::string numeric_text = text.substr(0, digits_end);
    const unsigned long long parsed =
        std::strtoull(numeric_text.c_str(), &end, 10);
    const unsigned long long limit = suffix == "%" ? 100ULL : 10000ULL;
    const unsigned long long multiplier = suffix == "%" ? 100ULL : 1ULL;
    if (errno != 0 || end == nullptr || *end != '\0' || parsed > limit ||
        parsed > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return false;
    }
    *out = static_cast<std::uint64_t>(parsed) * multiplier;
    return true;
}

// ---- typed field readers --------------------------------------------------

// Non-negative integer member (the schema allows negative integers for
// ceiling fields; the parse must reject them).
bool parse_uint_field(json_t* object, const char* key, std::uint64_t* out) {
    json_t* value = json_object_get(object, key);
    if (value == nullptr) {
        return true;
    }
    if (!json_is_integer(value)) {
        return false;
    }
    const json_int_t parsed = json_integer_value(value);
    if (parsed < 0) {
        return false;
    }
    *out = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parse_size_text_field(json_t* object, const char* key,
                           std::uint64_t* out) {
    json_t* value = json_object_get(object, key);
    if (value == nullptr) {
        return true;
    }
    if (!json_is_string(value)) {
        return false;
    }
    return parse_size_bytes_text(json_string_value(value), out);
}

bool parse_duration_text_field(json_t* object, const char* key,
                               std::uint64_t* out) {
    json_t* value = json_object_get(object, key);
    if (value == nullptr) {
        return true;
    }
    if (!json_is_string(value)) {
        return false;
    }
    return parse_duration_ms_text(json_string_value(value), out);
}

// ---- section parsers ------------------------------------------------------

bool parse_listeners(json_t* root, std::vector<ListenerConfig>* out) {
    json_t* listeners = json_object_get(root, "listeners");
    if (listeners == nullptr) {
        return true;
    }
    if (!json_is_array(listeners)) {
        return false;
    }
    std::size_t index = 0;
    json_t* listener = nullptr;
    json_array_foreach(listeners, index, listener) {
        if (!json_is_object(listener)) {
            return false;
        }
        ListenerConfig config;
        config.name = json_string_field(listener, "name");
        config.tcp = json_string_field(listener, "tcp");
        config.public_scheme = json_string_field(listener, "publicScheme");
        config.public_authority = json_string_field(listener, "publicAuthority");
        const json_t* trusted = json_object_get(listener, "trusted");
        config.trusted = json_is_true(trusted);  // absent == false
        json_t* routing = json_object_get(listener, "routing");
        if (json_is_object(routing)) {
            config.routing.mode = json_string_field(routing, "mode");
            config.routing.suffix = json_string_field(routing, "suffix");
        }
        json_t* limits = json_object_get(listener, "limits");
        if (json_is_object(limits)) {
            if (!parse_uint_field(limits, "connections",
                                  &config.limits.connections) ||
                !parse_size_text_field(limits, "headerBytes",
                                       &config.limits.header_bytes) ||
                !parse_duration_text_field(limits, "headerTimeout",
                                           &config.limits.header_timeout_ms) ||
                !parse_duration_text_field(
                    limits, "bodyIdleTimeout",
                    &config.limits.body_idle_timeout_ms) ||
                !parse_duration_text_field(
                    limits, "streamIdleTimeout",
                    &config.limits.stream_idle_timeout_ms)) {
                return false;
            }
        }
        json_t* cors = json_object_get(listener, "cors");
        if (json_is_object(cors)) {
            config.cors.configured = true;
            if (!parse_string_array(cors, "allowedOrigins",
                                    &config.cors.allowed_origins) ||
                !parse_string_array(cors, "allowedMethods",
                                    &config.cors.allowed_methods) ||
                !parse_string_array(cors, "allowedHeaders",
                                    &config.cors.allowed_headers) ||
                !parse_duration_text_field(cors, "maxAge",
                                           &config.cors.max_age_ms)) {
                return false;
            }
            if (config.cors.allowed_origins.empty() ||
                config.cors.allowed_methods.empty()) {
                return false;
            }
            if (config.cors.allowed_origins.size() > 1 &&
                std::find(config.cors.allowed_origins.begin(),
                          config.cors.allowed_origins.end(),
                          "*") != config.cors.allowed_origins.end()) {
                return false;
            }
            for (const std::string& origin : config.cors.allowed_origins) {
                if (!valid_cors_origin(origin)) {
                    return false;
                }
            }
            for (std::string& method : config.cors.allowed_methods) {
                if (!valid_cors_method_token(method)) {
                    return false;
                }
                for (char& c : method) {
                    if (c >= 'a' && c <= 'z') {
                        c = static_cast<char>(c - 'a' + 'A');
                    }
                }
            }
            for (std::string& header : config.cors.allowed_headers) {
                if (!valid_cors_header_token(header)) {
                    return false;
                }
                for (char& c : header) {
                    if (c >= 'A' && c <= 'Z') {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                }
            }
        }
        out->push_back(std::move(config));
    }
    return true;
}

bool parse_permissions(json_t* root, HostPolicy* policy) {
    json_t* permissions = json_object_get(root, "permissions");
    if (permissions == nullptr) {
        return true;
    }
    if (!json_is_object(permissions)) {
        return false;
    }
    if (!parse_string_array(permissions, "modules", &policy->module_allowlist) ||
        !parse_string_array(permissions, "environmentNames",
                            &policy->env_patterns) ||
        !parse_string_array(permissions, "fsReadRoots",
                            &policy->fs_read_roots) ||
        !parse_string_array(permissions, "storageNamespaces",
                            &policy->storage_namespaces) ||
        !parse_string_array(permissions, "stdioStreams",
                            &policy->stdio_streams)) {
        return false;
    }
    policy->storage_allowed = !policy->storage_namespaces.empty();
    policy->stdio_allowed = !policy->stdio_streams.empty();
    std::vector<std::string> targets;
    if (!parse_string_array(permissions, "fetchTargets", &targets)) {
        return false;
    }
    for (const std::string& target : targets) {
        FetchTarget parsed;
        if (!parse_fetch_target_text(target, &parsed)) {
            return false;
        }
        policy->fetch_targets.push_back(std::move(parsed));
    }
    return true;
}

bool parse_isolation(json_t* root, std::string* mode,
                     std::vector<std::string>* required,
                     std::string* cgroup_root) {
    json_t* isolation = json_object_get(root, "isolation");
    if (isolation == nullptr) {
        return true;
    }
    if (!json_is_object(isolation)) {
        return false;
    }
    *mode = json_string_field(isolation, "mode");
    if (!mode->empty() && *mode != "strict") {
        return false;  // semantic gate, reported by the caller
    }
    if (!parse_string_array(isolation, "required", required)) {
        return false;
    }
    *cgroup_root = json_string_field(isolation, "cgroupRoot");
    return true;
}

bool parse_trusted_bytecode_keys(
    json_t* root, std::vector<TrustedKeyDescriptor>* out) {
    json_t* keys = json_object_get(root, "trustedBytecodeKeys");
    if (keys == nullptr) {
        return true;
    }
    if (!json_is_object(keys)) {
        return false;
    }
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(keys, key, value) {
        if (!json_is_string(value)) {
            return false;
        }
        TrustedKeyDescriptor descriptor;
        descriptor.key_id = key;
        descriptor.key_path = json_string_value(value);
        out->push_back(std::move(descriptor));
    }
    return true;
}

bool parse_worker_tier(json_t* tier, WorkerTierConfig* out) {
    json_t* worker = json_object_get(tier, "worker");
    if (worker == nullptr) {
        return true;
    }
    if (!json_is_object(worker)) {
        return false;
    }
    // The size texts are grammar-validated here and carried verbatim; the
    // consuming PR converts them to bytes at use time.
    json_t* js_heap = json_object_get(worker, "jsHeap");
    if (json_is_string(js_heap)) {
        std::uint64_t ignored = 0;
        if (!parse_size_bytes_text(json_string_value(js_heap), &ignored)) {
            return false;
        }
        out->js_heap = json_string_value(js_heap);
    }
    json_t* address = json_object_get(worker, "processAddressSpace");
    if (json_is_string(address)) {
        std::uint64_t ignored = 0;
        if (!parse_size_bytes_text(json_string_value(address), &ignored)) {
            return false;
        }
        out->process_address_space = json_string_value(address);
    }
    json_t* memory = json_object_get(worker, "memoryMax");
    if (json_is_string(memory)) {
        std::uint64_t ignored = 0;
        if (!parse_size_bytes_text(json_string_value(memory), &ignored)) {
            return false;
        }
        out->memory_max = json_string_value(memory);
    }
    return parse_uint_field(worker, "fileDescriptors",
                            &out->file_descriptors) &&
           parse_uint_field(worker, "pidsMax", &out->pids_max);
}

bool parse_request_tier(json_t* tier, RequestTierConfig* out) {
    json_t* request = json_object_get(tier, "request");
    if (request == nullptr) {
        return true;
    }
    if (!json_is_object(request)) {
        return false;
    }
    json_t* timeout = json_object_get(request, "timeout");
    if (json_is_string(timeout)) {
        std::uint64_t ignored = 0;
        if (!parse_duration_ms_text(json_string_value(timeout), &ignored)) {
            return false;
        }
        out->timeout = json_string_value(timeout);
    }
    return parse_uint_field(request, "maxInflightPerWorker",
                            &out->max_inflight_per_worker) &&
           parse_uint_field(request, "maxStreamingInflightPerWorker",
                            &out->max_streaming_inflight_per_worker) &&
           parse_uint_field(request, "streamIdleTimeoutMs",
                            &out->stream_idle_timeout_ms) &&
           parse_uint_field(request, "writeTimeoutMs",
                            &out->write_timeout_ms);
}

bool parse_pool_tier(json_t* tier, PoolTierConfig* out) {
    json_t* pool = json_object_get(tier, "pool");
    if (pool == nullptr) {
        return true;
    }
    if (!json_is_object(pool)) {
        return false;
    }
    return parse_uint_field(pool, "queueRequests", &out->queue_requests) &&
           parse_size_text_field(pool, "queueHeaderBytes",
                                 &out->queue_header_bytes) &&
           parse_duration_text_field(pool, "queueTimeout",
                                     &out->queue_timeout_ms);
}

bool parse_tier(json_t* tier, TierConfig* out) {
    if (!json_is_object(tier)) {
        return false;
    }
    return parse_worker_tier(tier, &out->worker) &&
           parse_request_tier(tier, &out->request) &&
           parse_pool_tier(tier, &out->pool);
}

bool parse_capacity(json_t* root, CapacityConfig* out) {
    json_t* capacity = json_object_get(root, "capacity");
    if (capacity == nullptr) {
        return true;
    }
    if (!json_is_object(capacity)) {
        return false;
    }
    // workersTotal is schema-positive; the Host limit gate mirrors the
    // coordinator's int budget.
    json_t* workers = json_object_get(capacity, "workersTotal");
    if (json_is_integer(workers)) {
        const json_int_t value = json_integer_value(workers);
        if (value <= 0 ||
            value > static_cast<json_int_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        out->workers_total = static_cast<std::uint64_t>(value);
    }
    // §9.4: activationSurgeWorkers is non-negative and defaults to 0
    // (absent = no zero-downtime replaces, see the schema member).
    return parse_uint_field(capacity, "activationSurgeWorkers",
                            &out->activation_surge_workers) &&
           parse_uint_field(capacity, "startupsConcurrent",
                            &out->startups_concurrent) &&
           parse_uint_field(capacity, "queuedRequestsTotal",
                            &out->queued_requests_total) &&
           parse_size_text_field(capacity, "queuedHeaderBytesTotal",
                                 &out->queued_header_bytes_total) &&
           parse_size_text_field(capacity, "workerMemoryCommitTotal",
                                 &out->worker_memory_commit_total);
}

bool parse_recovery(json_t* root, RecoveryConfig* out) {
    json_t* recovery = json_object_get(root, "recovery");
    if (recovery == nullptr) {
        return true;
    }
    if (!json_is_object(recovery)) {
        return false;
    }
    json_t* crash_budget = json_object_get(recovery, "crashBudget");
    if (crash_budget != nullptr) {
        if (!json_is_object(crash_budget) ||
            !parse_uint_field(crash_budget, "maxEvents",
                              &out->crash_budget.max_events) ||
            !parse_duration_text_field(crash_budget, "window",
                                       &out->crash_budget.window_ms)) {
            return false;
        }
    }
    json_t* restart_backoff = json_object_get(recovery, "restartBackoff");
    if (restart_backoff != nullptr) {
        if (!json_is_object(restart_backoff) ||
            !parse_duration_text_field(restart_backoff, "initial",
                                       &out->restart_backoff.initial_ms) ||
            !parse_duration_text_field(restart_backoff, "maximum",
                                       &out->restart_backoff.maximum_ms)) {
            return false;
        }
        json_t* jitter = json_object_get(restart_backoff, "jitter");
        if (json_is_string(jitter)) {
            std::uint64_t ignored = 0;
            if (!parse_jitter_basis_points_text(json_string_value(jitter),
                                                &ignored)) {
                return false;
            }
            out->restart_backoff.jitter = json_string_value(jitter);
        }
    }
    return parse_uint_field(recovery, "replacementsConcurrentPerApp",
                            &out->replacements_concurrent_per_app) &&
           parse_duration_text_field(recovery, "activeHealthInterval",
                                     &out->active_health_interval_ms) &&
           parse_uint_field(recovery, "activeHealthFailures",
                            &out->active_health_failures);
}

}  // namespace

bool parse_host_config(std::string_view json, ParsedHostConfig* out,
                       std::string* error) {
    json_error_t parse_error;
    json_t* root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root)) {
        if (error != nullptr) {
            *error = "invalid host.json";
        }
        if (root != nullptr) {
            json_decref(root);
        }
        return false;
    }

    auto reject = [&](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        json_decref(root);
        return false;
    };

    ParsedHostConfig config;
    config.applications_root = json_string_field(root, "applicationsRoot");
    config.state_root = json_string_field(root, "stateRoot");
    config.secret_root_template = json_string_field(root, "secretRootTemplate");
    config.bindings_root = json_string_field(root, "bindingsRoot");
    json_t* admin = json_object_get(root, "admin");
    if (json_is_object(admin)) {
        config.admin_unix_path = json_string_field(admin, "unix");
        const std::string mode_text = json_string_field(admin, "mode");
        if (!mode_text.empty()) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(mode_text.c_str(), &end, 8);
            if (end == nullptr || *end != '\0' || parsed > 07777 ||
                static_cast<mode_t>(parsed) != 0600) {
                return reject("admin.mode must be 0600");
            }
            config.admin_mode = static_cast<mode_t>(parsed);
        }
    }

    if (!parse_listeners(root, &config.listeners)) {
        return reject("invalid host.json listeners");
    }
    if (!parse_permissions(root, &config.policy)) {
        return reject("invalid host.json permissions");
    }
    if (!parse_isolation(root, &config.isolation_mode,
                         &config.isolation_required,
                         &config.isolation_cgroup_root)) {
        return reject("isolation.mode must be strict");
    }
    if (!parse_trusted_bytecode_keys(root, &config.trusted_keys)) {
        return reject("invalid host.json trustedBytecodeKeys");
    }

    json_t* defaults = json_object_get(root, "defaults");
    if (defaults != nullptr && !parse_tier(defaults, &config.defaults)) {
        return reject("invalid host.json defaults");
    }
    json_t* maximums = json_object_get(root, "maximums");
    if (maximums != nullptr) {
        if (!parse_tier(maximums, &config.maximums)) {
            return reject("invalid host.json maximums");
        }
        // Project the ceilings the policy compiler consumes (§9.2/§9.3/§10.3).
        config.policy.max_requests_per_worker =
            config.maximums.request.max_inflight_per_worker;
        config.policy.max_streaming_inflight_per_worker =
            config.maximums.request.max_streaming_inflight_per_worker;
        config.policy.max_stream_idle_timeout_ms =
            config.maximums.request.stream_idle_timeout_ms;
        config.policy.max_write_timeout_ms =
            config.maximums.request.write_timeout_ms;
        if (!config.maximums.worker.memory_max.empty()) {
            std::uint64_t bytes = 0;
            if (!parse_size_bytes_text(config.maximums.worker.memory_max,
                                       &bytes)) {
                return reject("invalid maximums.worker.memoryMax");
            }
            config.policy.max_worker_memory_bytes = bytes;
        }
        config.policy.max_queue_requests =
            config.maximums.pool.queue_requests;
        config.policy.max_queue_header_bytes =
            config.maximums.pool.queue_header_bytes;
        config.policy.max_queue_timeout_ms =
            config.maximums.pool.queue_timeout_ms;
    }
    if (!parse_capacity(root, &config.capacity)) {
        return reject("invalid host.json capacity");
    }
    if (config.capacity.workers_total > kMaxStaticPoolWorkers) {
        return reject("capacity.workersTotal exceeds the static pool worker limit");
    }
    // capacity.workersTotal is the single worker-count ceiling (see
    // policy_compiler.h); the parse gate above and the practical fixed-pool
    // limit guarantee it fits uint32.
    config.policy.max_workers =
        static_cast<std::uint32_t>(config.capacity.workers_total);
    if (!parse_recovery(root, &config.recovery)) {
        return reject("invalid host.json recovery");
    }

    if (config.applications_root.empty() || config.state_root.empty() ||
        config.admin_unix_path.empty() ||
        config.secret_root_template.find("{application}") ==
            std::string::npos) {
        return reject("host.json is missing a required root or admin path");
    }

    *out = std::move(config);
    json_decref(root);
    return true;
}

ResolvedRecoveryPolicy resolve_recovery_policy(const RecoveryConfig& config) {
    // Host defaults: a zero (absent) field maps to the value the strict
    // GenerationPool::create_adopted validation expects for a running Host.
    // The jitter text is a percent or decimal-basis-points grammar; the
    // schema validated it, so an unparseable value here is a startup error.
    ResolvedRecoveryPolicy out;
    out.policy.max_events =
        config.crash_budget.max_events > 0
            ? config.crash_budget.max_events
            : 5;
    if (out.policy.max_events > kMaxTrackedInstabilityEvents) {
        out.error = "crash budget exceeds the tracked-event window";
        return out;
    }
    out.policy.window_ms =
        config.crash_budget.window_ms > 0 ? config.crash_budget.window_ms
                                          : 60000;
    out.policy.backoff_initial_ms =
        config.restart_backoff.initial_ms > 0
            ? config.restart_backoff.initial_ms
            : 100;
    out.policy.backoff_maximum_ms =
        config.restart_backoff.maximum_ms > 0
            ? config.restart_backoff.maximum_ms
            : 10000;
    if (out.policy.backoff_maximum_ms < out.policy.backoff_initial_ms) {
        out.error = "restart backoff maximum precedes its initial delay";
        return out;
    }
    // "20%" = 2000 basis points, "1000" = 1000 basis points (10%);
    // absent/empty text takes the Host default. The schema validated the
    // grammar, so an unparseable value here is a startup error.
    const std::string& jitter_text = config.restart_backoff.jitter;
    if (jitter_text.empty()) {
        out.policy.jitter_basis_points = 1000;
    } else {
        std::uint64_t parsed = 0;
        if (!parse_jitter_basis_points_text(jitter_text, &parsed) ||
            parsed > std::numeric_limits<std::uint32_t>::max()) {
            out.error = "invalid restart backoff jitter";
            return out;
        }
        out.policy.jitter_basis_points = static_cast<std::uint32_t>(parsed);
    }
    // host.json has no stability-window field (health interval is the
    // PROBE cadence, not the backoff reset); the Host default always
    // applies.
    out.policy.stable_reset_ms = 60000;
    out.policy.replacements_concurrent_per_app =
        config.replacements_concurrent_per_app > 0
            ? config.replacements_concurrent_per_app
            : 1;
    out.ok = true;
    return out;
}

}  // namespace capsid::host
