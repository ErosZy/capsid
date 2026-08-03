// Frozen RED: runtime_bytecode_compiler_round_trip (M1D).
//
// Proves the trusted-bytecode deployment path end to end:
//   compiler (capsid-bytecode-compile) → offline Ed25519 sign (test key) →
//   host verifier (verify_bytecode_attestation) → trusted worker load
//   (capsid_worker_load_trusted_bytecode_named) → identical request result
//   to the source-loaded worker.
//
// Also asserts:
//   - compiler/library/worker compatibility IDs are identical;
//   - repeated compiles are bit-for-bit identical;
//   - tampering any of sourceName, digests, bytecode or signature, or the
//     compatibility claim without a valid signature, fails closed.

#include "capsid/runtime.h"
#include "host/bytecode_attestation.h"

#include <jansson.h>
#include <openssl/evp.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "build_identity.h"

namespace {

void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require_result(capsid_result result, const char* operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open file: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_bytes(const char* path) {
    const std::string text = read_file(path);
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string run_command(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const std::string& argument : argv) {
        args.push_back(const_cast<char*>(argument.c_str()));
    }
    args.push_back(nullptr);

    int pipes[2];
    if (pipe(pipes) != 0) {
        fail("cannot create pipe");
    }
    const pid_t pid = fork();
    if (pid < 0) {
        fail("cannot fork");
    }
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
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("command failed (" + argv[0] + "): " + output);
    }
    return output;
}

// ---- worker harness (same contract as test_worker_integration) ----

void wait_for_ready(capsid_worker* worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker startup error: ") +
                     std::string(reinterpret_cast<const char*>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for READY");
        }
        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
    }
}

std::string run_request(capsid_worker* worker) {
    require_result(
        capsid_worker_begin_bodyless_request(
            worker, 1, "GET", "https://example.test/sync", NULL, 0),
        "begin bodyless request");
    bool received_head = false;
    std::string body;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("request flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                if (event.request_id != 1 || event.status != 200) {
                    fail("unexpected response head");
                }
                received_head = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                if (!received_head || event.request_id != 1) {
                    fail("response body arrived before its head");
                }
                body.append(reinterpret_cast<const char*>(event.payload.data),
                            event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, event.request_id,
                        static_cast<std::uint32_t>(event.payload.size)),
                    "replenish response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                if (!received_head || event.request_id != 1) {
                    fail("unexpected response end");
                }
                return body;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker request error: ") +
                     std::string(reinterpret_cast<const char*>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("request event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for the response");
        }
        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
    }
}

capsid_worker* spawn_worker(const char* worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    capsid_worker* worker = nullptr;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    return worker;
}

// ---- signing helpers (offline pipeline with a test key) ----

struct TestKey {
    std::vector<std::uint8_t> private_key;
    std::vector<std::uint8_t> public_key;
};

TestKey generate_test_key() {
    TestKey key;
    key.private_key.resize(32);
    key.public_key.resize(32);
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, key.private_key.data(), 32);
    if (pkey == nullptr) {
        fail("cannot create Ed25519 key");
    }
    std::size_t public_size = 32;
    if (EVP_PKEY_get_raw_public_key(pkey, key.public_key.data(), &public_size) !=
        1) {
        fail("cannot extract public key");
    }
    EVP_PKEY_free(pkey);
    return key;
}

std::vector<std::uint8_t> sign_message(const TestKey& key,
                                       const std::vector<std::uint8_t>& message) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, key.private_key.data(), 32);
    if (pkey == nullptr) {
        fail("cannot recreate Ed25519 key");
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr ||
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) != 1) {
        fail("cannot init signing");
    }
    std::size_t signature_size = 64;
    if (EVP_DigestSign(ctx, nullptr, &signature_size, message.data(),
                       message.size()) != 1) {
        fail("cannot size signature");
    }
    std::vector<std::uint8_t> signature(signature_size);
    if (EVP_DigestSign(ctx, signature.data(), &signature_size, message.data(),
                       message.size()) != 1) {
        fail("cannot sign");
    }
    signature.resize(signature_size);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return signature;
}

struct AttestationJson {
    std::string application;
    std::string version;
    std::string source_name;
    std::string source_sha256;
    std::string bytecode_sha256;
    std::string compatibility_id;
    std::string key_id;
};

AttestationJson parse_attestation(const std::string& json) {
    json_error_t error;
    json_t* root = json_loads(json.c_str(), JSON_REJECT_DUPLICATES, &error);
    if (root == nullptr) {
        fail("cannot parse attestation");
    }
    AttestationJson claims;
    const char* names[] = { "application",    "version",       "sourceName",
                            "sourceSha256",   "bytecodeSha256", "compatibilityId",
                            "keyId" };
    std::string* targets[] = { &claims.application,      &claims.version,
                               &claims.source_name,      &claims.source_sha256,
                               &claims.bytecode_sha256,  &claims.compatibility_id,
                               &claims.key_id };
    for (std::size_t index = 0; index < 7; ++index) {
        json_t* value = json_object_get(root, names[index]);
        if (!json_is_string(value)) {
            json_decref(root);
            fail(std::string("attestation missing string field: ") + names[index]);
        }
        *targets[index] = json_string_value(value);
    }
    json_decref(root);
    return claims;
}

capsid::host::BytecodeAttestationResult verify(
    const std::vector<std::uint8_t>& source,
    const std::vector<std::uint8_t>& bytecode,
    const std::string& attestation_json,
    const std::vector<std::uint8_t>& signature,
    const TestKey& key) {
    const AttestationJson claims = parse_attestation(attestation_json);
    const capsid::host::TrustedBytecodeKey trusted_key = {
        claims.key_id,
        std::span<const std::uint8_t>(key.public_key.data(),
                                      key.public_key.size()),
    };
    capsid::host::BytecodeAttestationInput input;
    input.source = std::span<const std::uint8_t>(source.data(), source.size());
    input.bytecode =
        std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(bytecode.data(), bytecode.size()));
    input.attestation_json = std::string_view(attestation_json);
    input.signature =
        std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(signature.data(), signature.size()));
    input.expected_application = claims.application;
    input.expected_version = claims.version;
    input.expected_source_name = claims.source_name;
    input.runtime_compatibility_id = claims.compatibility_id;
    input.trusted_keys =
        std::span<const capsid::host::TrustedBytecodeKey>(&trusted_key, 1);
    return capsid::host::verify_bytecode_attestation(input);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fail("expected compiler tool, worker path and fixture path");
    }
    const char* compiler_tool = argv[1];
    const char* worker_path = argv[2];
    const char* fixture_path = argv[3];

    const std::string work_dir = "/tmp/capsid-roundtrip";
    const std::string command =
        std::string("rm -rf ") + work_dir + " && mkdir -p " + work_dir;
    if (system(command.c_str()) != 0) {
        fail("cannot prepare work dir");
    }

    // 1. Compile the fixture with the frozen CLI.
    const std::string source_name = "file:///app/sync-app.js";
    const std::string bytecode_out = work_dir + "/bundle.qjsb";
    const std::string attestation_out = work_dir + "/bytecode.json";
    const std::string message_out = work_dir + "/message.bin";
    run_command({
        compiler_tool,
        "--source", fixture_path,
        "--source-name", source_name,
        "--application", "orders",
        "--version", "2026-08-03-001",
        "--key-id", "test-key-1",
        "--bytecode-out", bytecode_out,
        "--attestation-out", attestation_out,
        "--signing-message-out", message_out,
    });

    const std::vector<std::uint8_t> source = read_bytes(fixture_path);
    const std::vector<std::uint8_t> bytecode = read_bytes(bytecode_out.c_str());
    const std::string attestation = read_file(attestation_out.c_str());
    const std::vector<std::uint8_t> message = read_bytes(message_out.c_str());
    const AttestationJson claims = parse_attestation(attestation);

    // 2. Compatibility identity: compiler == library == worker.
    const std::string tool_id = run_command({ compiler_tool, "--print-compatibility-id" });
    std::string tool_id_trimmed = tool_id;
    while (!tool_id_trimmed.empty() &&
           (tool_id_trimmed.back() == '\n' || tool_id_trimmed.back() == '\r')) {
        tool_id_trimmed.pop_back();
    }
    if (tool_id_trimmed != CAPSID_BUILD_COMPATIBILITY_ID) {
        fail("compiler compatibility ID does not match the library's");
    }
    if (claims.compatibility_id != CAPSID_BUILD_COMPATIBILITY_ID) {
        fail("attestation compatibility ID does not match the library's");
    }

    // 3. Determinism: a second compile is bit-for-bit identical.
    const std::string bytecode_out2 = work_dir + "/bundle2.qjsb";
    const std::string attestation_out2 = work_dir + "/bytecode2.json";
    const std::string message_out2 = work_dir + "/message2.bin";
    run_command({
        compiler_tool,
        "--source", fixture_path,
        "--source-name", source_name,
        "--application", "orders",
        "--version", "2026-08-03-001",
        "--key-id", "test-key-1",
        "--bytecode-out", bytecode_out2,
        "--attestation-out", attestation_out2,
        "--signing-message-out", message_out2,
    });
    if (read_bytes(bytecode_out2.c_str()) != bytecode ||
        read_file(attestation_out2.c_str()) != attestation ||
        read_bytes(message_out2.c_str()) != message) {
        fail("repeated compile is not bit-for-bit identical");
    }

    // 4. Offline signing with a test key, then the host verifier accepts.
    const TestKey key = generate_test_key();
    const std::vector<std::uint8_t> signature = sign_message(key, message);
    const capsid::host::BytecodeAttestationResult verified = verify(
        source, bytecode, attestation, signature, key);
    if (verified.selection != capsid::host::BytecodeArtifactSelection::kTrustedBytecode ||
        verified.code != capsid::host::BytecodeAttestationErrorCode::kNone) {
        fail("verified artifact did not select trusted bytecode: " +
             verified.message);
    }

    // 5. Trusted worker loads the bytecode and returns the same result as
    // the source-loaded worker.
    std::string trusted_body;
    {
        capsid_worker* worker = spawn_worker(worker_path);
        require_result(
            capsid_worker_load_trusted_bytecode_named(
                worker, bytecode.data(), bytecode.size(), source_name.c_str()),
            "load trusted bytecode");
        wait_for_ready(worker);
        trusted_body = run_request(worker);
        capsid_worker_destroy(worker);
    }
    std::string source_body;
    {
        capsid_worker* worker = spawn_worker(worker_path);
        require_result(
            capsid_worker_load_bundle_named(
                worker, source.data(), source.size(), source_name.c_str()),
            "load source bundle");
        wait_for_ready(worker);
        source_body = run_request(worker);
        capsid_worker_destroy(worker);
    }
    if (trusted_body != source_body) {
        fail("trusted bytecode result differs from the source result: '" +
             trusted_body + "' vs '" + source_body + "'");
    }

    // 6. Tamper matrix: every single-bit-class tamper fails closed.
    //    a. signature bit flip -> kInvalidSignature.
    {
        std::vector<std::uint8_t> bad_signature = signature;
        bad_signature[0] ^= 0x01;
        const capsid::host::BytecodeAttestationResult result =
            verify(source, bytecode, attestation, bad_signature, key);
        if (result.code != capsid::host::BytecodeAttestationErrorCode::kInvalidSignature) {
            fail("flipped signature bit was not rejected");
        }
    }
    //    b. bytecode bit flip -> digest mismatch.
    {
        std::vector<std::uint8_t> bad_bytecode = bytecode;
        bad_bytecode[10] ^= 0x01;
        const capsid::host::BytecodeAttestationResult result =
            verify(source, bad_bytecode, attestation, signature, key);
        if (result.code != capsid::host::BytecodeAttestationErrorCode::kDigestMismatch) {
            fail("flipped bytecode bit was not rejected");
        }
    }
    //    c. sourceName tamper without re-signing -> invalid signature.
    {
        std::string bad_attestation = attestation;
        const std::string::size_type pos =
            bad_attestation.find(source_name);
        if (pos == std::string::npos) {
            fail("cannot locate sourceName in attestation");
        }
        bad_attestation.replace(pos, 1, "x");
        const capsid::host::BytecodeAttestationResult result =
            verify(source, bytecode, bad_attestation, signature, key);
        if (result.code != capsid::host::BytecodeAttestationErrorCode::kInvalidSignature) {
            fail("tampered sourceName was not rejected");
        }
    }
    //    d. digest claim tamper -> the signature covers the original
    //    claims, so any claim edit (without a recompiled message) is an
    //    invalid signature; a re-signed tampered digest is a digest
    //    mismatch on the artifact hash.
    {
        std::string bad_attestation = attestation;
        const std::string::size_type pos =
            bad_attestation.find("\"sourceSha256\":\"sha256:");
        if (pos == std::string::npos) {
            fail("cannot locate sourceSha256 in attestation");
        }
        // First hex digit after the frozen "sha256:" prefix.
        bad_attestation.replace(pos + 24, 1, "0");
        const capsid::host::BytecodeAttestationResult unsigned_result = verify(
            source, bytecode, bad_attestation, signature, key);
        if (unsigned_result.code !=
            capsid::host::BytecodeAttestationErrorCode::kInvalidSignature) {
            fail("tampered digest claim was not rejected");
        }
        // A digest claim tamper with a matching signature is impossible to
        // produce without a recompiled message; the artifact-hash path is
        // covered by case (b). Assert the tampered attestation is rejected
        // under the same key regardless of signing.
        const capsid::host::BytecodeAttestationResult re_signed = verify(
            source, bytecode, bad_attestation,
            sign_message(key, read_bytes(message_out.c_str())), key);
        if (re_signed.code !=
            capsid::host::BytecodeAttestationErrorCode::kInvalidSignature) {
            fail("digest tamper path did not fail closed");
        }
    }

    std::cout << "PASS" << std::endl;
    return 0;
}
