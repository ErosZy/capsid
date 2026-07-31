#include "ipc_validation.h"
#include "protocol.h"
#include "response_headers.h"
#include "capsid/runtime.h"

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

int main() {
    test_hello_and_bundle_state_machine();
    test_request_head_decoder();
    test_response_header_decoder();
    return 0;
}
