#include "host/worker_executor.h"
#include "host/protocol_launch.h"
#include "host/channel_ledger.h"
#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
void require(bool value,const char *message) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1); }
}
void denied_dial(const char *worker_path,int scenario) {
    namespace asio=boost::asio;
    using tcp=asio::ip::tcp;
    using capsid::host::WorkerEvent;
    asio::io_context io;
    tcp::acceptor upstream(io,tcp::endpoint(asio::ip::make_address("127.0.0.1"),0));
    const uint16_t port=upstream.local_endpoint().port();upstream.non_blocking(true);
    char hostname[]="localhost";
    capsid_egress_rule rules[2];for(auto &rule:rules)capsid_egress_rule_init(&rule);
    rules[0].action=CAPSID_EGRESS_ALLOW;rules[0].target=hostname;rules[0].port_start=rules[0].port_end=port;
    rules[1].action=CAPSID_EGRESS_DENY;rules[1].target="127.0.0.0/8";
    capsid_egress_policy policy;capsid_egress_policy_init(&policy);policy.rules=rules;policy.rule_count=scenario==0 ? 2 : 1;
    if(scenario==2) { policy.rules=nullptr;policy.rule_count=0;policy.default_action=CAPSID_EGRESS_ALLOW; }
    capsid_capability_policy capability;capsid_capability_policy_init(&capability);
    capsid_worker_config config;capsid_worker_config_init(&config);config.worker_path=worker_path;config.egress_policy=&policy;
    if(scenario==1)config.capability_policy=&capability; // enabled, no net grant: overrides legacy egress
    const std::string target="ws://"+std::string(scenario==2 ? "127.0.0.1" : "localhost")+":"+std::to_string(port)+"/";
    const std::string source="export default {async fetch(){return new Response(await new Promise(resolve=>{const ws=new WebSocket('"+target+"');const events=[];ws.onopen=()=>events.push('open');ws.onerror=()=>events.push('error');ws.onclose=e=>resolve(JSON.stringify({events,code:e.code,clean:e.wasClean}))}))}}";
    capsid::host::WorkerExecutor executor;
    capsid::host::WorkerExecutor::WorkerFactory factory=[&](capsid_worker **out,std::string *) {
        capsid_worker *worker=nullptr;capsid::host::ProtocolLaunch launch;
        if(capsid::host::spawn_with_protocol(&config,launch,&worker)!=CAPSID_OK)return false;
        if(capsid_worker_load_bundle(worker,reinterpret_cast<const uint8_t *>(source.data()),source.size())!=CAPSID_OK || capsid_worker_flush(worker)!=CAPSID_OK) {
            capsid_worker_destroy(worker);return false;
        }
        *out=worker;return true;
    };
    const auto budget=capsid::host::default_channel_budget();
    const auto initial_channels=budget->channels(),initial_dials=budget->dials(),initial_bytes=budget->queued_bytes();
    std::string error;const bool ready=executor.start(factory,&error);require(ready,error.c_str());
    hostname[0]='X';rules[1].action=CAPSID_EGRESS_ALLOW; // caller mutation must not widen the spawn snapshot
    capsid::host::Command begin;begin.type=capsid::host::CommandType::kBeginRequest;begin.request_id=1;
    begin.method="GET";begin.url="https://app.test/";begin.end_request=true;executor.submit(std::move(begin));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    std::string body;bool complete=false;
    while(!complete) {
        require(std::chrono::steady_clock::now()<deadline && !executor.exited(),"denial completes promptly without poisoning worker");
        for(auto &event:executor.drain_events()) {
            require(event.type!=WorkerEvent::Type::kExit && event.type!=WorkerEvent::Type::kError,"healthy denied dial");
            if(event.type==WorkerEvent::Type::kResponseHead) require(event.status==200,"failure delivered to JS, not HTTP 500");
            if(event.type==WorkerEvent::Type::kResponseBody) body.append(event.body.begin(),event.body.end());
            if(event.type==WorkerEvent::Type::kResponseEnd)complete=true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(body.find("\"events\":[\"error\"]")!=std::string::npos && body.find("\"code\":1006")!=std::string::npos && body.find("\"clean\":false")!=std::string::npos,"denied dial emits error and abnormal close without open");
    tcp::socket accepted(io);boost::system::error_code accept_error;upstream.accept(accepted,accept_error);
    require(accept_error==asio::error::would_block || accept_error==asio::error::try_again,"no TCP connection to denied address");
    executor.stop_and_join();
    require(budget->channels()==initial_channels && budget->dials()==initial_dials && budget->queued_bytes()==initial_bytes,"dial/socket/queue reservations returned after stop");
}
}
int main(int argc,char **argv) {
    require(argc==2,"worker path required");
    for(int scenario=0;scenario<3;++scenario)denied_dial(argv[1],scenario);
    std::puts("host outbound: immutable User authority, resolved CIDR deny, capability precedence and protected IP PASS");
}
