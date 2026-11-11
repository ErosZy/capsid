// Compare the Host's READY expectation with a real Binding Worker under
// explicit zero/nonzero process-memory limits, independently of build defaults.
#include "capsid/runtime.h"
#include "host/binding_compile.h"
#include "win32_compat.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void require_result(capsid_result result, const char* operation) {
    require(result == CAPSID_OK,
            std::string(operation) + ": " + capsid_result_string(result));
}

template <class Consume>
void receive_until(capsid_worker* worker, Consume consume) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        require(flush == CAPSID_OK || flush == CAPSID_WOULD_BLOCK,
                "Worker flush failed");
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            require(event.type != CAPSID_EVENT_EXIT, "Worker exited early");
            if (event.type == CAPSID_EVENT_ERROR) {
                require(false, "Worker error: " + std::string(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size));
            }
            if (consume(event)) {
                return;
            }
            continue;
        }
        require(result == CAPSID_WOULD_BLOCK, "Worker event read failed");
        capsid_pollfd fd = {};
        fd.fd = capsid_worker_fd(worker);
        fd.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        require(capsid::win32::capsid_poll(&fd, 1, 50) >= 0,
                "Worker poll failed");
    }
    require(false, "Worker event deadline");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 3, "usage: test-worker-binding-ready <worker> <zero|finite>");
    const std::string mode = argv[2];
    require(mode == "zero" || mode == "finite", "unknown limit mode");
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.process_memory_limit = mode == "zero" ? 0 : UINT64_C(2147483648);
    config.strict_sandbox = 0;

    capsid::host::EffectiveBinding binding;
    binding.id = "echo";
    binding.package.source =
        "export default () => ({ echo(value) { return 'ready:' + value; } });";
    binding.request.config_json = "{}";
    const std::vector<capsid::host::EffectiveBinding> bindings = {binding};
    capsid_worker* worker = nullptr;
    require_result(capsid_worker_spawn(&config, &worker), "spawn");
    std::string error;
    const bool loaded = capsid::host::load_effective_bindings_into_worker(
        worker, bindings, &error);
    require(loaded, "load Binding: " + error);
    const std::string source =
        "import echo from 'capsid:binding/echo';"
        "export default { async fetch() {"
        " return new Response(await echo.echo('ok')); } };";
    require_result(capsid_worker_load_bundle_named(
        worker, reinterpret_cast<const std::uint8_t*>(source.data()),
        source.size(), "file:///binding-ready.js"), "load application");

    std::vector<std::uint8_t> payload;
    receive_until(worker, [&payload](const capsid_event& event) {
        if (event.type != CAPSID_EVENT_READY) {
            return false;
        }
        payload.assign(event.payload.data, event.payload.data + event.payload.size);
        return true;
    });
    capsid_build_info info;
    capsid_build_info_init(&info);
    require_result(capsid_runtime_build_info(&info), "build identity");
    capsid::WorkerReadyProof actual;
    std::string compatibility;
    require(capsid::parse_ready_proof(payload, &compatibility, &actual, &error),
            "parse real READY");
    auto expected = capsid::host::expected_worker_ready_proof(bindings, config);
    std::cout << "memoryLimit=" << config.process_memory_limit
              << " actualFeatures=" << actual.applied_feature_bits
              << " expectedFeatures=" << expected.applied_feature_bits
              << std::endl;
    const bool verified = capsid::host::verify_worker_ready(
        payload, info.compatibility_id, expected, &error);
    require(verified, "actual READY rejected: " + error);

    // The opposite memory-limit feature remains a mismatch, including when
    // the expected feature mask is zero. Other proof fields stay exact.
    expected.applied_feature_bits ^= CAPSID_SANDBOX_FEATURE_RLIMITS;
    const bool accepted_mismatch = capsid::host::verify_worker_ready(
        payload, info.compatibility_id, expected, &error);
    require(!accepted_mismatch && error.find("features") != std::string::npos,
            "opposite resource-feature proof was accepted");

    require_result(capsid_worker_begin_bodyless_request(
        worker, 1, "GET", "https://example.test/", nullptr, 0), "request");
    std::string body;
    receive_until(worker, [worker, &body](const capsid_event& event) {
        if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
            body.append(reinterpret_cast<const char*>(event.payload.data),
                        event.payload.size);
            require(event.payload.size <= UINT32_MAX, "response credit overflow");
            require_result(capsid_worker_grant_response_credit(
                worker, event.request_id,
                static_cast<std::uint32_t>(event.payload.size)), "response credit");
        }
        return event.type == CAPSID_EVENT_RESPONSE_END;
    });
    require(body == "ready:ok", "actual Binding response differs: " + body);
    require_result(capsid_worker_shutdown(worker), "shutdown");
    capsid_worker_destroy(worker);
    std::cout << "PASS binding READY " << mode << std::endl;
    return 0;
}
