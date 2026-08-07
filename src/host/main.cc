// capsid-host executable entry point.
//
// The frozen M1A CLI is strictly validated before anything is spawned:
// unknown or missing arguments fail before any side effect. Startup order is
// fixed by the design: validate arguments, read and load the source bundle,
// spawn the worker, wait for READY and verify the compatibility ID, bind the
// listener, and only then write one canonical JSON line to --ready-fd.
// stdout never carries readiness or logs; diagnostics go to stderr.

#include "host/single_worker_server.h"
#include "host/static_pool_server.h"

#include "build_identity.h"
#include "capsid/runtime.h"
#include "host/admin_service.h"
#include "host/config.h"
#include "host/managed_admin_backend.h"
#include "host/worker_supervisor.h"

#include <jansson.h>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "host/active_state.h"
#include "host/request_normalization.h"

namespace {

constexpr std::string_view kProbeGeneration =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

void fail(const std::string& message) {
    std::fprintf(stderr, "capsid-host: %s\n", message.c_str());
    std::exit(2);
}

bool valid_application_id(const std::string& application) {
    capsid::host::ActiveStateDocument probe;
    probe.state = capsid::host::ActiveServiceState::kActive;
    probe.application = application;
    probe.version = "v0";
    probe.generation = std::string(kProbeGeneration);
    return capsid::host::encode_active_state_json(probe).ok;
}

std::uint64_t parse_positive_integer(const std::string& value,
                                     const char* name) {
    if (value.empty()) {
        fail(std::string("--") + name + " requires a positive integer");
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            fail(std::string("--") + name +
                 " requires a positive integer: " + value);
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(
                      std::numeric_limits<std::int64_t>::max())) {
        fail(std::string("--") + name +
             " requires a positive integer: " + value);
    }
    return static_cast<std::uint64_t>(parsed);
}

// Non-negative integer: allows the explicit 0 that disables a feature
// (e.g. --queue-requests 0).
std::uint64_t parse_nonnegative_integer(const std::string& value,
                                        const char* name) {
    if (value.empty()) {
        fail(std::string("--") + name + " requires a non-negative integer");
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            fail(std::string("--") + name +
                 " requires a non-negative integer: " + value);
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' ||
        parsed > static_cast<unsigned long long>(
                      std::numeric_limits<std::int64_t>::max())) {
        fail(std::string("--") + name +
             " requires a non-negative integer: " + value);
    }
    return static_cast<std::uint64_t>(parsed);
}

// "250ms" / "5s" / "1m" style duration (the same grammar the effective
// config uses for queueTimeout). Unit suffix is required.
std::uint64_t parse_duration_ms(const std::string& value,
                                const char* name) {
    std::string::size_type number_end = 0;
    for (std::string::size_type index = 0; index < value.size(); ++index) {
        const char c = value[index];
        if (c < '0' || c > '9') {
            break;
        }
        number_end = index + 1;
    }
    if (number_end == 0 || number_end == value.size()) {
        fail(std::string("--") + name +
             " requires a duration like 250ms or 5s: " + value);
    }
    const std::string number_text = value.substr(0, number_end);
    const std::string unit = value.substr(number_end);
    std::uint64_t multiplier = 0;
    if (unit == "ms") {
        multiplier = 1;
    } else if (unit == "s") {
        multiplier = 1000;
    } else if (unit == "m") {
        multiplier = 60U * 1000U;
    } else {
        fail(std::string("--") + name +
             " requires ms, s or m units: " + value);
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed =
        std::strtoull(number_text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(
                      std::numeric_limits<std::uint64_t>::max() /
                      multiplier)) {
        fail(std::string("--") + name + " is out of range: " + value);
    }
    return static_cast<std::uint64_t>(parsed) * multiplier;
}

// host:port; only decimal ports and non-empty hosts are accepted.
void parse_listen(const std::string& value,
                  std::string* out_address,
                  std::uint16_t* out_port) {
    const std::size_t colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 == value.size()) {
        fail("--listen requires host:port");
    }
    *out_address = value.substr(0, colon);
    const std::string port_text = value.substr(colon + 1);
    for (const char c : port_text) {
        if (c < '0' || c > '9') {
            fail("--listen port must be decimal: " + value);
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(port_text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed > 65535) {
        fail("--listen port must be in [0, 65535]: " + value);
    }
    *out_port = static_cast<std::uint16_t>(parsed);
}

std::vector<std::uint8_t> read_bundle(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("cannot read --source-bundle: " + path);
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        fail("cannot size --source-bundle: " + path);
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(size));
        if (!input) {
            fail("cannot read --source-bundle: " + path);
        }
    }
    return bytes;
}

// ---- managed mode: host.json authority, real coordinator, Admin service ----

// Process-level stop signal. SIGTERM is blocked process-wide and waited
// for with sigwait on the main thread, so no C++ object is ever touched
// inside a signal handler.
std::atomic<bool> g_stop{false};

// Parsed host.json fields that drive the managed mode. The authoritative
// schema validation runs first (validate_config_json); this extraction maps
// the validated document onto the coordinator options and fails closed on
// anything it cannot map.
struct ManagedConfig {
    std::string applications_root;
    std::string state_root;
    std::string secret_root_template;  // contains "{application}"
    std::string admin_unix_path;
    mode_t admin_mode = 0600;
    capsid::host::HostPolicy policy;
    // capacity.workersTotal: the process-global worker permit. It is
    // consumed before any spawn/durable activation and returned when the
    // operation settles. startupsConcurrent is NOT reinterpreted as the
    // Admin pending-queue ceiling.
    int worker_capacity = 1;
    // recovery.*: the worker supervisor's crash-budget/backoff policy
    // (design §10.5). Defaults mirror the documented example and are
    // applied during extraction; the pure decision functions fail closed
    // on a policy they cannot validate.
    capsid::host::WorkerRecoveryPolicy recovery;
};

std::string json_string_field(json_t* object, const char* key) {
    json_t* value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : std::string();
}

// "host:port" / "host" (any port); decimal ports only.
bool parse_fetch_target_text(const std::string& text,
                             capsid::host::FetchTarget* out) {
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

// "256MiB" style size with explicit suffix (same grammar as the managed
// coordinator's worker.memoryMax).
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

bool parse_managed_config(const std::string& json, ManagedConfig* out,
                          std::string* error) {
    json_error_t parse_error;
    json_t* root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root)) {
        *error = "invalid host.json";
        if (root) {
            json_decref(root);
        }
        return false;
    }
    out->applications_root = json_string_field(root, "applicationsRoot");
    out->state_root = json_string_field(root, "stateRoot");
    out->secret_root_template = json_string_field(root, "secretRootTemplate");
    json_t* admin = json_object_get(root, "admin");
    if (json_is_object(admin)) {
        out->admin_unix_path = json_string_field(admin, "unix");
        const std::string mode_text = json_string_field(admin, "mode");
        if (!mode_text.empty()) {
            char* end = nullptr;
            const unsigned long parsed =
                std::strtoul(mode_text.c_str(), &end, 8);
            if (end == nullptr || *end != '\0' || parsed > 07777) {
                *error = "invalid admin.mode";
                json_decref(root);
                return false;
            }
            // host-v1 has no management-group field, so a
            // group/world-accessible pathname cannot be authorized
            // coherently; managed mode keeps the socket at exact 0600.
            if (static_cast<mode_t>(parsed) != 0600) {
                *error = "admin.mode must be 0600";
                json_decref(root);
                return false;
            }
            out->admin_mode = static_cast<mode_t>(parsed);
        }
    }
    json_t* permissions = json_object_get(root, "permissions");
    if (json_is_object(permissions)) {
        if (!parse_string_array(permissions, "modules",
                                &out->policy.module_allowlist) ||
            !parse_string_array(permissions, "environmentNames",
                                &out->policy.env_patterns) ||
            !parse_string_array(permissions, "fsReadRoots",
                                &out->policy.fs_read_roots) ||
            !parse_string_array(permissions, "storageNamespaces",
                                &out->policy.storage_namespaces) ||
            !parse_string_array(permissions, "stdioStreams",
                                &out->policy.stdio_streams)) {
            *error = "invalid host.json permissions";
            json_decref(root);
            return false;
        }
        out->policy.storage_allowed = !out->policy.storage_namespaces.empty();
        out->policy.stdio_allowed = !out->policy.stdio_streams.empty();
        std::vector<std::string> targets;
        if (!parse_string_array(permissions, "fetchTargets", &targets)) {
            *error = "invalid host.json fetchTargets";
            json_decref(root);
            return false;
        }
        for (const std::string& target : targets) {
            capsid::host::FetchTarget parsed;
            if (!parse_fetch_target_text(target, &parsed)) {
                *error = "invalid host.json fetchTargets entry";
                json_decref(root);
                return false;
            }
            out->policy.fetch_targets.push_back(std::move(parsed));
        }
    }
    json_t* isolation = json_object_get(root, "isolation");
    if (json_is_object(isolation)) {
        const std::string mode = json_string_field(isolation, "mode");
        if (!mode.empty() && mode != "strict") {
            *error = "isolation.mode must be strict";
            json_decref(root);
            return false;
        }
    }
    json_t* maximums = json_object_get(root, "maximums");
    if (json_is_object(maximums)) {
        json_t* request = json_object_get(maximums, "request");
        if (json_is_object(request)) {
            json_t* inflight =
                json_object_get(request, "maxInflightPerWorker");
            if (json_is_integer(inflight)) {
                out->policy.max_requests_per_worker =
                    static_cast<std::uint64_t>(json_integer_value(inflight));
            }
            // E-2 SSE-permit caps (maximums.request.*, §9.3): the Host
            // ceilings the App stream config is compiled against in
            // compile_policy. Each cap is optional; 0 = no ceiling.
            json_t* max_streaming = json_object_get(
                request, "maxStreamingInflightPerWorker");
            if (json_is_integer(max_streaming)) {
                const json_int_t value = json_integer_value(max_streaming);
                if (value < 0) {
                    *error = "invalid maximums.request."
                             "maxStreamingInflightPerWorker";
                    json_decref(root);
                    return false;
                }
                out->policy.max_streaming_inflight_per_worker =
                    static_cast<std::uint64_t>(value);
            }
            json_t* stream_idle =
                json_object_get(request, "streamIdleTimeoutMs");
            if (json_is_integer(stream_idle)) {
                const json_int_t value = json_integer_value(stream_idle);
                if (value < 0) {
                    *error = "invalid maximums.request.streamIdleTimeoutMs";
                    json_decref(root);
                    return false;
                }
                out->policy.max_stream_idle_timeout_ms =
                    static_cast<std::uint64_t>(value);
            }
            // E-3 slow-client write deadline cap (§9.2): 0 = no ceiling.
            json_t* write_timeout =
                json_object_get(request, "writeTimeoutMs");
            if (json_is_integer(write_timeout)) {
                const json_int_t value = json_integer_value(write_timeout);
                if (value < 0) {
                    *error = "invalid maximums.request.writeTimeoutMs";
                    json_decref(root);
                    return false;
                }
                out->policy.max_write_timeout_ms =
                    static_cast<std::uint64_t>(value);
            }
        }
        json_t* worker = json_object_get(maximums, "worker");
        if (json_is_object(worker)) {
            json_t* memory = json_object_get(worker, "memoryMax");
            if (json_is_string(memory)) {
                std::uint64_t bytes = 0;
                if (!parse_size_bytes_text(json_string_value(memory),
                                           &bytes)) {
                    *error = "invalid maximums.worker.memoryMax";
                    json_decref(root);
                    return false;
                }
                out->policy.max_worker_memory_bytes = bytes;
            }
        }
        // E-1 admission-queue caps (maximums.pool.*, §10.3): the Host
        // ceilings the App queue is compiled against in compile_policy.
        // Each cap is optional; an absent cap leaves the App free to set
        // its own queue values.
        json_t* pool = json_object_get(maximums, "pool");
        if (json_is_object(pool)) {
            json_t* queue_requests = json_object_get(pool, "queueRequests");
            if (json_is_integer(queue_requests)) {
                const json_int_t value = json_integer_value(queue_requests);
                if (value < 0) {
                    *error = "invalid maximums.pool.queueRequests";
                    json_decref(root);
                    return false;
                }
                out->policy.max_queue_requests =
                    static_cast<std::uint64_t>(value);
            }
            json_t* queue_header_bytes =
                json_object_get(pool, "queueHeaderBytes");
            if (json_is_string(queue_header_bytes)) {
                std::uint64_t bytes = 0;
                if (!parse_size_bytes_text(json_string_value(queue_header_bytes),
                                           &bytes)) {
                    *error = "invalid maximums.pool.queueHeaderBytes";
                    json_decref(root);
                    return false;
                }
                out->policy.max_queue_header_bytes = bytes;
            }
            json_t* queue_timeout = json_object_get(pool, "queueTimeout");
            if (json_is_string(queue_timeout)) {
                out->policy.max_queue_timeout_ms = parse_duration_ms(
                    json_string_value(queue_timeout), "maximums.pool.queueTimeout");
            }
        }
    }
    json_t* capacity = json_object_get(root, "capacity");
    if (json_is_object(capacity)) {
        json_t* workers = json_object_get(capacity, "workersTotal");
        if (json_is_integer(workers)) {
            const json_int_t value = json_integer_value(workers);
            if (value <= 0) {
                *error = "capacity.workersTotal must be positive";
                json_decref(root);
                return false;
            }
            out->worker_capacity = static_cast<int>(value);
        }
    }
    // recovery.* (design §10.5): crashBudget/restartBackoff feed the
    // worker supervisor's pure decision functions. All fields are
    // optional; the documented defaults are applied below. jitter is a
    // percentage string ("20%") mapped to basis points (20% -> 2000).
    json_t* recovery = json_object_get(root, "recovery");
    if (json_is_object(recovery)) {
        json_t* crash_budget = json_object_get(recovery, "crashBudget");
        if (json_is_object(crash_budget)) {
            json_t* max_events = json_object_get(crash_budget, "maxEvents");
            if (json_is_integer(max_events)) {
                const json_int_t value = json_integer_value(max_events);
                if (value <= 0 ||
                    value > static_cast<json_int_t>(
                                 capsid::host::kMaxTrackedInstabilityEvents)) {
                    *error = "recovery.crashBudget.maxEvents out of range";
                    json_decref(root);
                    return false;
                }
                out->recovery.max_events =
                    static_cast<std::uint32_t>(value);
            }
            json_t* window = json_object_get(crash_budget, "window");
            if (json_is_string(window)) {
                out->recovery.window_ms = parse_duration_ms(
                    json_string_value(window),
                    "recovery.crashBudget.window");
            }
        }
        json_t* restart_backoff =
            json_object_get(recovery, "restartBackoff");
        if (json_is_object(restart_backoff)) {
            json_t* initial = json_object_get(restart_backoff, "initial");
            if (json_is_string(initial)) {
                out->recovery.backoff_initial_ms = parse_duration_ms(
                    json_string_value(initial),
                    "recovery.restartBackoff.initial");
            }
            json_t* maximum = json_object_get(restart_backoff, "maximum");
            if (json_is_string(maximum)) {
                out->recovery.backoff_maximum_ms = parse_duration_ms(
                    json_string_value(maximum),
                    "recovery.restartBackoff.maximum");
            }
            json_t* stable_reset =
                json_object_get(restart_backoff, "stableReset");
            if (json_is_string(stable_reset)) {
                out->recovery.stable_reset_ms = parse_duration_ms(
                    json_string_value(stable_reset),
                    "recovery.restartBackoff.stableReset");
            }
            json_t* jitter = json_object_get(restart_backoff, "jitter");
            if (json_is_string(jitter)) {
                const std::string text = json_string_value(jitter);
                bool valid = !text.empty() && text.back() == '%';
                std::uint64_t percent = 0;
                if (valid) {
                    for (const char c : text.substr(0, text.size() - 1)) {
                        if (c < '0' || c > '9') {
                            valid = false;
                            break;
                        }
                        percent = percent * 10 +
                                  static_cast<unsigned>(c - '0');
                    }
                }
                if (!valid || percent > 100) {
                    *error =
                        "recovery.restartBackoff.jitter must be a "
                        "percentage like \"20%\"";
                    json_decref(root);
                    return false;
                }
                out->recovery.jitter_basis_points =
                    static_cast<std::uint32_t>(percent * 100);
            }
        }
        json_t* concurrent =
            json_object_get(recovery, "replacementsConcurrentPerApp");
        if (json_is_integer(concurrent)) {
            const json_int_t value = json_integer_value(concurrent);
            if (value <= 0) {
                *error =
                    "recovery.replacementsConcurrentPerApp must be positive";
                json_decref(root);
                return false;
            }
            out->recovery.replacements_concurrent_per_app =
                static_cast<std::uint32_t>(value);
        }
    }
    // Defaults mirror the documented recovery shape (maxEvents 5 / 60s
    // window / 250ms..30s backoff / 20% jitter / 60s stable reset / 1
    // concurrent replacement). Zero marks "not set": the decision
    // functions require every field positive, so an explicit zero would
    // fail closed at startup instead of silently disabling recovery.
    if (out->recovery.max_events == 0) {
        out->recovery.max_events = 5;
    }
    if (out->recovery.window_ms == 0) {
        out->recovery.window_ms = 60000;
    }
    if (out->recovery.backoff_initial_ms == 0) {
        out->recovery.backoff_initial_ms = 250;
    }
    if (out->recovery.backoff_maximum_ms == 0) {
        out->recovery.backoff_maximum_ms = 30000;
    }
    if (out->recovery.jitter_basis_points == 0) {
        out->recovery.jitter_basis_points = 2000;
    }
    if (out->recovery.stable_reset_ms == 0) {
        out->recovery.stable_reset_ms = 60000;
    }
    if (out->recovery.replacements_concurrent_per_app == 0) {
        out->recovery.replacements_concurrent_per_app = 1;
    }
    if (out->applications_root.empty() || out->state_root.empty() ||
        out->admin_unix_path.empty() ||
        out->secret_root_template.find("{application}") ==
            std::string::npos) {
        *error = "host.json is missing a required root or admin path";
        json_decref(root);
        return false;
    }
    json_decref(root);
    return true;
}

// Safe open of a Host-owned directory: O_NOFOLLOW, directory, euid owner,
// no group/other bits.
int open_verified_root(const std::string& path, const char* what) {
    const int fd = open(path.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        fail(std::string("cannot open ") + what);
    }
    struct stat st = {};
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        close(fd);
        fail(std::string("unverified ") + what);
    }
    return fd;
}

// Valid App ID (frozen lowercase grammar).
bool valid_managed_app_id(const std::string& value) {
    if (value.empty() || value.size() > 63) {
        return false;
    }
    if (!((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) {
        return false;
    }
    for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// True when the App has a durable active-state document (an active
// generation or a retired tombstone). Recovery only needs a permit for
// those; an App with no state cannot consume capacity.
bool has_durable_active_state(const std::string& state_root,
                              const std::string& application) {
    const std::string path =
        state_root + "/apps/" + application + "/active.json";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

// Discovers configured Apps beneath the verified applications root.
std::vector<std::string> discover_applications(int apps_fd) {
    std::vector<std::string> applications;
    DIR* dir = fdopendir(dup(apps_fd));
    if (dir == nullptr) {
        fail("cannot enumerate applications root");
    }
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == nullptr) {
            if (errno != 0 && errno != EINTR) {
                fail("cannot enumerate applications root");
            }
            break;
        }
        const std::string name(entry->d_name);
        if (!valid_managed_app_id(name)) {
            continue;
        }
        struct stat st = {};
        if (fstatat(apps_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(st.st_mode)) {
            applications.push_back(name);
        }
    }
    closedir(dir);
    return applications;
}

// Cross-platform stat timestamp accessors: Apple spells the fields
// st_mtimespec/st_ctimespec; other POSIX systems use st_mtim/st_ctim
// (same pattern as the coordinator's platform macros).
#if defined(__APPLE__)
#define CAPSID_MAIN_MTIME_SEC(st) ((st).st_mtimespec.tv_sec)
#define CAPSID_MAIN_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define CAPSID_MAIN_CTIME_SEC(st) ((st).st_ctimespec.tv_sec)
#define CAPSID_MAIN_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#else
#define CAPSID_MAIN_MTIME_SEC(st) ((st).st_mtim.tv_sec)
#define CAPSID_MAIN_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define CAPSID_MAIN_CTIME_SEC(st) ((st).st_ctim.tv_sec)
#define CAPSID_MAIN_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

// Bounded read of the host.json document: O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,
// only a Host-owned regular file, a size cap checked before reading and a
// device/inode/size re-check after reading (a FIFO or symlink never blocks
// or redirects startup, and a concurrent swap is rejected).
std::string read_host_config(const std::string& path) {
    const int fd = open(path.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        fail("cannot open --host-config: " + path);
    }
    struct stat before = {};
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_uid != geteuid() || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > 1024U * 1024U) {
        close(fd);
        fail("--host-config is not a Host-owned regular file under 1 MiB");
    }
    std::string json;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            close(fd);
            fail("cannot read --host-config");
        }
        if (count == 0) {
            break;
        }
        if (json.size() + static_cast<std::size_t>(count) > 1024U * 1024U) {
            close(fd);
            fail("--host-config exceeds 1 MiB");
        }
        json.append(buffer, static_cast<std::size_t>(count));
    }
    struct stat after = {};
    if (fstat(fd, &after) != 0 || after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino || after.st_size != before.st_size ||
        CAPSID_MAIN_MTIME_SEC(after) != CAPSID_MAIN_MTIME_SEC(before) ||
        CAPSID_MAIN_MTIME_NSEC(after) != CAPSID_MAIN_MTIME_NSEC(before) ||
        CAPSID_MAIN_CTIME_SEC(after) != CAPSID_MAIN_CTIME_SEC(before) ||
        CAPSID_MAIN_CTIME_NSEC(after) != CAPSID_MAIN_CTIME_NSEC(before)) {
        close(fd);
        fail("--host-config changed while it was read");
    }
    close(fd);
    return json;
}

int run_managed(const std::string& host_config_path,
                const std::string& worker_path) {
    // SIGTERM is blocked FIRST, before any thread exists: every thread
    // inherits the mask, and the main thread waits with sigwait below.
    sigset_t term_set;
    sigemptyset(&term_set);
    sigaddset(&term_set, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &term_set, nullptr) != 0) {
        fail("cannot block SIGTERM");
    }
    const std::string host_json = read_host_config(host_config_path);
    // The authoritative schema boundary runs first; no second lax parser.
    const capsid::host::ConfigValidationResult validated =
        capsid::host::validate_config_json(
            capsid::host::ConfigDocument::kHost, host_json);
    if (!validated.ok) {
        fail("host.json rejected at " + validated.error.path + ": " +
             validated.error.message);
    }
    ManagedConfig config;
    std::string config_error;
    if (!parse_managed_config(host_json, &config, &config_error)) {
        fail(config_error);
    }
    // Safe-open the roots; the secret template's parent is the dirfd the
    // coordinator opens App subdirectories from.
    const int apps_fd = open_verified_root(config.applications_root,
                                           "applications root");
    // The template must be EXACTLY <parent>/{application}: a trailing
    // suffix would be silently misinterpreted as part of the parent root.
    const std::string secret_template = config.secret_root_template;
    const std::string::size_type placeholder =
        secret_template.find("/{application}");
    const std::string secret_parent =
        secret_template.substr(0, placeholder);
    if (placeholder == std::string::npos || secret_parent.empty() ||
        secret_template.size() !=
            secret_parent.size() + std::string("/{application}").size()) {
        fail("secretRootTemplate must be exactly "
             "<root>/{application}");
    }
    const int secrets_fd = open_verified_root(secret_parent,
                                              "secret root template");
    const std::vector<std::string> applications =
        discover_applications(apps_fd);
    if (applications.empty()) {
        fail("applications root contains no configured Apps");
    }
    // Stable ownership for every App's options (the coordinator stores
    // pointers to them).
    std::vector<std::unique_ptr<capsid::host::ManagedHostOptions>> owned;
    std::vector<capsid::host::ManagedHostOptions*> app_options;
    for (const std::string& application : applications) {
        auto options = std::make_unique<capsid::host::ManagedHostOptions>();
        options->applications_root_fd = apps_fd;
        options->secret_root_template_fd = secrets_fd;
        options->state_root = config.state_root;
        options->application = application;
        options->worker_path = worker_path;
        options->host_policy = config.policy;
        options->recovery_policy = config.recovery;
        options->runtime_compatibility_id = CAPSID_BUILD_COMPATIBILITY_ID;
        options->stop_requested = &g_stop;
        app_options.push_back(options.get());
        owned.push_back(std::move(options));
    }
    // Active workers owned by this process: App -> worker.
    std::map<std::string, capsid_worker*> active_workers;
    std::mutex workers_mutex;
    const auto activate_worker =
        [&](const std::string& application, capsid_worker* worker) {
            std::lock_guard<std::mutex> lock(workers_mutex);
            const std::map<std::string, capsid_worker*>::iterator existing =
                active_workers.find(application);
            if (existing != active_workers.end() &&
                existing->second != nullptr) {
                // Activation replaces the previous worker.
                capsid_worker_destroy(existing->second);
            }
            active_workers[application] = worker;
            return true;
        };
    const auto retire_worker = [&](const std::string& application) {
        std::lock_guard<std::mutex> lock(workers_mutex);
        const std::map<std::string, capsid_worker*>::iterator existing =
            active_workers.find(application);
        if (existing != active_workers.end() &&
            existing->second != nullptr) {
            capsid_worker_destroy(existing->second);
            active_workers.erase(existing);
        }
    };
    // The supervisor destroys a dead worker ONLY when the map still names
    // exactly that worker: a raced deploy's live worker is never destroyed
    // by the supervisor.
    const auto remove_if_current =
        [&](const std::string& application, capsid_worker* worker) {
            std::lock_guard<std::mutex> lock(workers_mutex);
            const std::map<std::string, capsid_worker*>::iterator existing =
                active_workers.find(application);
            if (existing != active_workers.end() &&
                existing->second == worker) {
                capsid_worker_destroy(existing->second);
                active_workers.erase(existing);
            }
        };
    const auto discard_worker = [&](capsid_worker* worker) {
        // A worker the supervisor itself spawned but could not publish:
        // never visible in the map, destroyed directly.
        capsid_worker_destroy(worker);
    };
    const auto reclaim_workers = [&]() {
        std::lock_guard<std::mutex> lock(workers_mutex);
        for (const auto& entry : active_workers) {
            if (entry.second != nullptr) {
                capsid_worker_destroy(entry.second);
            }
        }
        active_workers.clear();
    };
    // The process-global worker permit (capacity.workersTotal). The slot
    // is bound to an active App: replacements do not re-acquire, a failed
    // replacement keeps the old slot, and only a newly acquired permit is
    // returned when the operation settles without a live worker.
    capsid::host::WorkerCapacityPermit capacity(config.worker_capacity);
    // Startup recovery: a durable active App is revalidated and its
    // replacement worker reaches READY before Admin readiness is
    // published. The global permit is acquired BEFORE any spawn; an
    // active-generation count beyond capacity fails closed at startup
    // instead of overspawning first.
    for (capsid::host::ManagedHostOptions* options : app_options) {
        // Only an App with durable state may consume a permit; a fresh
        // App never occupies capacity before its first deploy.
        bool newly_acquired = false;
        if (has_durable_active_state(options->state_root,
                                     options->application)) {
            newly_acquired = capacity.acquire(options->application);
            if (!newly_acquired &&
                !capacity.holds(options->application)) {
                fail("active generation count exceeds "
                     "capacity.workersTotal");
            }
        }
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(options, &status);
        if (!recovered.ok) {
            if (newly_acquired) {
                capacity.release(options->application);
            }
            fail("cannot recover active application " + options->application);
        }
        if (recovered.worker != nullptr) {
            // READY succeeded: the recovered worker holds the slot.
            capacity.record_success(options->application);
            activate_worker(options->application, recovered.worker);
        } else if (newly_acquired) {
            // No active/retired generation: no permanent occupancy.
            capacity.release(options->application);
        }
    }
    // M2 item 5a: one supervisor thread per App owns the observation of
    // the current worker's IPC stream and the replacement/quarantine
    // decisions derived from it (design §10.5). Created after startup
    // recovery so recovered workers anchor the recovery state; joined on
    // shutdown after the worker reclaim closes the observed channels.
    std::vector<std::unique_ptr<capsid::host::WorkerSupervisor>> supervisors;
    for (capsid::host::ManagedHostOptions* options : app_options) {
        capsid::host::WorkerSupervisorOptions supervisor_options;
        supervisor_options.managed_options = options;
        supervisor_options.policy = config.recovery;
        supervisor_options.current_worker =
            [&](const std::string& application) -> capsid_worker* {
                std::lock_guard<std::mutex> lock(workers_mutex);
                const std::map<std::string, capsid_worker*>::const_iterator
                    existing = active_workers.find(application);
                return existing != active_workers.end() ? existing->second
                                                        : nullptr;
            };
        supervisor_options.publish_worker = activate_worker;
        supervisor_options.remove_if_current = remove_if_current;
        supervisor_options.discard_worker = discard_worker;
        supervisor_options.stop_requested = &g_stop;
        supervisors.push_back(
            std::make_unique<capsid::host::WorkerSupervisor>(
                std::move(supervisor_options)));
    }
    capsid::host::ManagedAdminBackend managed(app_options);
    managed.capacity = &capacity;
    capsid::host::AsyncAdminBackendOptions async_options;
    // Fixed bounded queue; startupsConcurrent is not the Admin ceiling.
    async_options.max_pending_operations = 8;
    async_options.external_stop = &g_stop;
    async_options.activate_worker = activate_worker;
    async_options.retire_worker = retire_worker;
    auto async = std::make_unique<capsid::host::AsyncAdminBackend>(
        &managed, async_options);
    capsid::host::AdminServiceOptions service_options;
    service_options.socket.path = config.admin_unix_path;
    service_options.socket.mode = config.admin_mode;
    service_options.http.api.authorization.allowed_uid =
        static_cast<std::uint64_t>(geteuid());
    service_options.http.api.max_header_bytes = 64U * 1024U;
    service_options.http.api.max_body_bytes = 64U * 1024U;
    service_options.http.header_timeout_ms = 5000;
    service_options.http.body_timeout_ms = 5000;
    service_options.http.write_timeout_ms = 5000;
    capsid::host::AdminService service(service_options, async.get());
    std::string service_error;
    if (!service.start(&service_error)) {
        fail("cannot start admin service: " + service_error);
    }
    // Shutdown order: stop the Admin control plane, cancel queued/running
    // deploys, reclaim active workers, then exit. The service's wait
    // removes the socket inode it created.
    int signal_number = 0;
    if (sigwait(&term_set, &signal_number) != 0) {
        fail("cannot wait for SIGTERM");
    }
    g_stop.store(true);
    service.request_stop();
    if (!service.wait(&service_error)) {
        fail("admin service failed: " + service_error);
    }
    // Explicitly wait for the async executor to settle (queued work
    // cancelled, running deploys interrupted) BEFORE reclaiming workers;
    // shutdown never depends on destructor ordering at function return.
    async.reset();
    reclaim_workers();
    // The reclaim destroyed every observed worker, which closes its IPC fd;
    // the supervisor threads see the channel die and exit. Joined after the
    // reclaim so their stop is a no-op observation, never a counted event.
    for (const auto& supervisor : supervisors) {
        supervisor->join();
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> values;
    for (int index = 1; index < argc; index += 2) {
        const char* key = argv[index];
        const char* value = index + 1 < argc ? argv[index + 1] : nullptr;
        if (key == nullptr || key[0] != '-' || key[1] != '-') {
            fail("arguments must be --key value pairs");
        }
        if (value == nullptr || value[0] == '\0') {
            fail(std::string("missing value for ") + key);
        }
        const std::string name = key + 2;
        if (values.find(name) != values.end()) {
            fail("duplicate argument: --" + name);
        }
        values[name] = value;
    }

    const auto require = [&values](const std::string& name) -> std::string {
        auto it = values.find(name);
        if (it == values.end()) {
            fail("missing --" + name);
        }
        return it->second;
    };

    const std::string mode = require("mode");
    if (mode == "managed") {
        // Strict managed CLI: only --host-config and --worker are allowed.
        for (const std::pair<const std::string, std::string>& entry :
             values) {
            if (entry.first != "mode" && entry.first != "host-config" &&
                entry.first != "worker") {
                fail("--mode managed accepts only --host-config and "
                     "--worker");
            }
        }
        return run_managed(require("host-config"), require("worker"));
    }
    if (mode != "single-worker" && mode != "static-pool") {
        fail("--mode must be single-worker, static-pool or managed");
    }
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = require("worker");
    options.source_bundle_path = require("source-bundle");
    options.source_name = require("source-name");
    if (options.source_name.rfind("file://", 0) != 0) {
        fail("--source-name must be an absolute file URL");
    }
    options.application = require("application");
    if (!valid_application_id(options.application)) {
        fail("--application is not a valid App ID");
    }
    const std::string listen = require("listen");
    parse_listen(listen, &options.listen_address, &options.listen_port);
    // The address itself is validated before anything is spawned: an
    // unparseable address must fail the CLI phase, not the post-spawn bind
    // phase (a failure after spawn would otherwise have to tear down a live
    // worker).
    {
        boost::system::error_code address_error;
        const boost::asio::ip::address address =
            boost::asio::ip::make_address(options.listen_address,
                                          address_error);
        if (address_error) {
            fail("--listen requires an IP address: " +
                 options.listen_address);
        }
        (void)address;
    }
    const std::string routing = require("routing");
    if (routing != "path") {
        fail("--routing must be path in M1A");
    }
    options.public_scheme = require("public-scheme");
    if (options.public_scheme != "http" && options.public_scheme != "https") {
        fail("--public-scheme must be http or https");
    }
    options.public_authority = require("public-authority");
    if (!capsid::host::is_valid_public_authority(
            options.public_authority)) {
        fail("--public-authority must be host[:port]");
    }
    options.request_timeout_ms =
        parse_positive_integer(require("request-timeout-ms"),
                               "request-timeout-ms");
    const std::uint64_t window =
        parse_positive_integer(require("initial-stream-window"),
                               "initial-stream-window");
    if (window > std::numeric_limits<std::uint32_t>::max()) {
        fail("--initial-stream-window exceeds uint32");
    }
    options.initial_stream_window = static_cast<std::uint32_t>(window);
    const std::string sandbox = require("strict-sandbox");
    if (sandbox != "on" && sandbox != "off") {
        fail("--strict-sandbox must be on or off");
    }
    options.strict_sandbox = sandbox == "on";
    options.ready_fd = static_cast<int>(
        parse_positive_integer(require("ready-fd"), "ready-fd"));
    if (options.ready_fd <= 0 ||
        options.ready_fd > static_cast<int>(std::numeric_limits<short>::max())) {
        fail("--ready-fd must be a positive descriptor number");
    }
    // The READY record must be deliverable; verify the descriptor is open
    // before spawning the worker.
    if (fcntl(options.ready_fd, F_GETFD) == -1) {
        fail("--ready-fd is not an open descriptor");
    }

    // Benchmark-only static pool (NOT a managed production path): a fixed
    // 1/2/4-worker pool sharing one SO_REUSEPORT listener, driven by the
    // same worker/bundle/ready-fd parameters as the single-worker mode.
    // The pool keeps the pool-level READY contract and SIGTERM-bounded
    // shutdown; single-worker mode is unchanged.
    std::uint32_t workers = 1;
    if (mode == "static-pool") {
        const std::string workers_text = require("workers");
        workers = static_cast<std::uint32_t>(
            parse_positive_integer(workers_text, "workers"));
        // M2 pool sizing scans {1,2,4,6,8}; the benchmark-only entry
        // accepts exactly this set (admission-sized pools come later).
        if (workers != 1 && workers != 2 && workers != 4 &&
            workers != 6 && workers != 8) {
            fail("--workers must be 1, 2, 4, 6 or 8 in static-pool mode");
        }
    }

    // M2 E-1 admission (§10.3): the benchmark CLI mirrors the effective
    // config's request/pool fields (the production path compiles the same
    // values through config → effective tier; see policy_compiler.cc).
    // Every field is optional; a missing value keeps the data plane
    // default. There is no main.cc hardcoding — the values flow into
    // StaticPoolServerOptions / SingleWorkerServerOptions below.
    const auto optional_value = [&values](const std::string& name)
        -> const std::string* {
        const auto it = values.find(name);
        return it == values.end() ? nullptr : &it->second;
    };
    const std::string* inflight_text = optional_value("max-inflight-per-worker");
    if (inflight_text != nullptr) {
        options.max_inflight_per_worker = parse_positive_integer(
            *inflight_text, "max-inflight-per-worker");
    }
    const std::string* queue_text = optional_value("queue-requests");
    if (queue_text != nullptr) {
        options.queue_requests =
            parse_nonnegative_integer(*queue_text, "queue-requests");
    }
    const std::string* queue_bytes_text = optional_value("queue-header-bytes");
    if (queue_bytes_text != nullptr) {
        if (!parse_size_bytes_text(*queue_bytes_text, &options.queue_header_bytes)) {
            fail("--queue-header-bytes must be a byte size (e.g. 2MiB)");
        }
    }
    const std::string* queue_timeout_text = optional_value("queue-timeout");
    if (queue_timeout_text != nullptr) {
        options.queue_timeout_ms = parse_duration_ms(
            *queue_timeout_text, "queue-timeout");
    }
    // M2 E-2 SSE permit (§9.3): the benchmark CLI mirrors the effective
    // config's request fields. Unlike the JSON route (where 0 = field not
    // set), a direct 0 here means unlimited — the same data-plane semantics
    // as --max-inflight-per-worker 0.
    const std::string* streaming_text = optional_value("max-streaming-inflight");
    if (streaming_text != nullptr) {
        options.max_streaming_inflight_per_worker = parse_nonnegative_integer(
            *streaming_text, "max-streaming-inflight");
    }
    const std::string* idle_text = optional_value("stream-idle-timeout");
    if (idle_text != nullptr) {
        options.stream_idle_timeout_ms = parse_nonnegative_integer(
            *idle_text, "stream-idle-timeout");
    }
    // M2 E-3 slow-client write deadline (§9.2): CLI 0 = unlimited, the same
    // data-plane semantics as the other CLI fields above.
    const std::string* write_timeout_text = optional_value("write-timeout");
    if (write_timeout_text != nullptr) {
        options.write_timeout_ms = parse_nonnegative_integer(
            *write_timeout_text, "write-timeout");
    }

    const std::vector<std::uint8_t> bundle =
        read_bundle(options.source_bundle_path);

    if (mode == "static-pool") {
        capsid::host::StaticPoolServerOptions pool_options;
        pool_options.workers = workers;
        // The admission values ride in the shard template (options above);
        // the pool-level StaticPoolServerOptions admission fields exist for
        // the production config route (effective tier → pool options, M2
        // managed-pool batch) and override the template when set.
        pool_options.worker_options = std::move(options);
        capsid::host::StaticPoolServer pool(std::move(pool_options));
        return pool.run(bundle);
    }
    capsid::host::SingleWorkerServer server(std::move(options));
    return server.run(bundle);
}
