#include "capsid/runtime.h"
#include "win32_compat.h"
#include "graceful_worker_exit.h"
#include "worker_exit_status.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>

// Existing response probes retain their five-second event deadline.
// The composed owner probe also awaits an independent three-second source gate.
#ifndef CAPSID_TEST_FETCH_EVENT_SECONDS
#define CAPSID_TEST_FETCH_EVENT_SECONDS 5
#endif
static_assert(CAPSID_TEST_FETCH_EVENT_SECONDS == 5 || CAPSID_TEST_FETCH_EVENT_SECONDS == 10, "unsupported fetch probe event deadline");

namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void ok(capsid_result value,const char* message){check(value==CAPSID_OK,message);}
struct Peer {
    capsid_worker* worker=nullptr;
    ~Peer(){capsid_worker_destroy(worker);}
    capsid_event next(){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(CAPSID_TEST_FETCH_EVENT_SECONDS);
        for(;;){
            const auto flushed=capsid_worker_flush(worker);check(flushed==CAPSID_OK||flushed==CAPSID_WOULD_BLOCK,"flush");
            capsid_event event{};event.struct_size=sizeof(event);const auto result=capsid_worker_next_event(worker,&event);
            if(result==CAPSID_OK){
                if(event.type==CAPSID_EVENT_ERROR)std::fprintf(stderr,"worker error: %.*s\n",int(event.payload.size),event.payload.data);
                check(event.type!=CAPSID_EVENT_ERROR&&event.type!=CAPSID_EVENT_EXIT,"healthy worker");
                if(event.type==CAPSID_EVENT_AUDIT||event.type==CAPSID_EVENT_LOG||event.type==CAPSID_EVENT_MEMORY_METRICS){
                    std::printf("LIMIT_AUX type=%d request=%llu status=%u payload=%.*s\n",int(event.type),static_cast<unsigned long long>(event.request_id),event.status,int(event.payload.size),event.payload.data);
                    std::fflush(stdout);continue;
                }
                return event;
            }
            check(result==CAPSID_WOULD_BLOCK&&std::chrono::steady_clock::now()<deadline,"event deadline");
            capsid_pollfd fd{};fd.fd=capsid_worker_fd(worker);fd.events=POLLIN|(flushed==CAPSID_WOULD_BLOCK?POLLOUT:0);
            capsid::win32::capsid_poll(&fd,1,10);
        }
    }
    std::string response(uint64_t id){
        std::string text;bool head=false;
        for(;;){const auto event=next();check(event.request_id==id,"HTTP owner");
            if(event.type==CAPSID_EVENT_RESPONSE_HEAD){check(event.status==200,"HTTP status");head=true;}
            else if(event.type==CAPSID_EVENT_RESPONSE_BODY){
                text.append(reinterpret_cast<const char*>(event.payload.data),event.payload.size);
                if(event.payload.size)ok(capsid_worker_grant_response_credit(worker,event.request_id,
                    static_cast<uint32_t>(event.payload.size)),"response credit");
            }
            else if(event.type==CAPSID_EVENT_RESPONSE_END){check(head,"complete response");return text;}
            else if(event.type==CAPSID_EVENT_REQUEST_CREDIT){check(event.credit>0,"positive request credit");}
            else{std::fprintf(stderr,"unexpected event type=%d request=%llu flags=%u credit=%u\n",int(event.type),static_cast<unsigned long long>(event.request_id),event.flags,event.credit);check(false,"ordinary native HTTP does not emit a routed channel event");}
        }
    }
};
}
int main(int argc,char** argv){
    try{
        check(argc==6,"worker, fixture, target, limit, view");
        char* end=nullptr;const auto limit=std::strtoull(argv[4],&end,10);check(end&&!*end,"integer limit");
        std::ifstream input(argv[2],std::ios::binary);check(input.is_open(),"fixture opened");
        const std::string bundle((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        capsid_worker_config config;capsid_worker_config_init(&config);config.worker_path=argv[1];config.max_fetch_response_body_bytes=limit;
        capsid_egress_rule rule;capsid_egress_rule_init(&rule);rule.action=CAPSID_EGRESS_ALLOW;rule.target="localhost";
        capsid_egress_policy policy;capsid_egress_policy_init(&policy);policy.rules=&rule;policy.rule_count=1;config.egress_policy=&policy;
        capsid_protocol_options options;capsid_protocol_options_init(&options,sizeof(options));options.wire_version=4;options.capabilities=123;options.max_channels=1;
        Peer peer;ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"configured native worker spawn");
        ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t*>(bundle.data()),bundle.size()),"bundle load");check(peer.next().type==CAPSID_EVENT_READY,"READY");
        const std::string url=std::string("http://app.test/probe?target=")+argv[3]+"&view="+argv[5];
        ok(capsid_worker_begin_bodyless_request(peer.worker,1,"GET",url.c_str(),nullptr,0),"native probe request");
        const auto result=peer.response(1);std::printf("LIMIT_PROBE %s\n",result.c_str());std::fflush(stdout);
        std::string acknowledgment;check(static_cast<bool>(std::getline(std::cin,acknowledgment))&&acknowledgment=="finish","source EOF observed before host shutdown");
        ok(capsid_worker_begin_bodyless_request(peer.worker,2,"GET","http://app.test/healthy",nullptr,0),"healthy request");check(peer.response(2)=="healthy","one-slot healthy reuse");
        const auto exited=capsid_test::shutdown_and_wait(peer.worker,2500);const auto cleanup=capsid::destroy_worker_with_status(peer.worker);peer.worker=nullptr;
        std::printf("LIMIT_EXIT {\"clean\":%s,\"processObserved\":%s,\"exitStatus\":%d,\"forced\":%s,\"errorEvents\":%u}\n",exited.clean()?"true":"false",exited.reaped?"true":"false",exited.status,cleanup.destroy_forced?"true":"false",exited.error_events);
        check(exited.clean()&&!cleanup.destroy_forced,"natural OS exit");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
