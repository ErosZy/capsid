#ifndef CAPSID_HOST_LOCAL_CAPSID_POLICY_H
#define CAPSID_HOST_LOCAL_CAPSID_POLICY_H

#include "host/policy_compiler.h"

#include <string>

namespace capsid::host {

// Local capsid.json permissions for the single-worker / static-pool data
// planes (v0.1.3). Unlike managed mode — where capsid.json is an App
// request intersected with the host.json authority — the local mode has no
// host.json: the document itself is the authority, and the compiled
// effective config feeds the worker's capability/egress policies verbatim.
//
// The document passes through the SAME frozen validation as managed mode
// (apiVersion, unknown fields, duplicates, env grammar, pool equality), so
// one grammar cannot diverge into two. What differs is the application
// boundary:
//   - permissions.* is applied: modules, env literals, fs read, fetch,
//     storage namespaces and stdio streams;
//   - env valueFrom is rejected — the managed secret store does not exist
//     on this path;
//   - worker.*, request.* and healthCheck are rejected as not applicable —
//     capacity, resources and the request window stay CLI-owned in these
//     modes, and an un-honored request fails loudly instead of being
//     silently ignored;
//   - pool is schema-required but inert here (the worker count is
//     CLI-decided).
struct LocalCapsidPolicy {
    // True when the file existed and its policy was applied. A missing
    // default ./capsid.json leaves the pre-v0.1.3 no-policy behavior
    // (every outbound Fetch denied, no capsid:* module).
    bool present = false;
    RuntimePolicy policy;
};

// Loads and compiles <path>. required=true fails on a missing file (the
// operator explicitly passed --capsid-json); required=false (the default
// ./capsid.json) treats a missing file as no policy. Every other failure —
// unreadable, oversized, schema-invalid, a rejected section, valueFrom —
// fails closed with *error set.
bool load_local_capsid_policy(const std::string& path,
                              bool required,
                              LocalCapsidPolicy* out,
                              std::string* error);

}  // namespace capsid::host

#endif
