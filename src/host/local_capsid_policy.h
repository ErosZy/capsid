#ifndef CAPSID_HOST_LOCAL_CAPSID_POLICY_H
#define CAPSID_HOST_LOCAL_CAPSID_POLICY_H

#include "host/binding_compile.h"
#include "host/policy_compiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace capsid::host {

// Local capsid.json permissions for the single-worker / static-pool data
// planes (v0.1.3). Unlike managed mode — where capsid.json is an App
// request intersected with the host.json authority — the local mode has no
// host.json: the document itself is the authority, and the compiled
// effective config feeds the worker's capability/egress policies verbatim.
//
// The document passes through the SAME frozen app-v1/app-v2 validation as
// managed mode (apiVersion, unknown fields, duplicates, env grammar, pool
// equality), so
// one grammar cannot diverge into two. What differs is the application
// boundary:
//   - permissions.* is applied: modules, env literals, fs read (including
//     fs.read.deny, which wins over allow), fetch, storage namespaces and
//     stdio streams;
//   - env valueFrom resolves against an explicit --secrets-root directory
//     (the managed secret layout: one regular file per key id); without
//     one it is rejected — there is no implicit secret store on this path;
//   - worker.* / request.* / pool.queue* / healthCheck / entry are parsed
//     and applied locally (v0.2.x): entry names the bundle file inside the
//     capsid.json directory when --source-bundle is absent, the resource
//     and request-window fields map onto the same worker/spawn knobs as
//     the CLI (an explicit CLI flag wins over the document), and an armed
//     healthCheck gates the READY record on one startup probe;
//   - pool is schema-required but its worker count is inert here (the
//     worker count is CLI-decided).
struct LocalCapsidPolicy {
    // The non-permission fields the single-worker / static-pool data
    // planes can honor, filled from the same parse managed mode uses.
    // Zero values keep the shard/worker defaults (the schema convention);
    // the queue fields use has_* because 0 means "queueing disabled".
    struct RuntimeSettings {
        bool has_entry = false;
        std::string entry;  // bundle file name, relative to the capsid.json directory
        bool has_request_timeout_ms = false;
        std::uint64_t request_timeout_ms = 0;
        bool has_max_inflight = false;
        std::uint64_t max_inflight_per_worker = 0;
        bool has_max_streaming_inflight = false;
        std::uint64_t max_streaming_inflight_per_worker = 0;
        bool has_stream_idle_timeout_ms = false;
        std::uint64_t stream_idle_timeout_ms = 0;
        bool has_write_timeout_ms = false;
        std::uint64_t write_timeout_ms = 0;
        bool has_queue_requests = false;
        std::uint64_t queue_requests = 0;
        bool has_queue_header_bytes = false;
        std::uint64_t queue_header_bytes = 0;
        bool has_queue_timeout_ms = false;
        std::uint64_t queue_timeout_ms = 0;
        // worker.* resources; 0 = unset (worker defaults).
        std::uint64_t js_heap_bytes = 0;
        std::uint64_t process_address_bytes = 0;
        std::uint64_t memory_bytes = 0;      // memoryMax: budget accounting
        std::uint64_t file_descriptors = 0;
        HealthCheckConfig health_check;      // configured=false = no probe
    };

    // True when the file existed and its policy was applied. A missing
    // default ./capsid.json leaves the pre-v0.1.3 no-policy behavior
    // (every outbound Fetch denied, no capsid:* module).
    bool present = false;
    RuntimePolicy policy;
    RuntimeSettings settings;
    // Immutable Manifest ∩ App results. Empty means the worker stays on
    // the baseline single Runtime path; no Binding Runtime is loaded.
    std::vector<EffectiveBinding> bindings;
};

// Loads and compiles <path>. required=true fails on a missing file (the
// operator explicitly passed --capsid-json); required=false (the default
// ./capsid.json) treats a missing file as no policy. secrets_root names the
// explicit local secret store for env valueFrom (empty = valueFrom is
// rejected; the file layout is one regular file per key id, the managed
// contract). Every other failure — unreadable, oversized, schema-invalid,
// a rejected section, a valueFrom without a store — fails closed with
// *error set.
bool load_local_capsid_policy(const std::string& path,
                              bool required,
                              const BindingRegistrySnapshot* binding_registry,
                              const std::string& secrets_root,
                              LocalCapsidPolicy* out,
                              std::string* error);

}  // namespace capsid::host

#endif
