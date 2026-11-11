// Exercise bounded per-worker Binding quota, explicit host cancellation and recovery.
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
#include <set>

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
    require(argc==4,"usage: worker-quota-probe <worker> <binding-source> <user-source>");
    const std::string base="http://127.0.0.1:1";
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

    std::set<std::uint64_t> canceled;
    const auto log_event=[](const capsid_event& event) {
        const std::string text(reinterpret_cast<const char*>(event.payload.data),event.payload.size);
        const char* digits="0123456789abcdef";std::string hex;hex.reserve(text.size()*2);
        for(const unsigned char value:text){hex.push_back(digits[value>>4]);hex.push_back(digits[value&15]);}
        std::cout<<"QUOTA_LOG "<<event.request_id<<" "<<hex<<std::endl;return text;
    };
    const auto tolerate_canceled=[&canceled](const capsid_event& event) {
        if((event.type==CAPSID_EVENT_RESPONSE_HEAD||event.type==CAPSID_EVENT_RESPONSE_BODY||event.type==CAPSID_EVENT_RESPONSE_END)&&canceled.count(event.request_id)){
            require(event.type!=CAPSID_EVENT_RESPONSE_BODY||event.payload.size==0,"no body leak from canceled held request");
            std::cout<<"QUOTA_CANCELED_EVENT "<<event.request_id<<" "<<event.type<<std::endl;return true;
        }
        return false;
    };
    const auto begin=[worker](std::uint64_t id,const std::string& step,const std::string& variant) {
        const std::string url="https://binding.test/quota?step="+step+"&variant="+variant+"&parent="+std::to_string(id);
        require_result(capsid_worker_begin_bodyless_request(worker,id,"GET",url.c_str(),nullptr,0),"begin concurrent request");
        std::cout<<"QUOTA_REQUEST "<<id<<" "<<step<<" "<<variant<<std::endl;
    };
    const auto wait_log=[&](const std::string& marker) {
        receive_until(worker,[&](const capsid_event& event){
            if(event.type==CAPSID_EVENT_LOG)return log_event(event).find(marker)!=std::string::npos;
            if(tolerate_canceled(event))return false;
            require(event.type!=CAPSID_EVENT_RESPONSE_HEAD&&event.type!=CAPSID_EVENT_RESPONSE_BODY&&event.type!=CAPSID_EVENT_RESPONSE_END,"held requests remain without a response");return false;
        });
    };
    const auto response=[&](std::uint64_t id) {
        std::string body;
        receive_until(worker,[&](const capsid_event& event){
            if(event.type==CAPSID_EVENT_LOG){log_event(event);return false;}
            if(tolerate_canceled(event))return false;
            if(event.type==CAPSID_EVENT_RESPONSE_HEAD||event.type==CAPSID_EVENT_RESPONSE_BODY||event.type==CAPSID_EVENT_RESPONSE_END)require(event.request_id==id,"response belongs to current finite request");
            if(event.type==CAPSID_EVENT_RESPONSE_BODY){body.append(reinterpret_cast<const char*>(event.payload.data),event.payload.size);require(event.payload.size<=UINT32_MAX,"response credit bound");require_result(capsid_worker_grant_response_credit(worker,id,static_cast<std::uint32_t>(event.payload.size)),"response credit");}
            return event.type==CAPSID_EVENT_RESPONSE_END;
        });std::cout<<"QUOTA_RESPONSE "<<id<<" "<<body<<std::endl;
    };
    for(std::uint64_t id=1;id<=16;++id){begin(id,"hold","none");wait_log("quota-ready:"+std::to_string(id)+":64");}
    const std::vector<std::string> variants={"plain","symbol","cycle","getter"};
    for(std::size_t index=0;index<variants.size();++index){const std::uint64_t id=17+index;begin(id,"overflow",variants[index]);response(id);}
    const auto cancel=[&](std::uint64_t id){canceled.insert(id);require_result(capsid_worker_cancel(worker,id),"explicit host cancel");std::cout<<"QUOTA_CANCEL "<<id<<std::endl;wait_log("quota-aborted:"+std::to_string(id)+":64");};
    cancel(1);
    begin(21,"hold","recovery");wait_log("quota-ready:21:64");
    begin(22,"overflow","plain");response(22);
    for(std::uint64_t id=2;id<=16;++id)cancel(id);
    cancel(21);
    begin(23,"final","none");response(23);
    std::string finish;std::getline(std::cin,finish);require(finish=="finish","parent observed complete worker quota cleanup");
    const auto exit=capsid_test::shutdown_and_wait(worker,2500);require(exit.clean(),"natural quota worker exit");capsid_worker_destroy(worker);
    std::cout<<"QUOTA_EXIT {\"reaped\":true,\"channelClosed\":true,\"code\":0}"<<std::endl;return 0;
}
