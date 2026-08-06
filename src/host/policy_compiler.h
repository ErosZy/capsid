#ifndef CAPSID_HOST_POLICY_COMPILER_H
#define CAPSID_HOST_POLICY_COMPILER_H

#include <cstdint>
#include <string>
#include <vector>

namespace capsid::host {

// Host/App effective-config compiler (M1D). The App request is the
// intersection of what the App asks for and what the Host allows; any
// overreach rejects the deployment. Rule IDs are stable hashes of the
// normalized rule (deterministic, non-zero, unique). effective.json
// records env names, literal/secret sources, secret key IDs and opaque
// revisions — never secret values.

struct FetchTarget {
    std::string host;
    std::vector<std::uint16_t> ports;  // empty = any port on the host
};

struct HostPolicy {
    std::vector<std::string> module_allowlist;
    // Env patterns in the frozen Host grammar: "NAME", "PREFIX*", "*SUFFIX"
    // or "*" (any). Longest-prefix matching wins for denials.
    std::vector<std::string> env_patterns;
    std::vector<std::string> env_deny_patterns;
    std::vector<std::string> fs_read_roots;  // normalized, no trailing '/'
    std::vector<FetchTarget> fetch_targets;
    bool storage_allowed = false;
    // Exact resource allow sets. Every App-requested namespace/stream must
    // be contained in the Host set; the bool flags are only the coarse
    // gate and can never widen a request beyond the sets. A Host that
    // allows storage with an empty set allows no namespace at all.
    std::vector<std::string> storage_namespaces;
    std::vector<std::string> stdio_streams;
    bool stdio_allowed = false;
    // The single worker-count ceiling (capacity.workersTotal). Unlike the
    // other Host maximums, 0 is NOT unlimited: a bounded pool is required,
    // so 0 means no worker capacity at all and rejects every App.
    std::uint32_t max_workers = 1;
    std::uint32_t min_ready = 1;  // legacy; not a ceiling (see compile_policy)
    std::uint64_t max_requests_per_worker = 0;  // 0 = unlimited
    std::uint64_t max_worker_memory_bytes = 0;
    // Host-side admission-queue maximums (host.json maximums.pool.*, E-1
    // §10.3). 0 = not set: unlike the worker-count ceiling, queueing is
    // App-decided and the Host only caps it, so 0 is the natural
    // "no cap" sentinel.
    std::uint64_t max_queue_requests = 0;      // 0 = no depth cap
    std::uint64_t max_queue_header_bytes = 0;  // 0 = no header-bytes cap
    std::uint64_t max_queue_timeout_ms = 0;    // 0 = no deadline cap
    // Host-side SSE-permit maximums (host.json maximums.request.*, E-2
    // §9.3). Same cap-only semantics as the queue maximums: 0 = the Host
    // imposes no ceiling; the 1/1 boundary rule is enforced at the shard.
    std::uint64_t max_streaming_inflight_per_worker = 0;
    std::uint64_t max_stream_idle_timeout_ms = 0;
    bool strict_sandbox = true;  // isolation is host-decided only
};

struct AppRequest {
    std::vector<std::string> modules;
    struct EnvRequest {
        std::string name;
        bool from_secret = false;  // valueFrom.secretKeyId
        std::string secret_key_id;
        std::string literal;  // when !from_secret
    };
    std::vector<EnvRequest> env;
    std::vector<std::string> fs_read;  // requested allow paths
    std::vector<FetchTarget> fetch;
    bool storage = false;
    // Exact storage namespace allow list (permissions.storage.namespaces).
    std::vector<std::string> storage_namespaces;
    bool stdio = false;
    // Exact stdio stream allow list (permissions.stdio); each entry is one
    // of "stdin"/"stdout"/"stderr".
    std::vector<std::string> stdio_streams;
    std::uint64_t requests_per_worker = 0;
    // pool.queue* (E-1 §10.3): the bounded per-shard admission queue.
    // 0 = queueing disabled (inflight-full rejects with 429 directly).
    std::uint64_t queue_requests = 0;       // queue depth; 0 = disabled
    std::uint64_t queue_header_bytes = 0;   // 0 = unbounded header bytes
    std::uint64_t queue_timeout_ms = 0;     // 0 = no queue deadline
    // request.* SSE permit (E-2 §9.3). 0 = unset: the shard keeps its
    // defaults (2 slots, 60s idle). An explicit non-zero value overrides;
    // the 1/1 boundary is validated at the shard.
    std::uint64_t max_streaming_inflight_per_worker = 0;  // 0 = unset
    std::uint64_t stream_idle_timeout_ms = 0;             // 0 = unset
    // worker.* resource fields. memory_bytes is worker.memoryMax (the
    // process memory ceiling); the three sub-resources keep their own
    // slots so none of them impersonates another at the Runtime boundary.
    std::uint64_t memory_bytes = 0;          // worker.memoryMax
    std::uint64_t js_heap_bytes = 0;         // worker.jsHeap
    std::uint64_t process_address_bytes = 0; // worker.processAddressSpace
    std::uint64_t file_descriptors = 0;      // worker.fileDescriptors, 0 = unset
    std::uint32_t workers = 1;  // fixed static pool, min_ready == workers
    std::uint32_t min_ready = 1;
};

struct EffectiveEnvEntry {
    std::string name;
    bool from_secret = false;
    std::string secret_key_id;   // opaque reference only
    std::string secret_revision; // opaque revision, never the value
    std::string literal;         // literal value when !from_secret
    std::uint32_t rule_id = 0;
};

struct EffectiveConfig {
    std::vector<std::string> modules;          // sorted intersection
    std::vector<EffectiveEnvEntry> env;        // in app request order
    std::vector<std::string> fs_read;          // normalized allow list
    // Parallel stable rule ids from the policy compiler. The Runtime
    // capability policy requires every rule id to be non-zero and unique
    // within the policy, so these travel with the effective config instead
    // of being re-derived (or hard-coded) at descriptor build time.
    std::vector<std::uint32_t> fs_rule_ids;
    std::vector<FetchTarget> fetch;
    std::vector<std::uint32_t> fetch_rule_ids;
    bool storage = false;
    std::vector<std::string> storage_namespaces;  // canonical order
    std::vector<std::uint32_t> storage_rule_ids;  // parallel to namespaces
    bool stdio = false;
    std::vector<std::string> stdio_streams;  // canonical order
    std::vector<std::uint32_t> stdio_rule_ids;  // parallel to streams
    std::uint64_t requests_per_worker = 0;
    std::uint64_t queue_requests = 0;       // pool.queueRequests
    std::uint64_t queue_header_bytes = 0;   // pool.queueHeaderBytes
    std::uint64_t queue_timeout_ms = 0;     // pool.queueTimeout
    std::uint64_t max_streaming_inflight_per_worker = 0;  // request.*, 0 = unset
    std::uint64_t stream_idle_timeout_ms = 0;             // request.*, 0 = unset
    // The effective process-memory permit: the largest stated ceiling
    // (memoryMax, jsHeap, processAddressSpace) for Host-budget accounting.
    std::uint64_t memory_bytes = 0;
    std::uint64_t js_heap_bytes = 0;          // -> Runtime js_heap_limit
    std::uint64_t process_address_bytes = 0;  // -> Runtime process_memory_limit
    std::uint64_t file_descriptors = 0;       // -> Runtime resource_limits
    std::uint32_t workers = 1;
    std::uint32_t min_ready = 1;
    bool strict_sandbox = true;

    // Canonical single-line effective.json (no secret values).
    std::string effective_json;
    // sha256 of the normalized App request (what the operator approved).
    std::string app_config_digest;
    // sha256 of the effective policy + resource section.
    std::string effective_digest;
    // Reverse lookup: rule id -> human-readable rule label.
    std::vector<std::pair<std::uint32_t, std::string>> rule_ids;
};

struct PolicyCompileResult {
    bool ok = false;
    // Static diagnostic; never contains secret values.
    std::string error;
    EffectiveConfig effective;
};

PolicyCompileResult compile_policy(
    const HostPolicy& host,
    const AppRequest& app,
    const std::vector<EffectiveEnvEntry>& resolved_secrets);

}  // namespace capsid::host

#endif
