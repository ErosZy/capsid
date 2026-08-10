// WP-05 §9.1: typed Host configuration model. The schema boundary
// (validate_config_json) stays the authority; this model is the typed
// projection of every accepted host.json field. A field the schema accepts
// but this parse cannot map is a startup error — never a silent ignore.
//
// PR-08 scope: the model + the parse; listeners/recovery/capacity-extra are
// carried typed and consumed by the later data-plane PRs (09/10). The
// semantic gates that protect the coordinator stay here: admin.mode must be
// exactly 0600, isolation.mode must be "strict", capacity.workersTotal must
// be positive and within the Host limit.

#ifndef CAPSID_HOST_HOST_CONFIG_MODEL_H
#define CAPSID_HOST_HOST_CONFIG_MODEL_H

#include "host/policy_compiler.h"
#include "host/trusted_key_store.h"
#include "host/worker_recovery.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace capsid::host {

// ---- listener model (§9.2; wired into the data plane by PR-09) ---------

struct ListenerRoutingConfig {
    std::string mode;    // "path" | "subdomain" | "header"
    std::string suffix;  // header mode: the trusted request header
};

struct ListenerLimitsConfig {
    std::uint64_t connections = 0;          // 0 = no ceiling
    std::uint64_t header_bytes = 0;         // 0 = no ceiling
    std::uint64_t header_timeout_ms = 0;    // 0 = no ceiling
    std::uint64_t body_idle_timeout_ms = 0; // 0 = no ceiling
    std::uint64_t stream_idle_timeout_ms = 0; // 0 = no ceiling
};

struct ListenerConfig {
    std::string name;
    std::string tcp;             // "host" or "host:port"
    std::string public_scheme;   // "https" for trusted listeners
    std::string public_authority;
    // §9.2 trust boundary: header routing requires trusted == true. The
    // field is an explicit declaration, never inferred from the transport
    // (design review §3.7/§8.1); the listener adapter fails the bind when a
    // header-mode listener is not trusted.
    bool trusted = false;
    ListenerRoutingConfig routing;
    ListenerLimitsConfig limits;
};

// ---- tiers -------------------------------------------------------------

// Host-side defaults/maximums worker tier. All strings are the "256MiB" /
// "5s" grammars except the plain integers.
struct WorkerTierConfig {
    std::string js_heap;             // size text, empty when absent
    std::string process_address_space;  // size text
    std::string memory_max;          // size text
    std::uint64_t file_descriptors = 0;
    std::uint64_t pids_max = 0;
};

struct RequestTierConfig {
    std::string timeout;             // duration text
    std::uint64_t max_inflight_per_worker = 0;
    std::uint64_t max_streaming_inflight_per_worker = 0;
    std::uint64_t stream_idle_timeout_ms = 0;
    std::uint64_t write_timeout_ms = 0;
};

struct PoolTierConfig {
    std::uint64_t queue_requests = 0;
    std::uint64_t queue_header_bytes = 0;
    std::uint64_t queue_timeout_ms = 0;
};

struct TierConfig {
    WorkerTierConfig worker;
    RequestTierConfig request;
    PoolTierConfig pool;
};

// ---- capacity / recovery ------------------------------------------------

// §9.4: workersTotal is the steady-state budget; the remaining capacity
// fields are carried typed for the PR-10 weighted ledger. startupsConcurrent
// is the startup-permit limit, NOT the Admin pending-queue ceiling.
struct CapacityConfig {
    std::uint64_t workers_total = 1;
    // §9.4: the extra budget for the new-warming + old-draining overlap
    // of a replacement deploy. v1 default 0 — a zero-downtime replace of
    // a serving pool is refused without surge/headroom (never silently
    // over-spawned); a fresh deploy never needs it.
    std::uint64_t activation_surge_workers = 0;
    std::uint64_t startups_concurrent = 0;   // 0 = Host default
    std::uint64_t queued_requests_total = 0;
    std::uint64_t queued_header_bytes_total = 0;
    std::uint64_t worker_memory_commit_total = 0;
};

struct CrashBudgetConfig {
    std::uint64_t max_events = 0;
    std::uint64_t window_ms = 0;
};

struct RestartBackoffConfig {
    std::uint64_t initial_ms = 0;
    std::uint64_t maximum_ms = 0;
    std::string jitter;  // basis-points text ("1000" = 10%)
};

struct RecoveryConfig {
    CrashBudgetConfig crash_budget;
    RestartBackoffConfig restart_backoff;
    std::uint64_t replacements_concurrent_per_app = 0;
    std::uint64_t active_health_interval_ms = 0;
    std::uint64_t active_health_failures = 0;
};

// ---- the model ----------------------------------------------------------

struct ParsedHostConfig {
    std::string applications_root;
    std::string state_root;
    std::string secret_root_template;  // contains "{application}"
    std::string admin_unix_path;
    unsigned admin_mode = 0600;
    std::vector<ListenerConfig> listeners;  // empty = admin-only mode
    // Compiled from permissions + maximums + capacity (see
    // policy_compiler.h for the field semantics).
    HostPolicy policy;
    // isolation.mode ("strict") and the required feature list. cgroupRoot
    // is carried typed; the sandbox launcher consumes it (WP-08).
    std::string isolation_mode;
    std::vector<std::string> isolation_required;
    std::string isolation_cgroup_root;
    // trustedBytecodeKeys: id → absolute key-file path. Loaded into a
    // TrustedKeyStore by the caller; a descriptor is only the *reference*.
    std::vector<TrustedKeyDescriptor> trusted_keys;
    TierConfig defaults;
    TierConfig maximums;
    CapacityConfig capacity;
    RecoveryConfig recovery;
};

// Parses the ALREADY schema-validated document (call validate_config_json
// first). Returns false with a stable operator-facing error when a field is
// accepted by the schema but cannot be mapped or fails a semantic gate
// (admin.mode, isolation.mode, capacity bounds, size/duration grammars).
bool parse_host_config(std::string_view json, ParsedHostConfig* out,
                       std::string* error);

struct ResolvedRecoveryPolicy {
    bool ok = false;
    WorkerRecoveryPolicy policy;
    std::string error;  // static text
};

// PR-09c: the coordinator's recovery-policy projection from the §9.5
// recovery block. A zero value means "Host default" — the strict
// GenerationPool::create_adopted validation rejects a zero field, so every
// field resolves to a non-zero value here (max_events=5, window_ms=60000,
// backoff_initial_ms=100, backoff_maximum_ms=10000, jitter=10%,
// stable_reset_ms=60000, replacements=1). The jitter text is decimal
// basis points ("1000" = 10%); the schema already validated the grammar,
// so an unparseable value at this layer is an error (never a silent
// default).
ResolvedRecoveryPolicy resolve_recovery_policy(const RecoveryConfig& config);

}  // namespace capsid::host

#endif
