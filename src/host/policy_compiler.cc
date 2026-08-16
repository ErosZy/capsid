// Host/App effective-config compiler (M1D). See policy_compiler.h.

#include "host/policy_compiler.h"

#include <openssl/evp.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace capsid::host {
namespace {

std::string sha256_hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &digest_size,
                   EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
        return "";
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned int i = 0; i < digest_size; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0f]);
    }
    return out;
}

// Stable non-zero rule id: FNV-1a 32-bit of the normalized rule label, with
// 0 mapped to 1.
std::uint32_t rule_id(const std::string& label) {
    std::uint32_t hash = 2166136261u;
    for (const char c : label) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619u;
    }
    return hash == 0 ? 1 : hash;
}

// Lexical path normalization: resolve "." and ".." without touching the
// filesystem; rejects escapes above the root.
bool normalize_path(const std::string& path, std::string* out) {
    if (path.empty() || path[0] != '/') {
        return false;
    }
    std::vector<std::string> components;
    std::size_t begin = 1;
    while (begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string component =
            path.substr(begin, end == std::string::npos ? std::string::npos
                                                        : end - begin);
        if (component == "..") {
            if (components.empty()) {
                return false;  // escapes the root
            }
            components.pop_back();
        } else if (!component.empty() && component != ".") {
            components.push_back(component);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    std::ostringstream out_stream;
    for (const std::string& component : components) {
        out_stream << '/' << component;
    }
    *out = out_stream.str().empty() ? "/" : out_stream.str();
    return true;
}

bool path_within(const std::string& path, const std::string& root) {
    if (path == root) {
        return true;
    }
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
           path[root.size()] == '/';
}

// Frozen env-pattern grammar: "*", an exact name, or a single trailing
// wildcard ("PREFIX*"). A leading wildcard ("*SUFFIX") is not part of the
// grammar and must be rejected, not interpreted.
bool env_pattern_matches(const std::string& pattern, const std::string& name) {
    if (pattern == "*") {
        return true;
    }
    const std::string::size_type star = pattern.find('*');
    if (star != std::string::npos) {
        if (star != pattern.size() - 1 || pattern.find('*', star + 1) !=
                                              std::string::npos) {
            return false;  // only a single trailing wildcard is valid
        }
        return name.compare(0, pattern.size() - 1, pattern, 0,
                            pattern.size() - 1) == 0;
    }
    return name == pattern;
}

bool valid_env_pattern(const std::string& pattern) {
    if (pattern == "*") {
        return true;
    }
    const std::string::size_type star = pattern.find('*');
    if (star == std::string::npos) {
        return !pattern.empty();
    }
    return star == pattern.size() - 1 &&
           pattern.find('*', star + 1) == std::string::npos;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out << buffer;
            } else {
                out << c;
            }
        }
    }
    return out.str();
}

}  // namespace

PolicyCompileResult compile_policy(
    const HostPolicy& host,
    const AppRequest& app,
    const std::vector<EffectiveEnvEntry>& resolved_secrets) {
    PolicyCompileResult result;

    // Pool: a fixed N/N static pool within the Host worker ceiling. The App
    // must state a positive pool with minReady == maxWorkers; elastic or
    // mismatched pools reject here even though the JSON schema already
    // forbids them (defense in depth). The Host has exactly one
    // worker-count ceiling (capacity.workersTotal -> host.max_workers), and
    // it must stay bounded: 0 means no worker capacity at all, not
    // unlimited like the other Host ceilings. Isolation is host-decided
    // only.
    if (app.workers == 0 || app.min_ready == 0) {
        result.error = "pool must be positive";
        return result;
    }
    if (app.min_ready != app.workers) {
        result.error = "static pool requires minReady == maxWorkers";
        return result;
    }
    if (host.max_workers == 0 || app.workers > host.max_workers) {
        result.error = "pool exceeds the Host worker ceiling";
        return result;
    }
    result.effective.workers = app.workers;
    result.effective.min_ready = app.min_ready;
    result.effective.strict_sandbox = host.strict_sandbox;

    // Modules: app request must be inside the host allowlist; duplicates
    // in the request reject.
    {
        std::set<std::string> allowed(host.module_allowlist.begin(),
                                      host.module_allowlist.end());
        std::set<std::string> requested;
        for (const std::string& module : app.modules) {
            if (!requested.insert(module).second) {
                result.error = "duplicate module request";
                return result;
            }
            if (allowed.find(module) == allowed.end()) {
                result.error = "module not allowed by the Host: " + module;
                return result;
            }
        }
        result.effective.modules.assign(requested.begin(), requested.end());
    }

    // Host env patterns must themselves follow the frozen grammar.
    for (const std::string& pattern : host.env_patterns) {
        if (!valid_env_pattern(pattern)) {
            result.error = "invalid Host env pattern";
            return result;
        }
    }

    std::set<std::string> requested_env_names;
    for (const AppRequest::EnvRequest& request : app.env) {
        if (!requested_env_names.insert(request.name).second) {
            result.error = "duplicate environment request";
            return result;
        }
    }
    if (app.env.size() != resolved_secrets.size()) {
        result.error = "resolved secret set does not match the request";
        return result;
    }
    {
        std::set<std::string> resolved_names;
        for (const EffectiveEnvEntry& entry : resolved_secrets) {
            if (!resolved_names.insert(entry.name).second) {
                result.error = "duplicate resolved secret";
                return result;
            }
            if (requested_env_names.find(entry.name) ==
                requested_env_names.end()) {
                result.error = "resolved secret not requested";
                return result;
            }
        }
        if (resolved_names != requested_env_names) {
            result.error = "resolved secret set is incomplete";
            return result;
        }
    }

    // Env: host pattern must cover each request; denials win.
    for (const AppRequest::EnvRequest& request : app.env) {
        bool covered = false;
        for (const std::string& pattern : host.env_patterns) {
            if (env_pattern_matches(pattern, request.name)) {
                covered = true;
                break;
            }
        }
        for (const std::string& pattern : host.env_deny_patterns) {
            if (env_pattern_matches(pattern, request.name)) {
                covered = false;
                break;
            }
        }
        if (!covered) {
            result.error = "environment variable not covered by the Host";
            return result;
        }
    }

    // fs read: normalized app paths must stay within normalized host
    // roots.
    std::vector<std::string> normalized_roots;
    for (const std::string& root : host.fs_read_roots) {
        std::string normalized_root;
        if (!normalize_path(root, &normalized_root)) {
            result.error = "invalid Host filesystem root";
            return result;
        }
        normalized_roots.push_back(normalized_root);
    }
    std::set<std::string> requested_fs;
    for (const std::string& raw : app.fs_read) {
        if (!requested_fs.insert(raw).second) {
            result.error = "duplicate filesystem request";
            return result;
        }
        std::string normalized;
        if (!normalize_path(raw, &normalized)) {
            result.error = "invalid filesystem read path";
            return result;
        }
        bool within = false;
        for (const std::string& root : normalized_roots) {
            if (path_within(normalized, root)) {
                within = true;
                break;
            }
        }
        if (!within) {
            result.error = "filesystem path outside the Host roots";
            return result;
        }
        result.effective.fs_read.push_back(normalized);
    }

    // Fetch: app host:port must be covered by a host target.
    for (const FetchTarget& target : app.fetch) {
        bool covered = false;
        for (const FetchTarget& allowed : host.fetch_targets) {
            if (allowed.host != target.host) {
                continue;
            }
            if (allowed.ports.empty()) {
                covered = true;
                break;
            }
            // The App's empty port list means ANY port; a Host with a
            // finite port list cannot cover that.
            if (target.ports.empty()) {
                break;
            }
            bool all_covered = true;
            for (const std::uint16_t port : target.ports) {
                if (std::find(allowed.ports.begin(), allowed.ports.end(),
                              port) == allowed.ports.end()) {
                    all_covered = false;
                    break;
                }
            }
            if (all_covered) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            result.error = "fetch target not covered by the Host";
            return result;
        }
        result.effective.fetch.push_back(target);
    }

    // storage/stdio: exact subset of the Host allowance. The Host's
    // precise namespace/stream sets are the authoritative intersection:
    // every App-requested resource must be contained in the Host set, and
    // the coarse bool flags can never widen a request beyond the sets (a
    // Host with storage_allowed but an empty namespace set allows none).
    if (app.storage && !host.storage_allowed) {
        result.error = "storage not allowed by the Host";
        return result;
    }
    if (app.stdio && !host.stdio_allowed) {
        result.error = "stdio not allowed by the Host";
        return result;
    }
    result.effective.storage = app.storage;
    result.effective.stdio = app.stdio;
    if (app.storage) {
        std::set<std::string> requested_namespaces(app.storage_namespaces.begin(),
                                                   app.storage_namespaces.end());
        if (requested_namespaces.size() != app.storage_namespaces.size()) {
            result.error = "duplicate storage namespace";
            return result;
        }
        const std::set<std::string> allowed(host.storage_namespaces.begin(),
                                            host.storage_namespaces.end());
        for (const std::string& namespace_name : requested_namespaces) {
            if (allowed.find(namespace_name) == allowed.end()) {
                result.error = "storage namespace not allowed by the Host";
                return result;
            }
        }
        result.effective.storage_namespaces.assign(
            requested_namespaces.begin(), requested_namespaces.end());
    }
    if (app.stdio) {
        std::set<std::string> requested_streams(app.stdio_streams.begin(),
                                                app.stdio_streams.end());
        if (requested_streams.size() != app.stdio_streams.size()) {
            result.error = "duplicate stdio stream";
            return result;
        }
        const std::set<std::string> allowed(host.stdio_streams.begin(),
                                            host.stdio_streams.end());
        for (const std::string& stream : requested_streams) {
            if (allowed.find(stream) == allowed.end()) {
                result.error = "stdio stream not allowed by the Host";
                return result;
            }
        }
        result.effective.stdio_streams.assign(requested_streams.begin(),
                                              requested_streams.end());
    }

    // worker/request/resource: app must not exceed host maximums; a Host
    // maximum of 0 means unlimited. The memory permit is the largest stated
    // ceiling, so a sub-resource (jsHeap/processAddressSpace) can never
    // sneak past the Host per-worker budget while memoryMax stays small.
    if (host.max_requests_per_worker != 0 &&
        app.requests_per_worker > host.max_requests_per_worker) {
        result.error = "request rate exceeds the Host maximum";
        return result;
    }
    if (app.requests_per_worker >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        result.error = "request rate exceeds the worker limit";
        return result;
    }
    result.effective.requests_per_worker = app.requests_per_worker;
    // E-1 admission-queue maximums (§10.3): the same intersection shape as
    // requests_per_worker — the App queue must not exceed the Host caps,
    // and a Host maximum of 0 means no cap (queueing stays App-decided).
    if (host.max_queue_requests != 0 &&
        app.queue_requests > host.max_queue_requests) {
        result.error = "queue depth exceeds the Host maximum";
        return result;
    }
    if (host.max_queue_header_bytes != 0 &&
        app.queue_header_bytes > host.max_queue_header_bytes) {
        result.error = "queue header bytes exceed the Host maximum";
        return result;
    }
    if (host.max_queue_timeout_ms != 0 &&
        app.queue_timeout_ms > host.max_queue_timeout_ms) {
        result.error = "queue timeout exceeds the Host maximum";
        return result;
    }
    result.effective.queue_requests = app.queue_requests;
    result.effective.queue_header_bytes = app.queue_header_bytes;
    result.effective.queue_timeout_ms = app.queue_timeout_ms;
    // E-2 SSE-permit maximums (§9.3): the same cap-only intersection as the
    // queue maximums — a Host maximum of 0 imposes no ceiling; the 1/1
    // boundary rule is enforced at the shard, not here.
    if (host.max_streaming_inflight_per_worker != 0 &&
        app.max_streaming_inflight_per_worker >
            host.max_streaming_inflight_per_worker) {
        result.error = "streaming permit exceeds the Host maximum";
        return result;
    }
    if (host.max_stream_idle_timeout_ms != 0 &&
        app.stream_idle_timeout_ms > host.max_stream_idle_timeout_ms) {
        result.error = "stream idle timeout exceeds the Host maximum";
        return result;
    }
    // E-3 slow-client write deadline (§9.2): cap-only, like the maximums
    // above; 0 imposes no ceiling.
    if (host.max_write_timeout_ms != 0 &&
        app.write_timeout_ms > host.max_write_timeout_ms) {
        result.error = "write timeout exceeds the Host maximum";
        return result;
    }
    result.effective.max_streaming_inflight_per_worker =
        app.max_streaming_inflight_per_worker;
    result.effective.stream_idle_timeout_ms = app.stream_idle_timeout_ms;
    result.effective.write_timeout_ms = app.write_timeout_ms;
    result.effective.js_heap_bytes = app.js_heap_bytes;
    result.effective.process_address_bytes = app.process_address_bytes;
    result.effective.file_descriptors = app.file_descriptors;
    result.effective.memory_bytes = std::max(
        app.memory_bytes,
        std::max(app.js_heap_bytes, app.process_address_bytes));
    if (host.max_worker_memory_bytes != 0 &&
        result.effective.memory_bytes > host.max_worker_memory_bytes) {
        result.error = "worker memory exceeds the Host maximum";
        return result;
    }
    // The process-memory ceiling — the most restrictive stated ceiling
    // among processAddressSpace/memoryMax — must never sit below the JS
    // heap: the Runtime's HELLO validation rejects
    // process_memory_limit < js_heap_limit, and the managed boundary
    // applies the same rule before staging so an invalid worker
    // configuration can never start.
    const std::uint64_t process_ceiling =
        std::min(result.effective.process_address_bytes,
                 result.effective.memory_bytes);
    if (result.effective.js_heap_bytes > 0 && process_ceiling > 0 &&
        process_ceiling < result.effective.js_heap_bytes) {
        result.error = "process memory ceiling below the JS heap limit";
        return result;
    }
#if defined(__APPLE__)
    // Darwin refuses the address-space limit by design (sandbox.cc's
    // __APPLE__ branch; the worker also rejects a nonzero
    // process_memory_limit at spawn). Reject the document at compile
    // time so a deployment with processAddressSpace fails closed here
    // instead of activating and then dying on its first worker spawn.
    if (result.effective.process_address_bytes > 0) {
        result.error = "process memory limit is unsupported on macOS; "
                       "remove processAddressSpace or set it to 0";
        return result;
    }
#endif
    // Canonical fetch ordering.
    std::sort(result.effective.fetch.begin(), result.effective.fetch.end(),
              [](const FetchTarget& a, const FetchTarget& b) {
                  return a.host < b.host;
              });
    // Canonical fs ordering.
    std::sort(result.effective.fs_read.begin(), result.effective.fs_read.end());

    // Canonical ordering: semantically identical inputs in any order must
    // produce the same effective config, JSON and digests.
    std::vector<AppRequest::EnvRequest> ordered_env = app.env;
    std::sort(ordered_env.begin(), ordered_env.end(),
              [](const AppRequest::EnvRequest& a,
                 const AppRequest::EnvRequest& b) {
                  return a.name < b.name;
              });

    // Env entries with secret resolution: name + source + key id + opaque
    // revision only. The literal/secret values never enter effective.json.
    for (const AppRequest::EnvRequest& request : ordered_env) {
        EffectiveEnvEntry entry;
        entry.name = request.name;
        entry.from_secret = request.from_secret;
        if (request.from_secret) {
            entry.secret_key_id = request.secret_key_id;
            bool found = false;
            for (const EffectiveEnvEntry& resolved : resolved_secrets) {
                if (resolved.name == request.name) {
                    entry.secret_revision = resolved.secret_revision;
                    found = true;
                    break;
                }
            }
            if (!found) {
                result.error = "secret not resolved for the requested name";
                return result;
            }
        } else {
            entry.literal = request.literal;
        }
        result.effective.env.push_back(entry);
    }

    // Rule ids: stable per normalized rule, unique, for every category.
    std::set<std::uint32_t> seen;
    const auto add_rule = [&](const std::string& label) -> bool {
        const std::uint32_t id = rule_id(label);
        if (!seen.insert(id).second) {
            result.error = "rule id collision";
            return false;
        }
        result.effective.rule_ids.push_back({ id, label });
        return true;
    };
    for (const std::string& module : result.effective.modules) {
        if (!add_rule("module:" + module)) {
            return result;
        }
    }
    for (EffectiveEnvEntry& entry : result.effective.env) {
        const std::string label =
            std::string("env:") + entry.name + ":" +
            (entry.from_secret ? "secret:" + entry.secret_key_id : "literal");
        const std::uint32_t id = rule_id(label);
        if (!seen.insert(id).second) {
            result.error = "rule id collision";
            return result;
        }
        entry.rule_id = id;
        result.effective.rule_ids.push_back({ id, label });
    }
    for (const std::string& path : result.effective.fs_read) {
        if (!add_rule("fs:" + path)) {
            return result;
        }
        result.effective.fs_rule_ids.push_back(rule_id("fs:" + path));
    }
    for (const FetchTarget& target : result.effective.fetch) {
        if (!add_rule("fetch:" + target.host)) {
            return result;
        }
        result.effective.fetch_rule_ids.push_back(
            rule_id("fetch:" + target.host));
    }
    for (const std::string& namespace_name : result.effective.storage_namespaces) {
        if (!add_rule("storage:" + namespace_name)) {
            return result;
        }
        result.effective.storage_rule_ids.push_back(
            rule_id("storage:" + namespace_name));
    }
    for (const std::string& stream : result.effective.stdio_streams) {
        if (!add_rule("stdio:" + stream)) {
            return result;
        }
        result.effective.stdio_rule_ids.push_back(rule_id("stdio:" + stream));
    }
    if (!add_rule("worker")) {
        return result;
    }
    if (!add_rule("isolation")) {
        return result;
    }

    // Canonical effective.json (no secret values) + digests. rule_ids are
    // sorted by id so the reverse lookup is order-independent.
    std::sort(result.effective.rule_ids.begin(),
              result.effective.rule_ids.end(),
              [](const std::pair<std::uint32_t, std::string>& a,
                 const std::pair<std::uint32_t, std::string>& b) {
                  return a.first < b.first;
              });
    std::ostringstream json;
    json << "{\"modules\":[";
    for (std::size_t index = 0; index < result.effective.modules.size(); ++index) {
        if (index > 0) {
            json << ',';
        }
        json << '"' << json_escape(result.effective.modules[index]) << '"';
    }
    json << "],\"env\":[";
    for (std::size_t index = 0; index < result.effective.env.size(); ++index) {
        const EffectiveEnvEntry& entry = result.effective.env[index];
        if (index > 0) {
            json << ',';
        }
        json << "{\"name\":\"" << json_escape(entry.name) << "\",\"source\":\"";
        if (entry.from_secret) {
            json << "secret\",\"secretKeyId\":\""
                 << json_escape(entry.secret_key_id)
                 << "\",\"revision\":\"" << json_escape(entry.secret_revision)
                 << '"';
        } else {
            json << "literal\"";
        }
        json << ",\"ruleId\":" << entry.rule_id << '}';
    }
    std::map<std::string, std::uint32_t> id_by_label;
    for (const std::pair<std::uint32_t, std::string>& pair :
         result.effective.rule_ids) {
        id_by_label[pair.second] = pair.first;
    }
    json << "],\"fsRead\":[";
    for (std::size_t index = 0; index < result.effective.fs_read.size(); ++index) {
        if (index > 0) {
            json << ',';
        }
        const std::map<std::string, std::uint32_t>::const_iterator id =
            id_by_label.find("fs:" + result.effective.fs_read[index]);
        if (id == id_by_label.end()) {
            result.error = "missing fs rule id";
            return result;
        }
        json << "{\"path\":\""
             << json_escape(result.effective.fs_read[index])
             << "\",\"ruleId\":" << id->second << '}';
    }
    json << "],\"fetch\":[";
    for (std::size_t index = 0; index < result.effective.fetch.size(); ++index) {
        if (index > 0) {
            json << ',';
        }
        const std::map<std::string, std::uint32_t>::const_iterator id =
            id_by_label.find("fetch:" + result.effective.fetch[index].host);
        if (id == id_by_label.end()) {
            result.error = "missing fetch rule id";
            return result;
        }
        json << "{\"host\":\""
             << json_escape(result.effective.fetch[index].host)
             << "\",\"ports\":[";
        const std::vector<std::uint16_t>& ports =
            result.effective.fetch[index].ports;
        for (std::size_t port_index = 0; port_index < ports.size(); ++port_index) {
            if (port_index > 0) {
                json << ',';
            }
            json << ports[port_index];
        }
        json << "],\"ruleId\":" << id->second << "}";
    }
    json << "],\"storage\":" << (result.effective.storage ? "true" : "false")
         << ",\"storageNamespaces\":[";
    for (std::size_t index = 0;
         index < result.effective.storage_namespaces.size(); ++index) {
        if (index > 0) {
            json << ',';
        }
        json << '"' << json_escape(result.effective.storage_namespaces[index])
             << '"';
    }
    json << "],\"stdio\":" << (result.effective.stdio ? "true" : "false")
         << ",\"stdioStreams\":[";
    for (std::size_t index = 0; index < result.effective.stdio_streams.size();
         ++index) {
        if (index > 0) {
            json << ',';
        }
        json << '"' << json_escape(result.effective.stdio_streams[index])
             << '"';
    }
    json << "],\"requestsPerWorker\":" << result.effective.requests_per_worker
         << ",\"queueRequests\":" << result.effective.queue_requests
         << ",\"queueHeaderBytes\":" << result.effective.queue_header_bytes
         << ",\"queueTimeoutMs\":" << result.effective.queue_timeout_ms
         << ",\"maxStreamingInflightPerWorker\":"
         << result.effective.max_streaming_inflight_per_worker
         << ",\"streamIdleTimeoutMs\":"
         << result.effective.stream_idle_timeout_ms
         << ",\"writeTimeoutMs\":"
         << result.effective.write_timeout_ms
         << ",\"memoryBytes\":" << result.effective.memory_bytes
         << ",\"jsHeapBytes\":" << result.effective.js_heap_bytes
         << ",\"processAddressBytes\":" << result.effective.process_address_bytes
         << ",\"fileDescriptors\":" << result.effective.file_descriptors
         << ",\"workers\":" << result.effective.workers
         << ",\"minReady\":" << result.effective.min_ready
         << ",\"strictSandbox\":" << (result.effective.strict_sandbox ? "true" : "false")
         << "}";
    result.effective.effective_json = json.str();

    // App digest: the normalized request the operator approved.
    std::ostringstream app_stream;
    for (const std::string& module : app.modules) {
        app_stream << "m:" << module << ';';
    }
    for (const AppRequest::EnvRequest& entry : app.env) {
        app_stream << "e:" << entry.name << ':'
                   << (entry.from_secret ? "s:" + entry.secret_key_id
                                         : "l:" + entry.literal)
                   << ';';
    }
    for (const std::string& path : app.fs_read) {
        app_stream << "f:" << path << ';';
    }
    for (const FetchTarget& target : app.fetch) {
        app_stream << "t:" << target.host << ':';
        for (const std::uint16_t port : target.ports) {
            app_stream << port << ',';
        }
        app_stream << ';';
    }
    app_stream << "storage:" << (app.storage ? 1 : 0) << ':';
    for (const std::string& namespace_name : app.storage_namespaces) {
        app_stream << namespace_name << ',';
    }
    app_stream << ";stdio:" << (app.stdio ? 1 : 0) << ':';
    for (const std::string& stream : app.stdio_streams) {
        app_stream << stream << ',';
    }
    app_stream << ";rps:" << app.requests_per_worker
               << ";qreq:" << app.queue_requests
               << ";qbytes:" << app.queue_header_bytes
               << ";qtimeout:" << app.queue_timeout_ms
               << ";streaming:" << app.max_streaming_inflight_per_worker
               << ";idle:" << app.stream_idle_timeout_ms
               << ";wtimeout:" << app.write_timeout_ms
               << ";mem:" << app.memory_bytes
               << ";heap:" << app.js_heap_bytes
               << ";addr:" << app.process_address_bytes
               << ";fds:" << app.file_descriptors;
    result.effective.app_config_digest = sha256_hex(app_stream.str());
    result.effective.effective_digest =
        sha256_hex(result.effective.effective_json);

    result.ok = true;
    return result;
}

void RuntimePolicy::apply(capsid_worker_config* config) const {
    config->capability_policy = &capability;
    // A null egress policy keeps the Runtime's deny-all default for empty
    // fetch sets; an empty-but-present policy must never be confused with
    // "no policy" (see the managed builder's net_policy note).
    config->egress_policy = has_egress ? &egress : nullptr;
}

bool build_runtime_policy(
    const EffectiveConfig& effective,
    const std::vector<std::pair<std::string, std::string>>& env_values,
    RuntimePolicy* out,
    std::string* error) {
    // Two-phase descriptor build: every owning vector is fully populated
    // before any pointer into it is taken. A c_str() taken after one
    // push_back and read after a later push_back dangles when the vector
    // reallocates (the double-env RED test caught exactly this
    // heap-use-after-free under ASan).
    out->module_names.clear();
    out->module_names.reserve(effective.modules.size());
    for (const std::string& module : effective.modules) {
        out->module_names.push_back(module);
    }
    out->rule_resources.clear();
    out->rule_resources.reserve(effective.env.size() +
                                effective.fs_read.size() +
                                effective.storage_namespaces.size() +
                                effective.stdio_streams.size());
    for (const EffectiveEnvEntry& entry : effective.env) {
        out->rule_resources.push_back(entry.name);
    }
    for (const std::string& path : effective.fs_read) {
        out->rule_resources.push_back(path);
    }
    for (const std::string& namespace_name : effective.storage_namespaces) {
        out->rule_resources.push_back(namespace_name);
    }
    for (const std::string& stream : effective.stdio_streams) {
        out->rule_resources.push_back(stream);
    }
    out->rules.clear();
    out->rules.reserve(out->rule_resources.size());
    std::size_t resource_index = 0;
    for (const EffectiveEnvEntry& entry : effective.env) {
        // The policy compiler assigns a stable non-zero rule id per entry;
        // the Runtime rejects a zero or duplicate id across the policy.
        if (entry.rule_id == 0) {
            *error = "missing env rule id";
            return false;
        }
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.permission = CAPSID_PERMISSION_ENV;
        rule.action = CAPSID_PERMISSION_ALLOW;
        rule.resource = out->rule_resources[resource_index].c_str();
        rule.rule_id = entry.rule_id;
        ++resource_index;
        out->rules.push_back(rule);
    }
    for (std::size_t index = 0; index < effective.fs_read.size(); ++index) {
        const std::uint32_t rule_id =
            index < effective.fs_rule_ids.size()
                ? effective.fs_rule_ids[index]
                : 0;
        if (rule_id == 0) {
            *error = "missing fs rule id";
            return false;
        }
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.permission = CAPSID_PERMISSION_READ;
        rule.action = CAPSID_PERMISSION_ALLOW;
        rule.resource = out->rule_resources[resource_index].c_str();
        rule.rule_id = rule_id;
        ++resource_index;
        out->rules.push_back(rule);
    }
    // Storage namespaces and stdio streams are exact-match resources: the
    // Runtime module gates on CAPSID_PERMISSION_STORAGE/STDIO with the
    // verbatim namespace/stream name. Each rule carries the compiler's
    // stable unique id, exactly like env/fs/fetch.
    for (std::size_t index = 0; index < effective.storage_namespaces.size();
         ++index) {
        const std::uint32_t rule_id =
            index < effective.storage_rule_ids.size()
                ? effective.storage_rule_ids[index]
                : 0;
        if (rule_id == 0) {
            *error = "missing storage rule id";
            return false;
        }
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.permission = CAPSID_PERMISSION_STORAGE;
        rule.action = CAPSID_PERMISSION_ALLOW;
        rule.resource = out->rule_resources[resource_index].c_str();
        rule.rule_id = rule_id;
        ++resource_index;
        out->rules.push_back(rule);
    }
    for (std::size_t index = 0; index < effective.stdio_streams.size();
         ++index) {
        const std::uint32_t rule_id =
            index < effective.stdio_rule_ids.size()
                ? effective.stdio_rule_ids[index]
                : 0;
        if (rule_id == 0) {
            *error = "missing stdio rule id";
            return false;
        }
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.permission = CAPSID_PERMISSION_STDIO;
        rule.action = CAPSID_PERMISSION_ALLOW;
        rule.resource = out->rule_resources[resource_index].c_str();
        rule.rule_id = rule_id;
        ++resource_index;
        out->rules.push_back(rule);
    }
    // Fetch is NOT a CAPSID_PERMISSION_NET rule: the Runtime rejects
    // resource-carrying NET rules and requires the network policy to live
    // in the egress policy (config.egress_policy). Targets and rules are
    // again populated in two phases. Explicit rule ids are left zero so
    // the ABI assigns index + 1: a host, expanded to several port rules,
    // must not repeat one explicit id (the Runtime rejects duplicates).
    out->egress_targets.clear();
    out->egress_rules.clear();
    out->has_egress = !effective.fetch.empty();
    std::size_t egress_rule_count = 0;
    for (const FetchTarget& target : effective.fetch) {
        egress_rule_count += target.ports.empty() ? 1 : target.ports.size();
    }
    out->egress_targets.reserve(effective.fetch.size());
    out->egress_rules.reserve(egress_rule_count);
    for (const FetchTarget& target : effective.fetch) {
        out->egress_targets.push_back(target.host);
    }
    for (std::size_t index = 0; index < effective.fetch.size(); ++index) {
        const FetchTarget& target = effective.fetch[index];
        if (target.ports.empty()) {
            capsid_egress_rule rule;
            capsid_egress_rule_init(&rule);
            rule.action = CAPSID_EGRESS_ALLOW;
            rule.target = out->egress_targets[index].c_str();
            rule.port_start = 0;  // any port
            rule.port_end = 0;
            rule.rule_id = 0;
            out->egress_rules.push_back(rule);
        } else {
            for (const std::uint16_t port : target.ports) {
                capsid_egress_rule rule;
                capsid_egress_rule_init(&rule);
                rule.action = CAPSID_EGRESS_ALLOW;
                rule.target = out->egress_targets[index].c_str();
                rule.port_start = port;
                rule.port_end = port;
                rule.rule_id = 0;
                out->egress_rules.push_back(rule);
            }
        }
    }
    capsid_egress_policy_init(&out->egress);
    out->egress.default_action = CAPSID_EGRESS_DENY;
    out->egress.rules =
        out->egress_rules.empty() ? nullptr : out->egress_rules.data();
    out->egress.rule_count =
        static_cast<std::uint32_t>(out->egress_rules.size());

    out->env_values = env_values;
    out->env_entries.clear();
    out->env_entries.reserve(out->env_values.size());
    // Entry pointers reference the policy's own copy, not the caller's
    // vector: on the local-capsid.json path the caller's env_values dies
    // when load_local_capsid_policy returns, long before the spawn.
    for (const std::pair<std::string, std::string>& entry : out->env_values) {
        capsid_env_entry env_entry;
        capsid_env_entry_init(&env_entry);
        env_entry.name = entry.first.c_str();
        env_entry.value = entry.second.c_str();
        out->env_entries.push_back(env_entry);
    }
    // The capability table wants const char* pointers, which must point
    // into this policy's own storage; module_names is stable here (the
    // two-phase build never touches it after this point).
    out->module_pointers.clear();
    out->module_pointers.reserve(out->module_names.size());
    for (const std::string& module : out->module_names) {
        out->module_pointers.push_back(module.c_str());
    }
    capsid_capability_policy_init(&out->capability);
    out->capability.allowed_modules =
        out->module_pointers.empty() ? nullptr : out->module_pointers.data();
    out->capability.allowed_module_count =
        static_cast<std::uint32_t>(out->module_names.size());
    out->capability.rules =
        out->rules.empty() ? nullptr : out->rules.data();
    out->capability.rule_count =
        static_cast<std::uint32_t>(out->rules.size());
    out->capability.env_entries =
        out->env_entries.empty() ? nullptr : out->env_entries.data();
    out->capability.env_entry_count =
        static_cast<std::uint32_t>(out->env_entries.size());
    // Without this the worker's egress check consults a default-constructed
    // (deny-all) net policy whenever a capability policy is present — which
    // is always the case in managed mode — so every fetch was rejected.
    // nullptr keeps the deny-all fail-closed default for empty fetch sets.
    out->capability.net_policy = out->has_egress ? &out->egress : nullptr;
    return true;
}

}  // namespace capsid::host
