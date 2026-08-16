// Local capsid.json permissions for the single-worker / static-pool data
// planes (v0.1.3). See local_capsid_policy.h for the application boundary;
// this file is the loader: read (1 MiB cap) -> frozen schema validation ->
// reject not-applicable sections -> parse -> reject valueFrom -> compile
// through the same policy compiler with a permissive host mirror -> build
// the runtime descriptors.

#include "host/local_capsid_policy.h"

#include "host/config.h"
#include "host/policy_compiler.h"
#include "win32_compat.h"

#include <jansson.h>

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace capsid::host {
namespace {

// The same per-file cap as the deployment-input safe-read boundary: a local
// capsid.json is an operator document, not a deployment artifact, but the
// budget is identical.
constexpr std::size_t kMaxLocalCapsidJsonBytes = 1024U * 1024U;

enum class ReadOutcome { kOk, kMissing, kFailed };

// Reads the file with the --source-bundle discipline (operator-owned local
// input, same trust level as the bundle it grants permissions to) plus the
// managed 1 MiB document cap. A missing file is distinguished from every
// other failure: the default ./capsid.json is allowed to be absent, an
// explicit --capsid-json is not.
ReadOutcome read_local_config_file(const std::string& path,
                                   std::vector<std::uint8_t>* out,
                                   std::string* error) {
    struct stat before = {};
    if (stat(path.c_str(), &before) != 0) {
        if (errno == ENOENT) {
            return ReadOutcome::kMissing;
        }
        *error = "cannot stat " + path;
        return ReadOutcome::kFailed;
    }
#if defined(_WIN32)
    const bool regular = (before.st_mode & _S_IFREG) != 0;
#else
    const bool regular = S_ISREG(before.st_mode);
#endif
    if (!regular) {
        *error = path + " is not a regular file";
        return ReadOutcome::kFailed;
    }
    if (static_cast<std::uint64_t>(before.st_size) >
        kMaxLocalCapsidJsonBytes) {
        *error = path + " exceeds the 1 MiB capsid.json cap";
        return ReadOutcome::kFailed;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "cannot open " + path;
        return ReadOutcome::kFailed;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        *error = "cannot size " + path;
        return ReadOutcome::kFailed;
    }
    if (static_cast<std::uint64_t>(size) > kMaxLocalCapsidJsonBytes) {
        *error = path + " exceeds the 1 MiB capsid.json cap";
        return ReadOutcome::kFailed;
    }
    input.seekg(0, std::ios::beg);
    out->resize(static_cast<std::size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char*>(out->data()),
                   static_cast<std::streamsize>(size));
        if (!input) {
            *error = "cannot read " + path;
            return ReadOutcome::kFailed;
        }
    }
    return ReadOutcome::kOk;
}

// In local mode the document is its own authority: the intersection host
// mirrors the App request exactly, so compile_policy passes every request
// through while still performing the full normalization, grammar and
// ordering pipeline (rule ids, digests, canonical effective config).
HostPolicy permissive_host(const AppRequest& app) {
    HostPolicy host;
    host.module_allowlist = app.modules;
    for (const AppRequest::EnvRequest& request : app.env) {
        // Exact-name patterns: the frozen env grammar already ran.
        host.env_patterns.push_back(request.name);
    }
    host.fs_read_roots = app.fs_read;
    host.fetch_targets = app.fetch;
    host.storage_allowed = app.storage;
    host.storage_namespaces = app.storage_namespaces;
    host.stdio_allowed = app.stdio;
    host.stdio_streams = app.stdio_streams;
    // The single worker-count ceiling: exactly the requested pool, so the
    // pool check passes (local mode never imposes a separate ceiling).
    host.max_workers = app.workers;
    return host;
}

}  // namespace

bool load_local_capsid_policy(const std::string& path,
                              bool required,
                              LocalCapsidPolicy* out,
                              std::string* error) {
    std::vector<std::uint8_t> bytes;
    const ReadOutcome outcome = read_local_config_file(path, &bytes, error);
    if (outcome == ReadOutcome::kMissing) {
        // A missing default file is the pre-v0.1.3 no-policy case; a
        // missing explicit --capsid-json is an operator error.
        if (!required) {
            out->present = false;
            out->policy = RuntimePolicy();
            return true;
        }
        *error = "cannot find " + path;
        return false;
    }
    if (outcome != ReadOutcome::kOk) {
        // The reader already set *error for every other failure.
        return false;
    }

    // 1. The frozen capsid/app-v1 schema boundary first: unknown fields,
    // duplicates, shapes, limits and the apiVersion contract.
    const std::string text(bytes.begin(), bytes.end());
    const ConfigValidationResult validated =
        validate_config_json(ConfigDocument::kApplication, text);
    if (!validated.ok) {
        *error = path + " rejected at " + validated.error.path + ": " +
                 validated.error.message;
        return false;
    }

    // 2. Sections this path cannot honor must fail loudly, not silently
    // skip: worker.* / request.* / healthCheck are CLI-owned in the
    // single-worker and static-pool modes. (pool is schema-required but
    // inert here — the worker count is CLI-decided.)
    {
        json_error_t parse_error;
        json_t* root = json_loadb(
            reinterpret_cast<const char*>(bytes.data()), bytes.size(),
            JSON_REJECT_DUPLICATES, &parse_error);
        if (root == nullptr || !json_is_object(root)) {
            if (root) {
                json_decref(root);
            }
            *error = path + ": invalid capsid.json";
            return false;
        }
        const char* sections[] = {"worker", "request", "healthCheck"};
        for (const char* section : sections) {
            if (json_object_get(root, section) != nullptr) {
                *error = path + ": \"" + section +
                         "\" is not applicable in local mode (capacity, "
                         "resources and the request window stay "
                         "CLI-owned; remove the section)";
                json_decref(root);
                return false;
            }
        }
        json_decref(root);
    }

    // 3. Map the validated document onto the App request through the same
    // parser managed mode uses.
    AppRequest app;
    std::string parse_error;
    if (!parse_app_request(bytes, &app, &parse_error)) {
        *error = path + ": " + parse_error;
        return false;
    }

    // 4. valueFrom has no store on this path: the worker's env comes only
    // from literal entries here.
    for (const AppRequest::EnvRequest& request : app.env) {
        if (request.from_secret) {
            *error = path + ": env valueFrom is unavailable in local mode "
                            "(no managed secret store)";
            return false;
        }
    }

    // 5. Compile through the authoritative compiler (normalization, rule
    // ids, digests, canonical effective config) with the permissive host.
    std::vector<EffectiveEnvEntry> resolved;
    resolved.reserve(app.env.size());
    for (const AppRequest::EnvRequest& request : app.env) {
        EffectiveEnvEntry entry;
        entry.name = request.name;
        entry.literal = request.literal;
        resolved.push_back(entry);
    }
    const PolicyCompileResult compiled =
        compile_policy(permissive_host(app), app, resolved);
    if (!compiled.ok) {
        *error = path + ": " + compiled.error;
        return false;
    }

    // 6. Runtime descriptors (capability + egress structs) for the spawn.
    std::vector<std::pair<std::string, std::string>> env_values;
    env_values.reserve(compiled.effective.env.size());
    for (const EffectiveEnvEntry& entry : compiled.effective.env) {
        env_values.emplace_back(entry.name, entry.literal);
    }
    out->policy = RuntimePolicy();
    if (!build_runtime_policy(compiled.effective, env_values, &out->policy,
                              error)) {
        *error = path + ": " + *error;
        return false;
    }
    out->present = true;
    return true;
}

}  // namespace capsid::host
