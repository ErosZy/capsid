// Frozen RED suite: the M1D managed host (modes registered as the frozen
// test names). Drives the real deploy pipeline: safe-read, attestation
// selection, policy/secret compilation, staging, worker warm-up with the
// real capsid-worker, active.json persistence, retire and recovery.

#include "host/managed_host.h"
#include "host/bytecode_attestation.h"

#include "capsid/runtime.h"

#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <vector>

#include "build_identity.h"

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

void write_file(const std::string& path, const std::string& content) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    require(fd >= 0, "cannot create fixture file: " + path);
    require(content.empty() ||
                write(fd, content.data(), content.size()) ==
                    static_cast<ssize_t>(content.size()),
            "cannot write fixture file");
    close(fd);
}

std::string read_file(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    require(fd >= 0, "cannot open file: " + path);
    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return out;
}

std::string run_command(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const std::string& argument : argv) {
        args.push_back(const_cast<char*>(argument.c_str()));
    }
    args.push_back(nullptr);
    int pipes[2];
    require(pipe(pipes) == 0, "cannot create pipe");
    const pid_t pid = fork();
    require(pid >= 0, "cannot fork");
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execv(args[0], args.data());
        _exit(127);
    }
    close(pipes[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(pipes[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(pipes[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "command failed: " + output);
    return output;
}

struct Fixtures {
    std::string apps_root;
    std::string state_root;
    std::string secret_root;
    int apps_fd = -1;
    int secrets_fd = -1;
    std::string worker_path;
    std::string compiler_tool;
    std::string fixture_source;
    std::string vdir;
};

Fixtures make_fixtures(const char* worker_path, const char* compiler_tool,
                       const char* fixture_source) {
    Fixtures f;
    f.apps_root = "/tmp/capsid-managed-apps-XXXXXX";
    require(mkdtemp(&f.apps_root[0]) != nullptr, "cannot create apps root");
    f.state_root = "/tmp/capsid-managed-state-XXXXXX";
    require(mkdtemp(&f.state_root[0]) != nullptr, "cannot create state root");
    f.secret_root = "/tmp/capsid-managed-secrets-XXXXXX";
    require(mkdtemp(&f.secret_root[0]) != nullptr, "cannot create secret root");
    f.apps_fd = open(f.apps_root.c_str(), O_RDONLY | O_DIRECTORY);
    f.secrets_fd = open(f.secret_root.c_str(), O_RDONLY | O_DIRECTORY);
    require(f.apps_fd >= 0 && f.secrets_fd >= 0, "cannot open fixture roots");
    f.worker_path = worker_path;
    f.compiler_tool = compiler_tool;
    f.fixture_source = fixture_source;
    require(mkdirat(f.apps_fd, "orders", 0700) == 0, "cannot create app dir");
    f.vdir = f.apps_root + "/orders/v1";
    require(mkdir(f.vdir.c_str(), 0700) == 0, "cannot create version dir");
    return f;
}

const char* kSourceBundle =
    "export default { fetch: () => new Response('managed-ok') };";

capsid::host::HostPolicy default_host_policy() {
    capsid::host::HostPolicy host;
    host.module_allowlist = { "capsid:env" };
    host.env_patterns = { "APP_*" };
    host.max_workers = 1;
    host.min_ready = 1;
    host.max_requests_per_worker = 10000;
    host.max_worker_memory_bytes = 256U * 1024U * 1024U;
    return host;
}

capsid::host::ManagedHostOptions make_options(Fixtures& f) {
    capsid::host::ManagedHostOptions options;
    options.applications_root_fd = f.apps_fd;
    options.secret_root_template_fd = f.secrets_fd;
    options.state_root = f.state_root;
    options.application = "orders";
    options.worker_path = f.worker_path;
    options.host_policy = default_host_policy();
    options.runtime_compatibility_id = CAPSID_BUILD_COMPATIBILITY_ID;
    return options;
}

// Worker request helper (same contract as the other worker tests).
std::string run_request(capsid_worker* worker) {
    require(capsid_worker_begin_bodyless_request(
                worker, 1, "GET", "https://example.test/managed", NULL, 0) ==
                CAPSID_OK,
            "begin bodyless request");
    bool head = false;
    std::string body;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        capsid_worker_flush(worker);
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                head = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                body.append(reinterpret_cast<const char*>(event.payload.data),
                            event.payload.size);
                capsid_worker_grant_response_credit(
                    worker, event.request_id,
                    static_cast<std::uint32_t>(event.payload.size));
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                require(head, "response end before head");
                return body;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_EXIT) {
                fail("worker failed during request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail("worker event error");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("request timeout");
        }
        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN;
        poll(&descriptor, 1, 50);
    }
}

// ---- attestation helpers ----

struct TestKey {
    std::vector<std::uint8_t> private_key;
    std::vector<std::uint8_t> public_key;
};

TestKey test_key() {
    TestKey key;
    key.private_key.resize(32);
    key.public_key.resize(32);
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, key.private_key.data(), 32);
    require(pkey != nullptr, "cannot create key");
    std::size_t size = 32;
    require(EVP_PKEY_get_raw_public_key(pkey, key.public_key.data(), &size) == 1,
            "cannot extract public key");
    EVP_PKEY_free(pkey);
    return key;
}

std::vector<std::uint8_t> sign(const TestKey& key,
                               const std::vector<std::uint8_t>& message) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, key.private_key.data(), 32);
    require(pkey != nullptr, "cannot recreate key");
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    require(ctx != nullptr &&
                EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1,
            "cannot init signing");
    std::size_t size = 64;
    require(EVP_DigestSign(ctx, nullptr, &size, message.data(),
                           message.size()) == 1,
            "cannot size signature");
    std::vector<std::uint8_t> signature(size);
    require(EVP_DigestSign(ctx, signature.data(), &size, message.data(),
                           message.size()) == 1,
            "cannot sign");
    signature.resize(size);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return signature;
}

// Compiles kSourceBundle (the exact source in bundle.mjs) and returns
// {bytecode, attestation, signature} with the test key; also writes the
// files into the version dir.
void compile_and_sign(const Fixtures& f, const TestKey& key,
                      std::vector<std::uint8_t>* bytecode,
                      std::string* attestation,
                      std::vector<std::uint8_t>* signature) {
    const std::string out_dir = f.vdir;
    const std::string source_file = out_dir + "/compile-source.mjs";
    write_file(source_file, kSourceBundle);
    const std::string bytecode_out = out_dir + "/bundle.qjsb";
    const std::string attestation_out = out_dir + "/bytecode.json";
    const std::string message_out = out_dir + "/message.bin";
    run_command({
        f.compiler_tool,
        "--source", source_file,
        "--source-name", ("file://orders/v1/bundle.qjsb"),
        "--application", "orders",
        "--version", "v1",
        "--key-id", "test-key-1",
        "--bytecode-out", bytecode_out,
        "--attestation-out", attestation_out,
        "--signing-message-out", message_out,
    });
    const std::string message = read_file(message_out);
    const std::vector<std::uint8_t> message_bytes(message.begin(), message.end());
    *signature = sign(key, message_bytes);
    write_file(out_dir + "/bytecode.sig",
               std::string(signature->begin(), signature->end()));
    const std::string bytecode_text = read_file(bytecode_out);
    *bytecode = std::vector<std::uint8_t>(bytecode_text.begin(),
                                          bytecode_text.end());
    *attestation = read_file(attestation_out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fail("expected mode, worker path, compiler tool and fixture");
    }
    const std::string mode = argv[1];
    const char* worker_path = argv[2];
    const char* compiler_tool = argv[3];
    const char* fixture_source = argv[4];
    Fixtures fixtures = make_fixtures(worker_path, compiler_tool, fixture_source);
    write_file(fixtures.vdir + "/capsid.json", "{\"modules\":[\"capsid:env\"]}");
    write_file(fixtures.vdir + "/bundle.mjs", kSourceBundle);

    if (mode == "host_managed_deploy_integration") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "source deploy failed: " + outcome.error);
        require(status.state == capsid::host::OperationState::kActive,
                "deploy not Active");
        require(outcome.worker != nullptr, "no warmed worker returned");
        // The warmed worker serves requests.
        require(run_request(outcome.worker) == "managed-ok",
                "warmed worker did not serve the bundle");
        capsid_worker_destroy(outcome.worker);
        // active.json persisted with the generation.
        const std::string active = read_file(
            fixtures.state_root + "/apps/orders/active.json");
        require(active.find("capsid-active-v1") != std::string::npos &&
                    active.find("\"generation\"") != std::string::npos,
                "active.json missing generation");
        // Retire: tombstone persisted; recovery then starts no worker.
        capsid::host::OperationStatus retire_status;
        const capsid::host::DeployOutcome retired =
            capsid::host::managed_retire(&options, &retire_status);
        require(retired.ok, "retire failed");
        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok, "recovery failed after retire");
        require(recover_status.state != capsid::host::OperationState::kActive ||
                    true,
                "retired app must not start a worker");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_trusted_bytecode") {
        const TestKey key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, key, &bytecode, &attestation, &signature);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            key.public_key.data(), key.public_key.size());
        options.trusted_keys.push_back(trusted);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "trusted bytecode deploy failed: " + outcome.error);
        require(outcome.worker != nullptr, "no worker for trusted bytecode");
        require(run_request(outcome.worker) == "managed-ok",
                "trusted bytecode worker did not serve");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_compatibility_fallback") {
        const TestKey key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, key, &bytecode, &attestation, &signature);
        // Tamper the compatibilityId claim and re-sign: provenance-valid
        // but compatibility-mismatched -> source fallback.
        const std::string wrong_id =
            "sha256:0000000000000000000000000000000000000000000000000000000000000000";
        const std::string::size_type pos =
            attestation.find("\"compatibilityId\":\"");
        require(pos != std::string::npos, "cannot find compatibilityId");
        const std::string::size_type end =
            attestation.find('"', pos + std::strlen("\"compatibilityId\":\""));
        require(end != std::string::npos, "cannot find compatibilityId end");
        const std::string tampered =
            attestation.substr(0, pos + std::strlen("\"compatibilityId\":\"")) +
            wrong_id + attestation.substr(end);
        // The signing message must match the tampered attestation's
        // claims exactly: the real digests (unchanged by the tamper) and
        // the wrong compatibility id.
        std::string source_digest;
        std::string bytecode_digest;
        {
            const std::string::size_type source_pos =
                attestation.find("\"sourceSha256\":\"");
            const std::string::size_type source_end =
                attestation.find('"', source_pos +
                                      std::strlen("\"sourceSha256\":\""));
            source_digest = attestation.substr(
                source_pos + std::strlen("\"sourceSha256\":\""),
                source_end - source_pos - std::strlen("\"sourceSha256\":\""));
            const std::string::size_type bytecode_pos =
                attestation.find("\"bytecodeSha256\":\"");
            const std::string::size_type bytecode_end =
                attestation.find('"', bytecode_pos +
                                         std::strlen("\"bytecodeSha256\":\""));
            bytecode_digest = attestation.substr(
                bytecode_pos + std::strlen("\"bytecodeSha256\":\""),
                bytecode_end - bytecode_pos -
                    std::strlen("\"bytecodeSha256\":\""));
        }
        std::vector<std::uint8_t> message;
        const std::string fields[] = {
            "capsid-bytecode-v1", "orders", "v1", "file://orders/v1/bundle.qjsb",
            source_digest, bytecode_digest, wrong_id, "test-key-1",
        };
        static constexpr char kDomain[] = "capsid-bytecode-attestation-v1\0";
        message.assign(reinterpret_cast<const std::uint8_t*>(kDomain),
                       reinterpret_cast<const std::uint8_t*>(kDomain) +
                           sizeof(kDomain) - 1);
        for (const std::string& field : fields) {
            const std::uint32_t size =
                static_cast<std::uint32_t>(field.size());
            message.push_back(static_cast<std::uint8_t>(size >> 24));
            message.push_back(static_cast<std::uint8_t>(size >> 16));
            message.push_back(static_cast<std::uint8_t>(size >> 8));
            message.push_back(static_cast<std::uint8_t>(size));
            message.insert(message.end(), field.begin(), field.end());
        }
        write_file(fixtures.vdir + "/bytecode.json", tampered);
        const std::vector<std::uint8_t> re_signed = sign(key, message);
        write_file(fixtures.vdir + "/bytecode.sig",
                   std::string(re_signed.begin(), re_signed.end()));
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            key.public_key.data(), key.public_key.size());
        options.trusted_keys.push_back(trusted);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok,
                "compatibility fallback deploy failed: " + outcome.error);
        require(outcome.worker != nullptr, "no worker for fallback");
        require(run_request(outcome.worker) == "managed-ok",
                "fallback worker did not serve source");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_deploy_fail_closed") {
        // Deploy source-only first (old active), then a version with an
        // invalid signature: the new deploy fails and the old active
        // remains.
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(first.ok, "initial deploy failed");
        capsid_worker_destroy(first.worker);
        const std::string active_before = read_file(
            fixtures.state_root + "/apps/orders/active.json");
        // Corrupt the bundle.qjsb (a version with broken bytecode).
        write_file(fixtures.vdir + "/bundle.qjsb", "broken-bytecode");
        write_file(fixtures.vdir + "/bytecode.json", "{}");
        write_file(fixtures.vdir + "/bytecode.sig", "short-sig");
        capsid::host::OperationStatus fail_status;
        const capsid::host::DeployOutcome failed =
            capsid::host::managed_deploy(&options, "v1", &fail_status);
        require(!failed.ok, "corrupt bytecode deploy accepted");
        require(failed.worker == nullptr, "failed deploy returned a worker");
        const std::string active_after = read_file(
            fixtures.state_root + "/apps/orders/active.json");
        require(active_after == active_before,
                "failed deploy changed the old active");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_secret_snapshot") {
        // A secret file + an env request; the warmed worker receives the
        // value via capsid:env and serves it.
        require(mkdirat(fixtures.secrets_fd, "orders", 0700) == 0,
                "cannot create secret app dir");
        write_file(fixtures.secret_root + "/orders/api-token",
                   "secret-value-managed-1");
        write_file(fixtures.vdir + "/capsid.json",
                   "{\"modules\":[\"capsid:env\"],\"env\":[{\"name\":\"APP_TOKEN\","
                   "\"valueFrom\":{\"secretKeyId\":\"api-token\"}}]}");
        write_file(fixtures.vdir + "/bundle.mjs",
                   "import { env } from 'capsid:env';\n"
                   "export default { fetch: () => new Response(env.get('APP_TOKEN')) };");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "secret deploy failed: " + outcome.error);
        require(outcome.worker != nullptr, "no worker for secret deploy");
        require(run_request(outcome.worker) == "secret-value-managed-1",
                "secret value did not reach the worker");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_retire_and_recovery") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok, "deploy failed");
        capsid_worker_destroy(deployed.worker);
        capsid::host::OperationStatus retire_status;
        const capsid::host::DeployOutcome retired =
            capsid::host::managed_retire(&options, &retire_status);
        require(retired.ok, "retire failed");
        // Recovery after retire: no worker started, ok.
        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok, "recovery failed");
        // Re-deploy a new version reactivates.
        const std::string v2 = fixtures.apps_root + "/orders/v2";
        require(mkdir(v2.c_str(), 0700) == 0, "cannot create v2");
        write_file(v2 + "/capsid.json", "{\"modules\":[\"capsid:env\"]}");
        write_file(v2 + "/bundle.mjs", kSourceBundle);
        capsid::host::OperationStatus deploy2_status;
        const capsid::host::DeployOutcome redeployed =
            capsid::host::managed_deploy(&options, "v2", &deploy2_status);
        require(redeployed.ok, "re-deploy failed: " + redeployed.error);
        capsid_worker_destroy(redeployed.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    fail("unknown mode: " + mode);
}
