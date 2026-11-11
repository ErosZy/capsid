// Run actual Binding fetch/RPC observations using supplied Binding and User modules.
// Preserve the existing15s event deadline, default JS heap and real READY proof.
#include "capsid/runtime.h"
#include "host/binding_compile.h"
#include "win32_compat.h"
#include "graceful_worker_exit.h"
#include <fstream>
#include <iterator>

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
            if(event.type==CAPSID_EVENT_LOG)std::cerr<<"WORKER_LOG "<<std::string(reinterpret_cast<const char*>(event.payload.data),event.payload.size)<<std::endl;
            if(event.type==CAPSID_EVENT_EXIT){const auto exit=capsid_test::wait_for_worker_exit(worker,2500,false);std::cerr<<"EARLY_EXIT reaped="<<exit.reaped<<" status="<<exit.status<<" closed="<<exit.channel_closed<<std::endl;require(false,"Worker exited early");}
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
    require(argc==6,"usage: binding-body-probe <worker> <binding-source> <user-source> <http-base> <view-group>");
    const std::string base=argv[4];require(base.rfind("http://127.0.0.1:",0)==0,"local actual source");
    capsid_worker_config config;capsid_worker_config_init(&config);config.worker_path=argv[1];config.strict_sandbox=0;config.process_memory_limit=0;
    std::ifstream input(argv[2],std::ios::binary);require(bool(input),"open Binding source");
    capsid::host::EffectiveBinding binding;binding.id="echo";binding.package.source=std::string(std::istreambuf_iterator<char>(input),{});
    binding.request.config_json="{}";binding.request.net_rules={base.substr(7)};binding.profiles={"network-client"};
    const std::vector<capsid::host::EffectiveBinding> bindings={binding};
    capsid_worker* worker = nullptr;
    require_result(capsid_worker_spawn(&config, &worker), "spawn");
    std::string error;
    const bool loaded = capsid::host::load_effective_bindings_into_worker(
        worker, bindings, &error);
    require(loaded, "load Binding: " + error);
    std::ifstream user_input(argv[3],std::ios::binary);require(bool(user_input),"open User source");
    const std::string source(std::istreambuf_iterator<char>(user_input),{});
    require(!source.empty(),"nonempty actual User source");
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

    for (std::uint64_t request_id=1;request_id<=2;++request_id) {
        const std::string phase=request_id==1?"open":"read";
        const std::string request_url="https://binding.test/probe?target="+base+"/body&views="+argv[5]+"&phase="+phase;
        require_result(capsid_worker_begin_bodyless_request(worker,request_id,"GET",request_url.c_str(),nullptr,0),"request");
        std::cout<<"BINDING_REQUEST "<<request_id<<std::endl;
        std::string body;
        receive_until(worker,[worker,request_id,&body](const capsid_event& event) {
            if (event.type==CAPSID_EVENT_RESPONSE_BODY) {
                require(event.request_id==request_id,"response belongs to current request");
                body.append(reinterpret_cast<const char*>(event.payload.data),event.payload.size);
                require(event.payload.size<=UINT32_MAX,"response credit overflow");
                require_result(capsid_worker_grant_response_credit(worker,event.request_id,static_cast<std::uint32_t>(event.payload.size)),"response credit");
            }
            if(event.type==CAPSID_EVENT_RESPONSE_END)require(event.request_id==request_id,"terminal belongs to current request");
            return event.type==CAPSID_EVENT_RESPONSE_END;
        });
        std::cout<<"BINDING_PHASE "<<request_id<<" "<<body<<std::endl;
        std::string stage;std::getline(std::cin,stage);require(stage==(request_id==1?"continue":"finish"),"parent acknowledged observed held source/cleanup boundary");
    }
    const auto exit=capsid_test::shutdown_and_wait(worker,2500);require(exit.clean(),"real natural Binding worker exit");capsid_worker_destroy(worker);
    std::cout<<"BINDING_EXIT {\"reaped\":true,\"channelClosed\":true,\"code\":0}"<<std::endl;
    return 0;
}
