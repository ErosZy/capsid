// Host/App effective-config compiler (M1D). See policy_compiler.h.

#include "host/policy_compiler.h"

#include <openssl/evp.h>

#include <algorithm>
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

bool env_pattern_matches(const std::string& pattern, const std::string& name) {
    if (pattern == "*") {
        return true;
    }
    if (pattern.size() >= 2 && pattern.back() == '*' &&
        pattern.find('*') == pattern.size() - 1) {
        return name.compare(0, pattern.size() - 1, pattern, 0,
                            pattern.size() - 1) == 0;
    }
    if (pattern.size() >= 2 && pattern[0] == '*' &&
        pattern.find('*') == 0) {
        const std::string suffix = pattern.substr(1);
        return name.size() >= suffix.size() &&
               name.compare(name.size() - suffix.size(), suffix.size(),
                            suffix) == 0;
    }
    return name == pattern;
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

    // Pool: M1D forces 1/1.
    if (app.workers != 1 || app.min_ready != 1 ||
        host.max_workers != 1 || host.min_ready != 1) {
        result.error = "M1D requires exactly one worker (pool 1/1)";
        return result;
    }
    result.effective.workers = 1;
    result.effective.min_ready = 1;
    result.effective.strict_sandbox = host.strict_sandbox;

    // Modules: app request must be inside the host allowlist.
    {
        std::set<std::string> allowed(host.module_allowlist.begin(),
                                      host.module_allowlist.end());
        std::set<std::string> chosen;
        for (const std::string& module : app.modules) {
            if (allowed.find(module) == allowed.end()) {
                result.error = "module not allowed by the Host: " + module;
                return result;
            }
            chosen.insert(module);
        }
        result.effective.modules.assign(chosen.begin(), chosen.end());
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

    // fs read: normalized app paths must stay within a host root.
    for (const std::string& raw : app.fs_read) {
        std::string normalized;
        if (!normalize_path(raw, &normalized)) {
            result.error = "invalid filesystem read path";
            return result;
        }
        bool within = false;
        for (const std::string& root : host.fs_read_roots) {
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
            if (target.ports.empty()) {
                covered = true;
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

    // storage/stdio: exact subset of the Host allowance.
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

    // worker/request/resource: app must not exceed host maximums.
    if (app.requests_per_worker > host.max_requests_per_worker) {
        result.error = "request rate exceeds the Host maximum";
        return result;
    }
    if (app.memory_bytes > host.max_worker_memory_bytes) {
        result.error = "worker memory exceeds the Host maximum";
        return result;
    }
    result.effective.requests_per_worker = app.requests_per_worker;
    result.effective.memory_bytes = app.memory_bytes;

    // Env entries with secret resolution: name + source + key id + opaque
    // revision only. The literal/secret values never enter effective.json.
    for (const AppRequest::EnvRequest& request : app.env) {
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

    // Rule ids: stable per normalized rule, unique.
    std::set<std::uint32_t> seen;
    for (const std::string& module : result.effective.modules) {
        const std::uint32_t id = rule_id("module:" + module);
        if (!seen.insert(id).second) {
            result.error = "rule id collision";
            return result;
        }
        result.effective.rule_ids.push_back({ id, "module:" + module });
    }
    for (const EffectiveEnvEntry& entry : result.effective.env) {
        const std::string label =
            std::string("env:") + entry.name + ":" +
            (entry.from_secret ? "secret:" + entry.secret_key_id : "literal");
        const std::uint32_t id = rule_id(label);
        if (!seen.insert(id).second) {
            result.error = "rule id collision";
            return result;
        }
        result.effective.rule_ids.push_back({ id, label });
        const_cast<EffectiveEnvEntry&>(entry).rule_id = id;
    }

    // Canonical effective.json (no secret values) + digests.
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
    json << "],\"fsRead\":[";
    for (std::size_t index = 0; index < result.effective.fs_read.size(); ++index) {
        if (index > 0) {
            json << ',';
        }
        json << '"' << json_escape(result.effective.fs_read[index]) << '"';
    }
    json << "],\"fetch\":[";
    for (std::size_t index = 0; index < result.effective.fetch.size(); ++index) {
        if (index > 0) {
            json << ',';
        }
        json << "{\"host\":\"" << json_escape(result.effective.fetch[index].host)
             << "\",\"ports\":[";
        const std::vector<std::uint16_t>& ports =
            result.effective.fetch[index].ports;
        for (std::size_t port_index = 0; port_index < ports.size(); ++port_index) {
            if (port_index > 0) {
                json << ',';
            }
            json << ports[port_index];
        }
        json << "]}";
    }
    json << "],\"storage\":" << (result.effective.storage ? "true" : "false")
         << ",\"stdio\":" << (result.effective.stdio ? "true" : "false")
         << ",\"requestsPerWorker\":" << result.effective.requests_per_worker
         << ",\"memoryBytes\":" << result.effective.memory_bytes
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
    app_stream << "storage:" << (app.storage ? 1 : 0)
               << ";stdio:" << (app.stdio ? 1 : 0)
               << ";rps:" << app.requests_per_worker
               << ";mem:" << app.memory_bytes;
    result.effective.app_config_digest = sha256_hex(app_stream.str());
    result.effective.effective_digest =
        sha256_hex(result.effective.effective_json);

    result.ok = true;
    return result;
}

}  // namespace capsid::host
