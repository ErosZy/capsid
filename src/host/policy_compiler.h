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
    bool stdio_allowed = false;
    std::uint32_t max_workers = 1;
    std::uint32_t min_ready = 1;
    std::uint64_t max_requests_per_worker = 0;  // 0 = unlimited
    std::uint64_t max_worker_memory_bytes = 0;
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
    bool stdio = false;
    std::uint64_t requests_per_worker = 0;
    std::uint64_t memory_bytes = 0;
    std::uint32_t workers = 1;  // M1D: must be 1
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
    std::vector<FetchTarget> fetch;
    bool storage = false;
    bool stdio = false;
    std::uint64_t requests_per_worker = 0;
    std::uint64_t memory_bytes = 0;
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
