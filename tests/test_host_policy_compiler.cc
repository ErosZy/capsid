// Frozen RED: host_policy_compiler (M1D + M2 static-pool boundary).
//
// Covers the Host/App effective-config compiler:
//   - a legal App request compiles to the effective intersection;
//   - modules, env, fs read, fetch, storage/stdio and worker/resource
//     overreach each reject;
//   - a fixed N/N static pool compiles within the Host worker ceiling, while
//     elastic/mismatched or over-ceiling pools reject;
//   - rule ids are stable and unique;
//   - effective.json records names, sources, key ids and opaque revisions
//     only — a canary value never appears;
//   - digests are deterministic.

#include "host/policy_compiler.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
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

    // 4b. fs deny compiles to a DENY rule inside the allow roots; deny
    // paths outside the roots, traversal and duplicates all reject.
    {
        capsid::host::AppRequest app = legal_app();
        app.fs_read = { "/srv/app" };
        app.fs_read_deny = { "/srv/app/private" };
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        const PolicyCompileResult result =
            compile_policy(default_host(), app, { resolved });
        require(result.ok, "legal fs deny rejected: " + result.error);
        require(result.effective.fs_read_deny.size() == 1 &&
                    result.effective.fs_read_deny[0] == "/srv/app/private",
                "fs deny path wrong");
        require(result.effective.fs_deny_rule_ids.size() == 1 &&
                    result.effective.fs_deny_rule_ids[0] != 0,
                "fs deny rule id missing");
        require(result.effective.effective_json.find("\"fsDeny\"") !=
                    std::string::npos &&
                    result.effective.effective_json.find(
                        "/srv/app/private") != std::string::npos,
                "effective.json lost the fs deny rule");

        capsid::host::RuntimePolicy runtime_policy;
        std::string build_error;
        require(capsid::host::build_runtime_policy(
                    result.effective,
                    { { "APP_TOKEN", "value-not-persisted" } },
                    &runtime_policy, &build_error),
                "fs deny descriptor build failed: " + build_error);
        bool found_deny = false;
        for (const capsid_permission_rule& rule : runtime_policy.rules) {
            if (rule.permission == CAPSID_PERMISSION_READ &&
                rule.action == CAPSID_PERMISSION_DENY &&
                rule.resource == std::string("/srv/app/private")) {
                found_deny = true;
            }
        }
        require(found_deny, "fs deny did not become a DENY rule");

        capsid::host::AppRequest outside = legal_app();
        outside.fs_read = { "/srv/app" };
        outside.fs_read_deny = { "/etc/passwd" };
        require(!compile_policy(default_host(), outside, {}).ok,
                "fs deny outside roots accepted");
        capsid::host::AppRequest traversal = legal_app();
        traversal.fs_read = { "/srv/app" };
        traversal.fs_read_deny = { "/srv/app/../../etc" };
        require(!compile_policy(default_host(), traversal, {}).ok,
                "fs deny traversal accepted");
        capsid::host::AppRequest duplicate = legal_app();
        duplicate.fs_read = { "/srv/app" };
        duplicate.fs_read_deny = { "/srv/app/private", "/srv/app/private" };
        require(!compile_policy(default_host(), duplicate, {}).ok,
                "duplicate fs deny accepted");
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
        capsid::host::AppRequest oversized_window = legal_app();
        oversized_window.requests_per_worker =
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max()) + 1;
        oversized_window.env.clear();
        capsid::host::HostPolicy unlimited_request_host = default_host();
        unlimited_request_host.max_requests_per_worker = 0;
        const PolicyCompileResult oversized_result =
            compile_policy(unlimited_request_host, oversized_window, {});
        require(!oversized_result.ok &&
                    oversized_result.error.find("worker") != std::string::npos,
                "request window above the Runtime limit accepted");
    }

    // 7b. E-1 admission queue (pool.queue*, §10.3): the App queue must not
    // exceed the Host maximums; a Host maximum of 0 leaves the App free.
    // The effective config carries the compiled queue into the data plane.
    {
        capsid::host::AppRequest queued = legal_app();
        queued.queue_requests = 16;
        queued.queue_header_bytes = 2U * 1024U * 1024U;
        queued.queue_timeout_ms = 5000;
        // legal_app() requests one secret; the resolved set must match.
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        {
            const PolicyCompileResult result =
                compile_policy(default_host(), queued, { resolved });
            require(result.ok, "legal queue rejected: " + result.error);
            require(result.effective.queue_requests == 16 &&
                        result.effective.queue_header_bytes ==
                            2U * 1024U * 1024U &&
                        result.effective.queue_timeout_ms == 5000,
                    "effective config lost the queue fields");
            require(result.effective.effective_json.find(
                        "\"queueRequests\":16") != std::string::npos &&
                        result.effective.effective_json.find(
                            "\"queueHeaderBytes\":2097152") !=
                            std::string::npos &&
                        result.effective.effective_json.find(
                            "\"queueTimeoutMs\":5000") != std::string::npos,
                    "effective.json lost the queue fields");
        }

        capsid::host::HostPolicy capped = default_host();
        capped.max_queue_requests = 8;
        capsid::host::AppRequest deep = queued;
        deep.queue_requests = 16;
        const PolicyCompileResult deep_result =
            compile_policy(capped, deep, { resolved });
        require(!deep_result.ok &&
                    deep_result.error.find("queue") != std::string::npos,
                "queue depth above the Host maximum accepted");

        capsid::host::HostPolicy bytes_capped = default_host();
        bytes_capped.max_queue_header_bytes = 1U * 1024U * 1024U;
        const PolicyCompileResult bytes_result =
            compile_policy(bytes_capped, queued, { resolved });
        require(!bytes_result.ok &&
                    bytes_result.error.find("queue") != std::string::npos,
                "queue header bytes above the Host maximum accepted");

        capsid::host::HostPolicy timeout_capped = default_host();
        timeout_capped.max_queue_timeout_ms = 1000;
        const PolicyCompileResult timeout_result =
            compile_policy(timeout_capped, queued, { resolved });
        require(!timeout_result.ok &&
                    timeout_result.error.find("queue") != std::string::npos,
                "queue timeout above the Host maximum accepted");
    }

    // 7c. E-2 SSE permit (request.*, §9.3): the App stream fields must not
    // exceed the Host maximums (cap-only, 0 = no ceiling); the effective
    // config carries the compiled values into the data plane. The 1/1
    // boundary rule is enforced at the shard, not in the compiler.
    {
        capsid::host::AppRequest streaming = legal_app();
        streaming.max_streaming_inflight_per_worker = 3;
        streaming.stream_idle_timeout_ms = 120000;
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        {
            const PolicyCompileResult result =
                compile_policy(default_host(), streaming, { resolved });
            require(result.ok, "legal SSE config rejected: " + result.error);
            require(result.effective.max_streaming_inflight_per_worker == 3 &&
                        result.effective.stream_idle_timeout_ms == 120000,
                    "effective config lost the SSE permit fields");
            require(result.effective.effective_json.find(
                        "\"maxStreamingInflightPerWorker\":3") !=
                        std::string::npos &&
                        result.effective.effective_json.find(
                            "\"streamIdleTimeoutMs\":120000") !=
                            std::string::npos,
                    "effective.json lost the SSE permit fields");
        }

        capsid::host::HostPolicy slot_capped = default_host();
        slot_capped.max_streaming_inflight_per_worker = 2;
        capsid::host::AppRequest deep = streaming;
        deep.max_streaming_inflight_per_worker = 4;
        const PolicyCompileResult slot_result =
            compile_policy(slot_capped, deep, { resolved });
        require(!slot_result.ok &&
                    slot_result.error.find("streaming") != std::string::npos,
                "streaming permit above the Host maximum accepted");

        capsid::host::HostPolicy idle_capped = default_host();
        idle_capped.max_stream_idle_timeout_ms = 60000;
        capsid::host::AppRequest long_idle = streaming;
        long_idle.stream_idle_timeout_ms = 300000;
        const PolicyCompileResult idle_result =
            compile_policy(idle_capped, long_idle, { resolved });
        require(!idle_result.ok &&
                    idle_result.error.find("stream") != std::string::npos,
                "stream idle timeout above the Host maximum accepted");
    }

    // 7d. E-3 slow-client write deadline (request.writeTimeoutMs, §9.2):
    // cap-only intersection like the other request maximums; the effective
    // config carries the compiled value into the data plane.
    {
        capsid::host::AppRequest writer = legal_app();
        writer.write_timeout_ms = 5000;
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        {
            const PolicyCompileResult result =
                compile_policy(default_host(), writer, { resolved });
            require(result.ok, "legal write-timeout config rejected: " +
                                   result.error);
            require(result.effective.write_timeout_ms == 5000,
                    "effective config lost the write deadline");
            require(result.effective.effective_json.find(
                        "\"writeTimeoutMs\":5000") != std::string::npos,
                    "effective.json lost the write deadline");
        }
        capsid::host::HostPolicy write_capped = default_host();
        write_capped.max_write_timeout_ms = 60000;
        capsid::host::AppRequest long_write = writer;
        long_write.write_timeout_ms = 300000;
        const PolicyCompileResult write_result =
            compile_policy(write_capped, long_write, { resolved });
        require(!write_result.ok &&
                    write_result.error.find("write timeout") !=
                        std::string::npos,
                "write deadline above the Host maximum accepted");
    }

    // 8. M2 static pools accept fixed N/N within the Host ceiling. The
    // compiler is a defense-in-depth entry point, so it must independently
    // reject minReady != maxWorkers even though the JSON schema already
    // rejects that shape. App isolation remains host-decided only.
    {
        capsid::host::HostPolicy host = default_host();
        host.max_workers = 4;
        // Host v1 has one worker-count ceiling (capacity.workersTotal), not
        // a second minReady ceiling. Keep this legacy field at its default
        // to prove it cannot silently cap an otherwise legal App pool.

        capsid::host::AppRequest fixed = legal_app();
        fixed.env.clear();
        fixed.workers = 3;
        fixed.min_ready = 3;
        const PolicyCompileResult compiled =
            compile_policy(host, fixed, {});
        require(compiled.ok,
                "fixed 3/3 static pool below Host ceiling rejected: " +
                    compiled.error);
        require(compiled.effective.workers == 3 &&
                    compiled.effective.min_ready == 3,
                "effective config lost the fixed pool size");
        require(compiled.effective.effective_json.find(
                    "\"workers\":3,\"minReady\":3") !=
                    std::string::npos,
                "effective.json lost the fixed pool contract or broke the "
                "M1D canonical layout");

        capsid::host::AppRequest elastic = fixed;
        elastic.min_ready = 2;
        const PolicyCompileResult elastic_result =
            compile_policy(host, elastic, {});
        require(!elastic_result.ok &&
                    elastic_result.error.find("pool") != std::string::npos,
                "minReady < maxWorkers escaped the static-pool gate");

        capsid::host::AppRequest over_ceiling = fixed;
        over_ceiling.workers = 5;
        over_ceiling.min_ready = 5;
        const PolicyCompileResult ceiling_result =
            compile_policy(host, over_ceiling, {});
        require(!ceiling_result.ok &&
                    ceiling_result.error.find("worker") != std::string::npos,
                "App pool above the Host worker ceiling was accepted");

        capsid::host::HostPolicy unbounded = host;
        unbounded.max_workers = 0;
        const PolicyCompileResult unbounded_result =
            compile_policy(unbounded, fixed, {});
        require(!unbounded_result.ok &&
                    unbounded_result.error.find("worker") != std::string::npos,
                "unbounded Host worker capacity was accepted");
        // Isolation is host-decided only: AppRequest has no isolation
        // field by construction.
    }

    // 8b. Any-port App vs finite Host ports rejects; Host maximum = 0
    // means unlimited; a suffix wildcard pattern rejects.
    {
        capsid::host::AppRequest app = legal_app();
        app.fetch = { { "api.example", {} } };  // any port
        require(!compile_policy(default_host(), app, {}).ok,
                "any-port request against finite Host ports accepted");

        capsid::host::HostPolicy unlimited = default_host();
        unlimited.max_requests_per_worker = 0;
        unlimited.max_worker_memory_bytes = 0;
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        const PolicyCompileResult unlimited_result =
            compile_policy(unlimited, legal_app(), { resolved });
        require(unlimited_result.ok,
                "finite app values rejected against unlimited Host");

        capsid::host::HostPolicy bad_pattern = default_host();
        bad_pattern.env_patterns = { "*SUFFIX" };  // not part of the grammar
        require(!compile_policy(bad_pattern, legal_app(), { resolved }).ok,
                "suffix wildcard pattern accepted");
    }

    // 8c. Inexact resolved secret sets reject: missing, duplicate, extra.
    {
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";
        // Missing: the request has one env entry, none resolved.
        require(!compile_policy(default_host(), legal_app(), {}).ok,
                "missing resolved secret accepted");
        // Extra: a resolved entry not requested.
        EffectiveEnvEntry extra;
        extra.name = "APP_OTHER";
        extra.from_secret = true;
        extra.secret_key_id = "other";
        extra.secret_revision = "file-v1:9:9:9:9:9";
        require(!compile_policy(default_host(), legal_app(), { resolved, extra }).ok,
                "extra resolved secret accepted");
        // Duplicate resolved names.
        require(!compile_policy(default_host(), legal_app(),
                                { resolved, resolved }).ok,
                "duplicate resolved secret accepted");
    }

    // 8d. Input permutation: the same semantics in a different order
    // produce the same effective JSON and digests.
    {
        EffectiveEnvEntry resolved;
        resolved.name = "APP_TOKEN";
        resolved.from_secret = true;
        resolved.secret_key_id = "api-token";
        resolved.secret_revision = "file-v1:1:2:3:4:5";

        capsid::host::AppRequest order_a = legal_app();
        capsid::host::AppRequest order_b = legal_app();
        // Reorder modules and env requests.
        order_b.modules = { "capsid:utils", "capsid:env" };
        capsid::host::AppRequest::EnvRequest second;
        second.name = "APP_REGION";
        second.literal = "eu";
        order_a.env.push_back(second);
        order_b.env.insert(order_b.env.begin(), second);
        EffectiveEnvEntry resolved_region;
        resolved_region.name = "APP_REGION";
        resolved_region.from_secret = false;
        resolved_region.literal = "eu";
        const PolicyCompileResult result_a = compile_policy(
            default_host(), order_a, { resolved, resolved_region });
        const PolicyCompileResult result_b = compile_policy(
            default_host(), order_b, { resolved, resolved_region });
        require(result_a.ok && result_b.ok, "permutation compile failed");
        require(result_a.effective.effective_json ==
                    result_b.effective.effective_json,
                "permutation changed effective.json");
        require(result_a.effective.effective_digest ==
                    result_b.effective.effective_digest,
                "permutation changed the effective digest");
        require(result_a.effective.rule_ids == result_b.effective.rule_ids,
                "permutation changed rule ids");
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
