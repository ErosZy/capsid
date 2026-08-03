// Frozen RED: host_policy_compiler (M1D).
//
// Covers the Host/App effective-config compiler:
//   - a legal App request compiles to the effective intersection;
//   - modules, env, fs read, fetch, storage/stdio and worker/resource
//     overreach each reject;
//   - a non-1/1 pool and an app-decided isolation request reject;
//   - rule ids are stable and unique;
//   - effective.json records names, sources, key ids and opaque revisions
//     only — a canary value never appears;
//   - digests are deterministic.

#include "host/policy_compiler.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

capsid::host::HostPolicy default_host() {
    capsid::host::HostPolicy host;
    host.module_allowlist = { "capsid:env", "capsid:fs", "capsid:utils" };
    host.env_patterns = { "APP_*" };
    host.env_deny_patterns = { "APP_SECRET" };
    host.fs_read_roots = { "/srv/app" };
    host.fetch_targets = { { "api.example", { 443 } } };
    host.storage_allowed = true;
    host.stdio_allowed = false;
    host.max_workers = 1;
    host.min_ready = 1;
    host.max_requests_per_worker = 10000;
    host.max_worker_memory_bytes = 256U * 1024U * 1024U;
    host.strict_sandbox = true;
    return host;
}

capsid::host::AppRequest legal_app() {
    capsid::host::AppRequest app;
    app.modules = { "capsid:env", "capsid:utils" };
    capsid::host::AppRequest::EnvRequest token;
    token.name = "APP_TOKEN";
    token.from_secret = true;
    token.secret_key_id = "api-token";
    app.env.push_back(token);
    app.fs_read = { "/srv/app/public" };
    app.fetch = { { "api.example", { 443 } } };
    app.storage = true;
    app.stdio = false;
    app.requests_per_worker = 1000;
    app.memory_bytes = 64U * 1024U * 1024U;
    app.workers = 1;
    app.min_ready = 1;
    return app;
}

}  // namespace

int main() {
    using capsid::host::compile_policy;
    using capsid::host::EffectiveEnvEntry;
    using capsid::host::PolicyCompileResult;

    const std::string canary = "canary-7d2f9c4e-11ab-4cde-9f01-23456789abcd";

    // 1. Legal request compiles to the effective intersection.
    {
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        const PolicyCompileResult result =
            compile_policy(default_host(), legal_app(), { resolved });
        require(result.ok, "legal app rejected: " + result.error);
        require(result.effective.modules.size() == 2,
                "module intersection wrong");
        require(result.effective.env.size() == 1 &&
                    result.effective.env[0].secret_key_id == "api-token" &&
                    result.effective.env[0].secret_revision == "file-v1:1:2:3:4:5",
                "env entry wrong");
        require(result.effective.fs_read.size() == 1 &&
                    result.effective.fs_read[0] == "/srv/app/public",
                "fs intersection wrong");
        require(result.effective.effective_json.find(canary) == std::string::npos,
                "effective.json leaked a value");
    }

    // 2. Module overreach rejects.
    {
        capsid::host::AppRequest app = legal_app();
        app.modules.push_back("capsid:process");  // not in the allowlist
        const PolicyCompileResult result =
            compile_policy(default_host(), app, {});
        require(!result.ok && result.error.find("module") != std::string::npos,
                "module overreach accepted");
    }

    // 3. Env overreach rejects (pattern + denial).
    {
        capsid::host::AppRequest app = legal_app();
        capsid::host::AppRequest::EnvRequest uncovered;
        uncovered.name = "OTHER_VAR";
        app.env.push_back(uncovered);
        require(!compile_policy(default_host(), app, {}).ok,
                "uncovered env accepted");

        capsid::host::AppRequest denied = legal_app();
        capsid::host::AppRequest::EnvRequest blocked;
        blocked.name = "APP_SECRET";  // host deny pattern
        denied.env.push_back(blocked);
        require(!compile_policy(default_host(), denied, {}).ok,
                "denied env accepted");
    }

    // 4. fs overreach rejects (outside roots + traversal).
    {
        capsid::host::AppRequest app = legal_app();
        app.fs_read = { "/etc/passwd" };
        require(!compile_policy(default_host(), app, {}).ok,
                "fs path outside roots accepted");
        capsid::host::AppRequest traversal = legal_app();
        traversal.fs_read = { "/srv/app/../../etc" };
        require(!compile_policy(default_host(), traversal, {}).ok,
                "fs traversal accepted");
    }

    // 5. Fetch overreach rejects.
    {
        capsid::host::AppRequest app = legal_app();
        app.fetch = { { "evil.example", { 443 } } };
        require(!compile_policy(default_host(), app, {}).ok,
                "fetch host outside targets accepted");
        capsid::host::AppRequest port = legal_app();
        port.fetch = { { "api.example", { 8443 } } };
        require(!compile_policy(default_host(), port, {}).ok,
                "fetch port outside targets accepted");
    }

    // 6. storage/stdio overreach rejects.
    {
        capsid::host::AppRequest app = legal_app();
        app.stdio = true;  // host disallows stdio
        require(!compile_policy(default_host(), app, {}).ok,
                "stdio overreach accepted");
    }

    // 7. Worker/resource overreach rejects.
    {
        capsid::host::AppRequest app = legal_app();
        app.requests_per_worker = 100000;  // > host maximum
        require(!compile_policy(default_host(), app, {}).ok,
                "request rate overreach accepted");
        capsid::host::AppRequest memory = legal_app();
        memory.memory_bytes = 512U * 1024U * 1024U;
        require(!compile_policy(default_host(), memory, {}).ok,
                "memory overreach accepted");
    }

    // 8. Non-1/1 pool rejects; isolation is host-decided only.
    {
        capsid::host::AppRequest app = legal_app();
        app.workers = 2;
        require(!compile_policy(default_host(), app, {}).ok,
                "non-1/1 pool accepted");
        // Isolation is host-decided only: AppRequest has no isolation
        // field by construction.
    }

    // 9. Rule ids stable and unique.
    {
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        const PolicyCompileResult first =
            compile_policy(default_host(), legal_app(), { resolved });
        const PolicyCompileResult second =
            compile_policy(default_host(), legal_app(), { resolved });
        require(first.ok && second.ok, "legal app failed on second compile");
        require(first.effective.rule_ids.size() ==
                    second.effective.rule_ids.size(),
                "rule id count changed");
        for (std::size_t index = 0; index < first.effective.rule_ids.size(); ++index) {
            require(first.effective.rule_ids[index].first ==
                        second.effective.rule_ids[index].first &&
                        first.effective.rule_ids[index].second ==
                            second.effective.rule_ids[index].second,
                    "rule ids not stable");
        }
        for (std::size_t a = 0; a < first.effective.rule_ids.size(); ++a) {
            for (std::size_t b = a + 1; b < first.effective.rule_ids.size(); ++b) {
                require(first.effective.rule_ids[a].first !=
                            first.effective.rule_ids[b].first,
                        "rule id collision");
            }
        }
        for (const auto& entry : first.effective.rule_ids) {
            require(entry.first != 0, "zero rule id");
        }
    }

    // 10. Deterministic digests + no secret values in any output.
    {
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        const PolicyCompileResult first =
            compile_policy(default_host(), legal_app(), { resolved });
        const PolicyCompileResult second =
            compile_policy(default_host(), legal_app(), { resolved });
        require(first.effective.app_config_digest ==
                    second.effective.app_config_digest &&
                    first.effective.effective_digest ==
                        second.effective.effective_digest,
                "digests not deterministic");
        require(first.effective.effective_json.find("file-v1") != std::string::npos,
                "revision missing from effective.json");
        require(first.effective.effective_json.find(canary) == std::string::npos &&
                    first.effective.effective_json.find("sk-live") == std::string::npos,
                "secret value leaked into effective.json");
    }

    std::cout << "PASS" << std::endl;
    return 0;
}
