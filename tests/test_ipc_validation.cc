#include "ipc_validation.h"
#include "protocol.h"
#include "response_headers.h"
#include "capsid/runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "test-ipc-validation: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

void append_string16(std::vector<uint8_t> *output,
                     const std::string &value) {
    capsid::protocol::append_u16(
        output, static_cast<uint16_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

void append_string32(std::vector<uint8_t> *output,
                     const std::string &value) {
    capsid::protocol::append_u32(
        output, static_cast<uint32_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

capsid::protocol::Frame hello_frame(
    const std::string &ca_path = std::string(),
    const std::string &egress_target = std::string(),
    const std::string &application_identity = std::string(),
    const std::string &allowed_module = std::string(),
    const std::string &permission_resource = std::string(),
    const std::string &env_name = std::string(),
    const std::string &env_value = std::string(),
    uint32_t capability_version =
        CAPSID_CAPABILITY_POLICY_VERSION) {
    capsid::protocol::Frame hello;
    hello.type = capsid::protocol::kHello;
    hello.flags = 0;
    hello.request_id = 0;
    capsid::protocol::append_u32(&hello.payload, CAPSID_ABI_VERSION);
    capsid::protocol::append_u64(
        &hello.payload, 64u * 1024u * 1024u);
    capsid::protocol::append_u64(&hello.payload, 0);
    capsid::protocol::append_u32(&hello.payload, 64);
    capsid::protocol::append_u64(&hello.payload, 5000);
    capsid::protocol::append_u32(&hello.payload, 1024u * 1024u);
    capsid::protocol::append_u32(&hello.payload, 4);
    capsid::protocol::append_u32(&hello.payload, 1024);
    capsid::protocol::append_u32(
        &hello.payload, 64u * 1024u);
    capsid::protocol::append_u32(
        &hello.payload, 4u * 1024u * 1024u);
    hello.payload.push_back(0);
    capsid::protocol::append_u32(&hello.payload, 0);
    capsid::protocol::append_u32(&hello.payload, 0);
    append_string16(&hello.payload, ca_path);
    capsid::protocol::append_u64(&hello.payload, 0);
    capsid::protocol::append_u64(&hello.payload, 0);
    capsid::protocol::append_u32(
        &hello.payload, CAPSID_EGRESS_DENY);
    capsid::protocol::append_u32(
        &hello.payload, egress_target.empty() ? 0 : 1);
    if (!egress_target.empty()) {
        capsid::protocol::append_u32(
            &hello.payload, CAPSID_EGRESS_ALLOW);
        capsid::protocol::append_u16(&hello.payload, 443);
        capsid::protocol::append_u16(&hello.payload, 443);
        capsid::protocol::append_u32(&hello.payload, 71);
        append_string16(&hello.payload, egress_target);
    }
    hello.payload.push_back(egress_target.empty() ? 0 : 1);
    const bool capability_enabled =
        !application_identity.empty() ||
        !allowed_module.empty() ||
        !permission_resource.empty() ||
        !env_name.empty();
    capsid::protocol::append_u32(
        &hello.payload,
        capability_enabled ? capability_version : 0);
    append_string16(&hello.payload, application_identity);
    capsid::protocol::append_u16(
        &hello.payload, allowed_module.empty() ? 0 : 1);
    if (!allowed_module.empty()) {
        append_string16(&hello.payload, allowed_module);
    }
    capsid::protocol::append_u16(
        &hello.payload, permission_resource.empty() ? 0 : 1);
    if (!permission_resource.empty()) {
        capsid::protocol::append_u32(
            &hello.payload, CAPSID_PERMISSION_ALLOW);
        capsid::protocol::append_u32(
            &hello.payload, CAPSID_PERMISSION_ENV);
        capsid::protocol::append_u32(&hello.payload, 81);
        append_string16(&hello.payload, permission_resource);
    }
    capsid::protocol::append_u32(
        &hello.payload, CAPSID_EGRESS_DENY);
    capsid::protocol::append_u32(&hello.payload, 0);
    if (!capability_enabled ||
        capability_version ==
            CAPSID_CAPABILITY_POLICY_VERSION) {
        capsid::protocol::append_u16(
            &hello.payload, env_name.empty() ? 0 : 1);
        if (!env_name.empty()) {
            append_string16(&hello.payload, env_name);
            append_string16(&hello.payload, env_value);
        }
    }
    return hello;
}

void test_hello_and_bundle_state_machine() {
    const capsid::protocol::Frame valid = hello_frame();
    require(
        valid.payload.size() ==
            capsid::protocol::kHelloFixedPayloadSize,
        "canonical HELLO fixture size drifted");

    for (size_t size = 0; size < valid.payload.size(); ++size) {
        capsid::protocol::Frame truncated = valid;
        truncated.payload.resize(size);
        capsid::WorkerStartupState state;
        std::string error;
        if (size ==
            capsid::protocol::kHelloLegacyFixedPayloadSize) {
            require(
                state.consume(truncated, &error),
                "legacy version-0 HELLO was rejected");
            continue;
        }
        require(
            !state.consume(truncated, &error),
            "truncated HELLO was accepted");
        require(
            !state.hello_received() && !error.empty(),
            "truncated HELLO mutated startup state");
    }

    capsid::WorkerStartupState state;
    std::string error;
    require(state.consume(valid, &error), error);
    require(state.hello_received(), "HELLO state not committed");
    require(
        !state.consume(valid, &error),
        "duplicate HELLO was accepted");

    capsid::protocol::Frame wrong_id = valid;
    wrong_id.request_id = 7;
    capsid::WorkerStartupState wrong_id_state;
    require(
        !wrong_id_state.consume(wrong_id, &error),
        "nonzero HELLO request id was accepted");

    capsid::WorkerStartupState nul_ca_state;
    require(
        !nul_ca_state.consume(
            hello_frame(std::string("ca\0path", 7)), &error),
        "NUL-containing CA path was accepted");

    capsid::WorkerStartupState nul_target_state;
    require(
        !nul_target_state.consume(
            hello_frame(
                std::string(),
                std::string("api\0.example", 12)),
            &error),
        "NUL-containing egress target was accepted");

    const capsid::protocol::Frame capability = hello_frame(
        std::string(),
        std::string(),
        "application-a",
        "capsid:permissions",
        "APP_*");
    capsid::WorkerStartupState capability_state;
    require(
        capability_state.consume(capability, &error),
        error);
    require(
        capability_state.config().capability_policy.enabled() &&
            !capability_state.config()
                 .legacy_egress_configured &&
            capability_state.config()
                    .capability_policy.application_identity() ==
                "application-a" &&
            capability_state.config()
                    .capability_policy.module_decision(
                        "capsid:permissions") ==
                capsid::kModuleGranted &&
            capability_state.config()
                    .capability_policy.evaluate(
                        CAPSID_PERMISSION_ENV,
                        "APP_MODE")
                    .state ==
                CAPSID_PERMISSION_STATE_GRANTED,
        "capability startup snapshot decoded incorrectly");

    const capsid::protocol::Frame legacy_capability =
        hello_frame(
            std::string(),
            std::string(),
            "legacy-application",
            "capsid:permissions",
            "APP_*",
            std::string(),
            std::string(),
            CAPSID_CAPABILITY_POLICY_VERSION_1);
    capsid::WorkerStartupState legacy_capability_state;
    require(
        legacy_capability_state.consume(
            legacy_capability,
            &error) &&
            legacy_capability_state.config()
                    .capability_policy.version() ==
                CAPSID_CAPABILITY_POLICY_VERSION_1,
        "legacy capability HELLO was rejected");

    const capsid::protocol::Frame environment = hello_frame(
        std::string(),
        std::string(),
        "environment-a",
        "capsid:env",
        "APP_*",
        "APP_MODE",
        "production");
    capsid::WorkerStartupState environment_state;
    require(
        environment_state.consume(environment, &error),
        error);
    std::string environment_value;
    require(
        environment_state.config()
                .capability_policy.env_value(
                    "APP_MODE",
                    &environment_value) &&
            environment_value == "production",
        "environment startup snapshot decoded incorrectly");

    capsid::WorkerStartupState nul_environment_state;
    require(
        !nul_environment_state.consume(
            hello_frame(
                std::string(),
                std::string(),
                "environment-a",
                "capsid:env",
                "APP_*",
                "APP_MODE",
                std::string("safe\0hidden", 11)),
            &error),
        "NUL-containing environment value was accepted");
    for (size_t size = 0;
         size < capability.payload.size();
         ++size) {
        capsid::protocol::Frame truncated = capability;
        truncated.payload.resize(size);
        capsid::WorkerStartupState truncated_state;
        require(
            !truncated_state.consume(truncated, &error),
            "truncated capability HELLO was accepted");
        require(
            !truncated_state.hello_received(),
            "truncated capability HELLO partially committed");
    }

    capsid::WorkerStartupState nul_module_state;
    require(
        !nul_module_state.consume(
            hello_frame(
                std::string(),
                std::string(),
                "application-a",
                std::string("capsid:permissions\0hidden", 25),
                std::string()),
            &error),
        "NUL-containing allowed module was accepted");

    capsid::protocol::Frame invalid_capability = capability;
    /*
     * The final capability net header is 8 bytes. Immediately before it are
     * the ENV resource bytes; corrupt the permission enum while preserving a
     * structurally complete frame.
     */
    const std::string marker("APP_*");
    const std::vector<uint8_t>::iterator marker_at =
        std::search(
            invalid_capability.payload.begin(),
            invalid_capability.payload.end(),
            marker.cbegin(),
            marker.cend(),
            [](uint8_t a, char b) { return static_cast<char>(a) == b; });
    require(
        marker_at != invalid_capability.payload.end(),
        "capability fixture marker not found");
    const size_t resource_offset = static_cast<size_t>(
        marker_at - invalid_capability.payload.begin());
    const size_t permission_offset =
        resource_offset - sizeof(uint32_t) * 2 -
        sizeof(uint16_t);
    invalid_capability.payload[permission_offset] = 0xff;
    capsid::WorkerStartupState invalid_capability_state;
    require(
        !invalid_capability_state.consume(
            invalid_capability, &error),
        "unknown capability permission was accepted");
    require(
        !invalid_capability_state.hello_received(),
        "invalid capability partially committed startup state");

    capsid::protocol::Frame trailing = valid;
    trailing.payload.push_back(0);
    capsid::WorkerStartupState trailing_state;
    require(
        !trailing_state.consume(trailing, &error),
        "HELLO trailing byte was accepted");

    capsid::protocol::Frame empty_named;
    empty_named.type = capsid::protocol::kLoadBundle;
    empty_named.flags =
        capsid::protocol::kFlagStart |
        capsid::protocol::kFlagEnd |
        capsid::protocol::kFlagBundleName;
    empty_named.request_id = 0;
    require(
        !state.consume(empty_named, &error),
        "empty named bundle frame was accepted");
    require(
        !state.bundle_started(),
        "invalid bundle frame partially committed state");

    capsid::protocol::Frame named = empty_named;
    append_string16(
        &named.payload, std::string("capsid:test\0hidden", 16));
    named.payload.push_back('x');
    require(
        !state.consume(named, &error),
        "NUL-containing bundle name was accepted");
    require(
        !state.bundle_started(),
        "invalid bundle name partially committed state");

    named.payload.clear();
    append_string16(&named.payload, "capsid:test/main");
    named.payload.push_back('x');
    require(state.consume(named, &error), error);
    require(
        state.bundle_complete() &&
            state.bundle_name() == "capsid:test/main" &&
            state.bundle().size() == 1 &&
            state.bundle()[0] == 'x',
        "valid named bundle did not commit atomically");
    require(
        !state.consume(named, &error),
        "bundle frame after completion was accepted");

    capsid::WorkerStartupState bytecode_state;
    require(
        bytecode_state.consume(valid, &error),
        "valid HELLO for bytecode state was rejected");
    capsid::protocol::Frame bytecode;
    bytecode.type = capsid::protocol::kLoadBundle;
    bytecode.flags =
        capsid::protocol::kFlagStart |
        capsid::protocol::kFlagEnd |
        capsid::protocol::kFlagTrustedBytecode;
    bytecode.request_id = 0;
    bytecode.payload.push_back(0x42);
    require(
        bytecode_state.consume(bytecode, &error),
        error);
    require(
        bytecode_state.bundle_complete() &&
            bytecode_state.bundle_is_trusted_bytecode() &&
            bytecode_state.bundle().size() == 1 &&
            bytecode_state.bundle()[0] == 0x42,
        "trusted bytecode startup flag was not retained");
}

// Builds one LOAD_BINDING wire blob chunk: a u32 descriptor length, the
// descriptor sections, then the raw source bytes.
std::vector<uint8_t> binding_blob(
    const std::string &name,
    const std::string &config,
    const std::vector<std::pair<std::string, std::string>> &secrets,
    const std::vector<std::string> &modules,
    const std::vector<std::string> &profiles,
    const std::vector<std::string> &net_rules,
    const std::vector<std::string> &fs_read,
    const std::vector<std::string> &fs_write,
    const std::vector<std::string> &env,
    const std::vector<std::string> &stdio,
    const std::string &source) {
    std::vector<uint8_t> descriptor;
    append_string16(&descriptor, name);
    append_string32(&descriptor, config);
    capsid::protocol::append_u32(
        &descriptor, static_cast<uint32_t>(secrets.size()));
    for (const auto &secret : secrets) {
        append_string16(&descriptor, secret.first);
        append_string32(&descriptor, secret.second);
    }
    const auto append_list = [&descriptor](
                                 const std::vector<std::string> &list) {
        capsid::protocol::append_u32(
            &descriptor, static_cast<uint32_t>(list.size()));
        for (const std::string &item : list) {
            append_string16(&descriptor, item);
        }
    };
    append_list(profiles);
    append_list(modules);
    append_list(net_rules);
    append_list(fs_read);
    append_list(fs_write);
    append_list(env);
    append_list(stdio);

    std::vector<uint8_t> blob;
    capsid::protocol::append_u32(
        &blob, static_cast<uint32_t>(descriptor.size()));
    blob.insert(blob.end(), descriptor.begin(), descriptor.end());
    blob.insert(blob.end(), source.begin(), source.end());
    return blob;
}

capsid::protocol::Frame load_binding_frame(uint32_t flags,
                                           const std::vector<uint8_t> &blob) {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kLoadBinding;
    frame.flags = flags;
    frame.request_id = 0;
    frame.payload = blob;
    return frame;
}

void test_binding_startup_state_machine() {
    const std::vector<uint8_t> blob = binding_blob(
        "mongo",
        R"json({"database":"orders"})json",
        {{"password", "s3cr3t"}},
        {"tjs:internal/core", "tjs:posix-socket"},
        {"network-client"},
        {"127.0.0.1:27017"},
        {"/etc/capsid/mongo"},
        {},
        {"APP_MODE"},
        {"stdout"},
        "export default () => ({});");

    // A LOAD_BINDING before HELLO is an invalid startup frame sequence.
    {
        capsid::WorkerStartupState state;
        std::string error;
        require(
            !state.consume(load_binding_frame(
                               capsid::protocol::kFlagStart |
                                   capsid::protocol::kFlagEnd,
                               blob),
                           &error),
            "LOAD_BINDING before HELLO was accepted");
        require(
            !state.hello_received() && state.bindings().empty(),
            "pre-HELLO LOAD_BINDING mutated startup state");
    }

    // A single start+end frame commits the descriptor atomically.
    {
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            state.consume(load_binding_frame(
                              capsid::protocol::kFlagStart |
                                  capsid::protocol::kFlagEnd,
                              blob),
                          &error),
            error);
        require(
            state.bindings().size() == 1,
            "single-frame LOAD_BINDING did not commit");
        const capsid::WorkerBindingDescriptor &binding =
            state.bindings().front();
        require(binding.name == "mongo", "binding name decoded wrong");
        require(
            binding.modules.size() == 2 &&
                binding.modules[0] == "tjs:internal/core" &&
                binding.modules[1] == "tjs:posix-socket",
            "binding modules decoded wrong");
        require(
            binding.config_json == R"json({"database":"orders"})json",
            "binding config decoded wrong");
        require(
            binding.secrets.size() == 1 &&
                binding.secrets[0].key == "password" &&
                std::string(binding.secrets[0].value.begin(),
                            binding.secrets[0].value.end()) == "s3cr3t",
            "binding secrets decoded wrong");
        require(
            binding.profiles.size() == 1 &&
                binding.profiles[0] == "network-client",
            "binding profiles decoded wrong");
        require(
            binding.net_rules.size() == 1 &&
                binding.net_rules[0] == "127.0.0.1:27017",
            "binding net rules decoded wrong");
        require(
            binding.fs_read.size() == 1 &&
                binding.fs_read[0] == "/etc/capsid/mongo" &&
                binding.fs_write.empty(),
            "binding fs paths decoded wrong");
        require(
            binding.env.size() == 1 && binding.env[0] == "APP_MODE",
            "binding env decoded wrong");
        require(
            binding.stdio.size() == 1 && binding.stdio[0] == "stdout",
            "binding stdio decoded wrong");
        require(
            std::string(binding.source.begin(), binding.source.end()) ==
                "export default () => ({});",
            "binding source decoded wrong");
    }

    // A chunked binding assembles the source in order.
    {
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            state.consume(
                load_binding_frame(capsid::protocol::kFlagStart,
                                   blob),
                &error),
            error);
        capsid::protocol::Frame middle = load_binding_frame(0, {});
        middle.payload.assign(4, 0x5a);
        require(
            state.consume(middle, &error),
            "chunked LOAD_BINDING middle frame was rejected");
        capsid::protocol::Frame end = load_binding_frame(
            capsid::protocol::kFlagEnd, {});
        end.payload.assign(2, 0x5b);
        require(
            state.consume(end, &error),
            "chunked LOAD_BINDING end frame was rejected");
        require(
            state.bindings().size() == 1,
            "chunked LOAD_BINDING did not commit");
        const capsid::WorkerBindingDescriptor &binding =
            state.bindings().front();
        require(
            std::string(binding.source.begin(), binding.source.end()) ==
                "export default () => ({});\x5a\x5a\x5a\x5a\x5b\x5b",
            "chunked binding source assembled in the wrong order");
    }

    // Descriptor validation failures reject the whole binding.
    {
        const std::vector<uint8_t> bad_name = binding_blob(
            "Mongo", "{}", {}, {}, {}, {}, {}, {}, {}, {}, "x");
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            !state.consume(load_binding_frame(
                               capsid::protocol::kFlagStart |
                                   capsid::protocol::kFlagEnd,
                               bad_name),
                           &error),
            "LOAD_BINDING with an invalid name was accepted");
        require(
            state.bindings().empty(),
            "rejected LOAD_BINDING partially committed state");
    }
    {
        const std::vector<uint8_t> bad_profile = binding_blob(
            "mongo", "{}", {}, {}, {"network-server"}, {}, {}, {}, {}, {},
            "x");
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            !state.consume(load_binding_frame(
                               capsid::protocol::kFlagStart |
                                   capsid::protocol::kFlagEnd,
                               bad_profile),
                           &error),
            "LOAD_BINDING with an unknown sandbox profile was accepted");
    }
    {
        const std::vector<uint8_t> bad_target = binding_blob(
            "mongo", "{}", {}, {}, {"network-client"}, {"noport"}, {}, {},
            {}, {}, "x");
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            !state.consume(load_binding_frame(
                               capsid::protocol::kFlagStart |
                                   capsid::protocol::kFlagEnd,
                               bad_target),
                           &error),
            "LOAD_BINDING with an invalid net target was accepted");
    }
    {
        const std::vector<uint8_t> huge_config = binding_blob(
            "mongo", std::string(256U * 1024U + 1, 'x'), {}, {}, {}, {},
            {}, {}, {}, {}, "x");
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            !state.consume(load_binding_frame(
                               capsid::protocol::kFlagStart |
                                   capsid::protocol::kFlagEnd,
                               huge_config),
                           &error),
            "LOAD_BINDING with an oversized config was accepted");
    }

    {
        const std::vector<uint8_t> bad_module = binding_blob(
            "mongo", "{}", {}, {"tjs:ffi"}, {}, {}, {}, {}, {}, {}, "x");
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            !state.consume(load_binding_frame(
                               capsid::protocol::kFlagStart |
                                   capsid::protocol::kFlagEnd,
                               bad_module),
                           &error),
            "LOAD_BINDING with a forbidden module was accepted");
    }
    {
        const std::vector<uint8_t> dup_module = binding_blob(
            "mongo", "{}", {}, {"tjs:utils", "tjs:utils"}, {}, {}, {}, {},
            {}, {}, "x");
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            !state.consume(load_binding_frame(
                               capsid::protocol::kFlagStart |
                                   capsid::protocol::kFlagEnd,
                               dup_module),
                           &error),
            "LOAD_BINDING with a duplicate module was accepted");
    }

    // A LOAD_BINDING after the bundle started is a sealed-sequence error.
    {
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        capsid::protocol::Frame bundle;
        bundle.type = capsid::protocol::kLoadBundle;
        bundle.flags = capsid::protocol::kFlagStart;
        bundle.request_id = 0;
        bundle.payload.push_back(0x42);
        require(state.consume(bundle, &error), error);
        require(
            !state.consume(load_binding_frame(
                               capsid::protocol::kFlagStart |
                                   capsid::protocol::kFlagEnd,
                               blob),
                           &error),
            "LOAD_BINDING after LOAD_BUNDLE start was accepted");
    }

    // A LOAD_BUNDLE cannot interleave a LOAD_BINDING sequence.
    {
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        require(
            state.consume(load_binding_frame(
                              capsid::protocol::kFlagStart,
                              blob),
                          &error),
            error);
        capsid::protocol::Frame bundle;
        bundle.type = capsid::protocol::kLoadBundle;
        bundle.flags = capsid::protocol::kFlagStart;
        bundle.request_id = 0;
        bundle.payload.push_back(0x42);
        require(
            !state.consume(bundle, &error),
            "LOAD_BUNDLE interleaved inside a LOAD_BINDING sequence");
    }

    // Zero LOAD_BINDING keeps the plain bundle path and an empty list.
    {
        capsid::WorkerStartupState state;
        std::string error;
        require(state.consume(hello_frame(), &error), error);
        capsid::protocol::Frame bundle;
        bundle.type = capsid::protocol::kLoadBundle;
        bundle.flags =
            capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
        bundle.request_id = 0;
        bundle.payload.push_back(0x42);
        require(state.consume(bundle, &error), error);
        require(
            state.bundle_complete() && state.bindings().empty(),
            "zero-binding startup diverged from the single-runtime path");
    }
}

// Binding v1 §7.4: the READY v4 proof is the 71-byte compatibility id
// plus an additive proof section; without bindings the payload stays the
// exact baseline.
void test_ready_proof_payload() {
    const std::string compat(
        "sha256:" +
        std::string(64, 'a'));
    const std::string digest(
        "sha256:" +
        std::string(64, 'b'));

    std::vector<uint8_t> baseline;
    require(
        capsid::append_ready_proof(
            &baseline, 7, 0, 0, "", ""),
        "baseline ready proof did not build");
    require(baseline.empty(),
            "zero-binding proof appended bytes");

    std::vector<uint8_t> extended;
    require(
        capsid::append_ready_proof(
            &extended, 0x123, 2, 3, "", digest),
        "extended ready proof did not build");
    capsid::WorkerReadyProof proof;
    std::string out_compat;
    std::string error;
    std::vector<uint8_t> payload(
        compat.begin(), compat.end());
    payload.insert(payload.end(), extended.begin(), extended.end());
    require(
        capsid::parse_ready_proof(payload, &out_compat, &proof, &error),
        "valid extended ready proof was rejected: " + error);
    require(out_compat == compat,
            "ready compat id parsed wrong");
    require(proof.extended && proof.applied_feature_bits == 0x123 &&
                proof.seccomp_mode == 2 && proof.landlock_abi == 3 &&
                proof.network_namespace_identity.empty() &&
                proof.sandbox_profile_digest == digest,
            "ready proof fields parsed wrong");

    // Namespace identity round-trips.
    std::vector<uint8_t> ns_payload(compat.begin(), compat.end());
    require(
        capsid::append_ready_proof(
            &ns_payload, 0, 0, 0, "ns-identity", digest),
        "namespace proof did not build");
    capsid::WorkerReadyProof ns_proof;
    require(
        capsid::parse_ready_proof(
            ns_payload, &out_compat, &ns_proof, &error),
        "namespace ready proof was rejected");
    require(ns_proof.network_namespace_identity == "ns-identity",
            "namespace identity parsed wrong");

    // The plain 71-byte baseline parses with no extension.
    capsid::WorkerReadyProof plain_proof;
    require(
        capsid::parse_ready_proof(
            std::vector<uint8_t>(compat.begin(), compat.end()),
            &out_compat,
            &plain_proof,
            &error),
        "baseline ready payload was rejected");
    require(!plain_proof.extended && out_compat == compat,
            "baseline ready payload parsed as extended");

    // Malformed extensions fail closed.
    std::vector<uint8_t> truncated = payload;
    truncated.pop_back();
    require(
        !capsid::parse_ready_proof(
            truncated, &out_compat, &proof, &error) &&
            !error.empty(),
        "truncated ready proof was accepted");
    std::vector<uint8_t> short_compat(
        compat.begin(), compat.begin() + 70);
    require(
        !capsid::parse_ready_proof(
            short_compat, &out_compat, &proof, &error),
        "short compat id was accepted");
    std::vector<uint8_t> bad_ns = payload;
    // Corrupt the namespace length field (first u16 after the three u32s).
    bad_ns[71 + 12] = 0xff;
    bad_ns[71 + 13] = 0xff;
    require(
        !capsid::parse_ready_proof(
            bad_ns, &out_compat, &proof, &error),
        "oversized namespace identity was accepted");
}

void test_request_head_decoder() {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kRequestHead;
    frame.flags = 0;
    frame.request_id = 42;
    append_string16(&frame.payload, "POST");
    append_string32(
        &frame.payload, "https://example.test/path");
    capsid::protocol::append_u16(&frame.payload, 2);
    append_string16(&frame.payload, "x-first");
    append_string32(&frame.payload, "one");
    append_string16(&frame.payload, "set-cookie");
    append_string32(&frame.payload, "a=1");

    capsid::WorkerRequestHead decoded;
    std::string error;
    require(
        capsid::decode_worker_request_head(
            frame, 4096, &decoded, &error),
        error);
    require(
        decoded.method == "POST" &&
            decoded.url == "https://example.test/path" &&
            decoded.headers.size() == 2 &&
            decoded.headers[1].name == "set-cookie",
        "request head fields decoded incorrectly");

    for (size_t size = 0; size < frame.payload.size(); ++size) {
        capsid::protocol::Frame truncated = frame;
        truncated.payload.resize(size);
        require(
            !capsid::decode_worker_request_head(
                truncated, 4096, &decoded, &error),
            "truncated request head was accepted");
    }
    capsid::protocol::Frame trailing = frame;
    trailing.payload.push_back(0);
    require(
        !capsid::decode_worker_request_head(
            trailing, 4096, &decoded, &error),
        "request head trailing byte was accepted");
    frame.request_id = 0;
    require(
        !capsid::decode_worker_request_head(
            frame, 4096, &decoded, &error),
        "zero request id was accepted");

    // Bodyless (kFlagRequestEnd) head: decoded with bodyless=true.
    frame.request_id = 42;
    frame.flags = capsid::protocol::kFlagRequestEnd;
    require(
        capsid::decode_worker_request_head(
            frame, 4096, &decoded, &error) &&
            decoded.bodyless,
        "bodyless flag was not exposed by the decoder");
    // Unknown flags are rejected.
    frame.flags = capsid::protocol::kFlagRequestEnd | 0x20u;
    require(
        !capsid::decode_worker_request_head(
            frame, 4096, &decoded, &error),
        "unknown request head flags were accepted");
}

void test_response_header_decoder() {
    std::vector<uint8_t> payload;
    capsid::protocol::append_u16(&payload, 2);
    append_string16(&payload, "x-first");
    append_string32(&payload, "one");
    append_string16(&payload, "set-cookie");
    append_string32(&payload, "a=1");

    size_t count = 0;
    capsid::ResponseHeaderView header;
    require(
        capsid::decode_response_headers(
            &payload[0], payload.size(), 1, &count, &header),
        "valid response headers were rejected");
    require(
        count == 2 &&
            std::string(
                reinterpret_cast<const char *>(header.name),
                header.name_size) == "set-cookie" &&
            std::string(
                reinterpret_cast<const char *>(header.value),
                header.value_size) == "a=1",
        "response header fields decoded incorrectly");

    for (size_t size = 0; size < payload.size(); ++size) {
        require(
            !capsid::decode_response_headers(
                &payload[0], size, 0, NULL, NULL),
            "truncated response header block was accepted");
    }

    payload.push_back(0);
    std::memset(&header, 0x5a, sizeof(header));
    require(
        !capsid::decode_response_headers(
            &payload[0], payload.size(), 0, &count, &header),
        "response header trailing byte was accepted");
    require(
        header.name == NULL && header.value == NULL &&
            header.name_size == 0 && header.value_size == 0,
        "failed header decode leaked partial output");
}

}  // namespace

// A v2 frame (kVersion=2 in the header) must be rejected by the v3 parser.
void test_protocol_version_rejection() {
    std::vector<uint8_t> wire;
    capsid::protocol::append_u32(&wire, capsid::protocol::kMagic);
    capsid::protocol::append_u16(&wire, 2);  // stale v2 version
    capsid::protocol::append_u16(&wire, capsid::protocol::kHello);
    capsid::protocol::append_u32(&wire, 0);  // flags
    capsid::protocol::append_u64(&wire, 0);  // request_id
    capsid::protocol::append_u32(&wire, 0);  // payload size

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    std::vector<uint8_t> payload;
    require(parser.append(wire.data(), wire.size()), "append failed");
    require(
        parser.next(&frame, &payload) == capsid::protocol::kParseError,
        "stale v2 frame was accepted by the v3 parser");
}

int main() {
    test_hello_and_bundle_state_machine();
    test_binding_startup_state_machine();
    test_ready_proof_payload();
    test_request_head_decoder();
    test_response_header_decoder();
    test_protocol_version_rejection();
    return 0;
}
