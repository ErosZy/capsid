#include "capsid/runtime.h"
#include "client_channel_state.h"
#include "win32_compat.h"
#include "worker_exit_assertions.h"
#include "graceful_worker_exit.h"
#include "worker_writer_receipt.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>
#include <thread>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {
void check(bool value,const char *message) {
    if(!value) {
        const char* error=capsid_last_error();
        throw std::runtime_error(std::string(message)+(error ? std::string(" (")+error+")" : ""));
    }
}
void ok(capsid_result result,const char *message) { check(result==CAPSID_OK,message); }
struct ReceiptCleanup {
    std::string path;
    ~ReceiptCleanup() { if (!path.empty()) std::remove(path.c_str()); }
};
void await_writer_pressure(const std::string& path,int64_t pid,uint64_t dial) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for (;;) {
        std::ifstream file(path,std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        if (!bytes.empty() && bytes.back()=='\n') {
            std::istringstream input(bytes);std::string magic,extra;
            unsigned long long owner=0,actual_dial=0,queued=0,limit=0,stalls=0,blocked=0;
            check(bool(input>>magic>>owner>>actual_dial>>queued>>limit>>stalls>>blocked),"complete actual writer receipt");
            check(!(input>>extra) && magic=="WRITER_PRESSURE" && owner==static_cast<unsigned long long>(pid) &&
                  actual_dial==dial && queued==4096 && limit==4096 && stalls && blocked,
                  "owned child actually observed ACCEPT, a full queue and a physical write stall");
            std::printf("WRITER_BARRIER pid=%llu dial=%llu queue=%llu limit=%llu stalls=%llu blocked=%llu\n",
                owner,actual_dial,queued,limit,stalls,blocked);
            return;
        }
        if (std::chrono::steady_clock::now()>=deadline) {
            std::fprintf(stderr,"WRITER_RECEIPT_DEADLINE path=%s bytes=%zu good=%d errno=%d\n",path.c_str(),bytes.size(),int(file.good()),errno);
            check(false,"actual blocked ACK and stalled writer receipt deadline");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Peer {
    capsid_worker *worker=nullptr;
    ~Peer() { capsid_worker_destroy(worker); }
    capsid_event next() {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for(;;) {
            auto flushed=capsid_worker_flush(worker); check(flushed==CAPSID_OK || flushed==CAPSID_WOULD_BLOCK,"flush");
            capsid_event event{};event.struct_size=sizeof(event);
            auto result=capsid_worker_next_event(worker,&event);
            if(result==CAPSID_OK) {
                if(event.type==CAPSID_EVENT_ERROR) std::fprintf(stderr,"worker error: %.*s\n",int(event.payload.size),event.payload.data);
                check(event.type!=CAPSID_EVENT_ERROR && event.type!=CAPSID_EVENT_EXIT,"healthy worker"); return event;
            }
            check(result==CAPSID_WOULD_BLOCK && std::chrono::steady_clock::now()<deadline,"event deadline");
            capsid_pollfd fd{}; fd.fd=capsid_worker_fd(worker);fd.events=POLLIN | (flushed==CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&fd,1,10);
        }
    }
    capsid_channel_event channel(capsid_event_type expected) {
        auto event=next();check(event.type==expected,"channel event order");
        capsid_channel_event view;capsid_channel_event_init(&view);
        ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"typed channel decode");return view;
    }
    void begin(uint64_t request,const char *path) {
        const std::string url=std::string("https://app.test")+path;
        ok(capsid_worker_begin_bodyless_request(worker,request,"GET",url.c_str(),nullptr,0),"HTTP begin");
    }
    std::string response(uint64_t id) {
        std::string text; bool head=false;
        for(;;) {
            auto event=next();check(event.request_id==id,"response belongs to original HTTP owner");
            if(event.type==CAPSID_EVENT_RESPONSE_HEAD) { check(event.status==200,"HTTP success");head=true; }
            else if(event.type==CAPSID_EVENT_RESPONSE_BODY) text.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);
            else if(event.type==CAPSID_EVENT_RESPONSE_END) { check(head,"complete response");return text; }
            else check(false,"no unexpected event in response");
        }
    }
};
constexpr char bundle[]=R"JS(
let orphanCallbacks=0;
const orphanSockets=[];
const headWaiters=[];
let pressureSocket=null,pressureCloseRequested=false;
export default {async fetch(request) {
    const path=new URL(request.url).pathname;
    if(path==='/healthy')return new Response('healthy');
    if(path==='/filler')return new Response('f'.repeat(1024*1024));
    if(path==='/pressure-close') {
        if(!pressureSocket)return new Response('incorrect-pressure-state');
        pressureCloseRequested=true;
        return new Response('close-requested');
    }
    if(path==='/barrier-size')return new Response(String(headWaiters.length));
    if(path==='/barrier-response')return new Promise(resolve=>headWaiters.push({resolve,kind:new URL(request.url).searchParams.get('kind')}));
    if(path==='/barrier-release') {
        const count=headWaiters.length;
        for(let i=0;i<count;i++) {
            const {resolve,kind}=headWaiters[i];
            const body=kind==='empty' ? null : 'body:'+i+':'+ 'q'.repeat(kind==='fixed' ? 64 : 8192);
            resolve(kind==='stream' ? new Response(new ReadableStream({start(c){c.enqueue(new TextEncoder().encode(body));c.close();}})) : new Response(body));
        }
        headWaiters.length=0;
        return new Response('released:'+count);
    }
    if(path==='/oversized') { try {new WebSocket('ws://example.test/'+ 'x'.repeat(8192));return new Response('accepted')} catch(e) {return new Response(e.name)} }
    if(path==='/orphan') {
        const socket=new WebSocket('wss://example.test/socket',['chat']);
        orphanSockets.push(socket);
        for(const type of ['open','message','error','close'])socket.addEventListener(type,()=>{orphanCallbacks++});
        return new Response('orphan');
    }
    if(path==='/orphan-status') {
        await new Promise(resolve=>setTimeout(resolve,25));
        return Response.json({callbacks:orphanCallbacks,retained:orphanSockets.length});
    }
    return Response.json(await new Promise(resolve=>{
        const events=[];const ws=new WebSocket('wss://example.test/socket',['chat']);
        const initial=ws.readyState;let early=false;try{ws.send('early')}catch{early=true}
        ws.onopen=()=>{events.push('open');ws.send('hello')};
        ws.onmessage=e=>{events.push(e.data);ws.close(1000,'done')};
        ws.onerror=()=>events.push('error');
        ws.onclose=e=>{events.push('close');resolve({initial,early,events,code:e.code,clean:e.wasClean,state:ws.readyState,protocol:ws.protocol})};
        if(path==='/cancel')ws.close();
        if(path==='/pressure') {
            pressureSocket=ws;
            // Preserve the original HTTP owner's timer context. A different
            // HTTP request signals the barrier without operating on its socket.
            const closeWhenObserved=()=>{
                if(pressureCloseRequested)ws.close();
                else setTimeout(closeWhenObserved,1);
            };
            setTimeout(closeWhenObserved,1);
        }
    }));
}};
)JS";
void orphaned_http_dials(const char *worker_path) {
    capsid_worker_config config;capsid_worker_config_init(&config);config.worker_path=worker_path;
    capsid_protocol_options options;capsid_protocol_options_init(&options,sizeof(options));
    options.wire_version=4;options.capabilities=27;options.max_channels=2;
    Peer peer;ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"orphan spawn");
    ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t *>(bundle),std::strlen(bundle)),"orphan load");
    check(peer.next().type==CAPSID_EVENT_READY,"orphan READY");
    uint64_t last_dial=0;uint32_t assigned=0;
    for(uint64_t round=0;round<20;++round) {
        const uint64_t request=100+round;
        peer.begin(request,"/orphan");
        bool complete=false,cancelled=false,result_ack=false;uint64_t dial=0;std::string body;
        while(!complete || !cancelled) {
            auto event=peer.next();
            if(event.type>=CAPSID_EVENT_CHANNEL_DIAL && event.type<=CAPSID_EVENT_CHANNEL_ACK) {
                capsid_channel_event view;capsid_channel_event_init(&view);
                ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"orphan channel decode");
                if(event.type==CAPSID_EVENT_CHANNEL_DIAL) {
                    check(!dial && view.dial_id>last_dial,"one monotonic orphan dial");
                    dial=view.dial_id;last_dial=dial;
                } else if(event.type==CAPSID_EVENT_CHANNEL_CANCEL) {
                    check(complete && dial && !cancelled,"HTTP completes before one cancellation barrier");
                    if(view.channel_id) {
                        check(result_ack && view.channel_id==assigned && !view.dial_id,"crossed result cancels assigned identity");
                        ok(capsid_worker_channel_confirm(peer.worker,view.channel_id,0,1),"orphan assigned cancel ACK");
                    } else {
                        check(view.dial_id==dial && !result_ack,"pending cancel retains original dial identity");
                        ok(capsid_worker_channel_confirm(peer.worker,0,dial,1),"orphan pending cancel ACK");
                    }
                    cancelled=true;
                } else if(event.type==CAPSID_EVENT_CHANNEL_ACK) {
                    check(!result_ack && view.dial_id==dial && view.channel_id==assigned,"orphan result ACK precedes assigned cancel");
                    result_ack=true;
                } else check(false,"abandoned constructor emits no DATA or Close");
            } else {
                check(event.request_id==request,"orphan HTTP owner identity");
                if(event.type==CAPSID_EVENT_RESPONSE_HEAD)check(event.status==200,"orphan HTTP head");
                else if(event.type==CAPSID_EVENT_RESPONSE_BODY)body.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);
                else if(event.type==CAPSID_EVENT_RESPONSE_END) {
                    check(body=="orphan" && dial,"orphan HTTP response is complete");complete=true;
                    if(round%2) {
                        ++assigned;
                        ok(capsid_worker_dial_accept(peer.worker,dial,assigned,1024*1024,"chat"),"ACCEPT crosses an ended HTTP owner");
                    }
                } else check(false,"orphan HTTP event kind");
            }
        }
    }
    peer.begin(200,"/orphan-status");
    check(peer.response(200)=="{\"callbacks\":0,\"retained\":20}","retained sockets receive no callbacks and pending capacity is reclaimed");
    peer.begin(201,"/healthy");check(peer.response(201)=="healthy","orphan cleanup leaves HTTP healthy");
    std::puts("orphan dials: 20 HTTP terminals, crossed results, two-slot reuse and no callbacks PASS");
}
void response_head_pressure(const char *worker_path, uint16_t version) {
    capsid_worker_config config; capsid_worker_config_init(&config);
    config.worker_path=worker_path; config.max_queued_bytes=4096; config.request_timeout_ms=10000;
    capsid_protocol_options options; capsid_protocol_options_init(&options,sizeof(options));
    options.wire_version=version; options.capabilities=version==4 ? 27 : 0;
    Peer peer; ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"head pressure spawn");
    ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t *>(bundle),std::strlen(bundle)),"head pressure load");
    check(peer.next().type==CAPSID_EVENT_READY,"head pressure READY");
    const char *kinds[]={"large","fixed","empty","stream"};
    for(uint64_t id=1;id<=16;++id) {
        const std::string target=std::string("/barrier-response?kind=")+kinds[(id-1)%4];
        peer.begin(id,target.c_str());
    }
    peer.begin(50,"/barrier-size");
    check(peer.response(50)=="16","all sixteen suspended HTTP owners reached the release barrier");
    // All handlers settle in one real job checkpoint. The 4 KiB wire queue
    // cannot hold even the first 8 KiB body; later heads must wait, in order.
    peer.begin(51,"/barrier-release");
    std::string bodies[17]; bool heads[17]={}, ends[17]={}; unsigned completed=0;
    while(completed<17) {
        const auto event=peer.next();
        const size_t index=event.request_id==51 ? 16 : static_cast<size_t>(event.request_id-1);
        check(index<17,"pressure event belongs to a released HTTP owner");
        if(event.type==CAPSID_EVENT_RESPONSE_HEAD) {
            check(!heads[index] && !ends[index] && event.status==200,"one successful head before body");heads[index]=true;
            // Null-body responses use the ordinary head/end route; short
            // untouched text uses the explicit fixed-body representation.
            const bool fixed=index==16 || index%4==1;
            if(bool(event.flags & CAPSID_RESPONSE_HEAD_FLAG_FIXED_BODY)!=fixed)
                std::fprintf(stderr,"pressure head id=%llu flags=%u length=%u expected_fixed=%d\n",
                    static_cast<unsigned long long>(event.request_id),event.flags,event.credit,int(fixed));
            check(bool(event.flags & CAPSID_RESPONSE_HEAD_FLAG_FIXED_BODY)==fixed,
                  "fixed text and empty/streamed/large representations preserved under pressure");
        } else if(event.type==CAPSID_EVENT_RESPONSE_BODY) {
            check(heads[index] && !ends[index],"response body follows its own head");
            bodies[index].append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);
        } else if(event.type==CAPSID_EVENT_RESPONSE_END) {
            check(heads[index] && !ends[index],"one terminal after its own head");ends[index]=true;++completed;
        } else check(false,"no protocol error or cancellation under response head pressure");
    }
    for(size_t i=0;i<16;++i) {
        const std::string expected=i%4==2 ? "" : "body:"+std::to_string(i)+":"+std::string(i%4==1 ? 64 : 8192,'q');
        check(bodies[i]==expected,"all pressured response bytes and identities preserved");
    }
    check(bodies[16]=="released:16","release response completes under the same pressure");
    peer.begin(52,"/healthy");check(peer.response(52)=="healthy","worker remains healthy after all seventeen terminals");
    std::printf("response head pressure v%u: 16-owner progress barrier, 4096-byte wire queue, mixed large/fixed/empty/stream bodies and terminals PASS\n",unsigned(version));
}

void result_ack_pressure_case(const char *worker_path,unsigned delay_before_accept_ms) {
    std::puts("exercising observed ACCEPT before open under writer pressure");
    capsid_worker_config config;capsid_worker_config_init(&config);config.worker_path=worker_path;config.max_queued_bytes=4096;
    config.request_timeout_ms=10000;
    capsid_protocol_options options;capsid_protocol_options_init(&options,sizeof(options));options.wire_version=4;options.capabilities=27;
    ReceiptCleanup receipt;
    Peer peer;ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"pressure spawn");
    const int64_t worker_pid=capsid_worker_pid(peer.worker);check(worker_pid>0,"actual owned writer worker PID");
    receipt.path=capsid_test::writer_receipt_path(worker_pid);
    ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t *>(bundle),std::strlen(bundle)),"load");check(peer.next().type==CAPSID_EVENT_READY,"READY");
    peer.begin(50,"/oversized");check(peer.response(50)=="RangeError","oversized OPEN is rejected without unannounced owner or dial frame");
    peer.begin(1,"/pressure");auto dial=peer.channel(CAPSID_EVENT_CHANNEL_DIAL);
    peer.begin(100,"/filler");
    bool filler_head=false, filler_complete=false;
    std::string filler;
    // The fixture delegates the head and first body to the real socket,
    // then stalls the following full-sized body frame. The real Worker
    // keeps processing input. Closing waits for both actual observer barriers,
    // so a slow Host cannot close the pending dial before ACCEPT was observed.
    while(filler.empty()) {
        const auto event=peer.next();
        check(event.request_id==100,"filler progress belongs to the pressure owner");
        if(event.type==CAPSID_EVENT_RESPONSE_HEAD) {
            check(!filler_head && event.status==200,"one filler head before its body");
            filler_head=true;
            ok(capsid_worker_grant_response_credit(peer.worker,100,1024*1024),
               "grant the filler body credit before checking all of its bytes");
        } else if(event.type==CAPSID_EVENT_RESPONSE_BODY) {
            check(filler_head,"filler body follows its own head");
            filler.append(reinterpret_cast<const char*>(event.payload.data),event.payload.size);
        } else check(false,"filler must make real body progress before ACCEPT");
    }
    if(delay_before_accept_ms)std::this_thread::sleep_for(std::chrono::milliseconds(delay_before_accept_ms));
    ok(capsid_worker_dial_accept(peer.worker,dial.dial_id,1,1024*1024,"chat"),"ACCEPT under pressure");
    ok(capsid_worker_flush(peer.worker),"deliver ACCEPT");
    await_writer_pressure(receipt.path,worker_pid,dial.dial_id);
    peer.begin(101,"/pressure-close");
    bool ack=false,cancel=false,complete=false,head=false,control_head=false,control_complete=false;
    std::string body,control_body;
    while(!complete || !filler_complete || !control_complete) {
        auto event=peer.next();
        if(event.type>=CAPSID_EVENT_CHANNEL_DIAL && event.type<=CAPSID_EVENT_CHANNEL_ACK) {
            capsid_channel_event view;capsid_channel_event_init(&view);ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"channel decode");
            if(event.type==CAPSID_EVENT_CHANNEL_ACK) {check(!ack && !cancel && view.channel_id==1 && view.dial_id==dial.dial_id,"one result ACK before assigned cancel");ack=true;}
            else if(event.type==CAPSID_EVENT_CHANNEL_CANCEL) {check(ack && !cancel && view.channel_id==1 && !view.dial_id,"one assigned cancel after observed ACCEPT before open dispatch");cancel=true;ok(capsid_worker_channel_confirm(peer.worker,1,0,1),"terminal ACK");}
            else check(false,"cancelled CONNECTING socket emits no DATA/open Close");
        } else if(event.request_id==1) {
            check(!complete,"no event after the pressure response terminal");
            if(event.type==CAPSID_EVENT_RESPONSE_HEAD) {check(!head && event.status==200,"one pressure response head");head=true;}
            else if(event.type==CAPSID_EVENT_RESPONSE_BODY) {check(head,"pressure body follows head");body.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);}
            else if(event.type==CAPSID_EVENT_RESPONSE_END) {check(head,"pressure terminal follows head");complete=true;}
            else check(false,"pressure HTTP event kind");
        } else if(event.request_id==100) {
            check(!filler_complete,"no filler event after its terminal");
            if(event.type==CAPSID_EVENT_RESPONSE_BODY)filler.append(reinterpret_cast<const char*>(event.payload.data),event.payload.size);
            else if(event.type==CAPSID_EVENT_RESPONSE_END)filler_complete=true;
            else check(false,"no duplicate filler head or unexpected event");
        } else if(event.request_id==101) {
            check(!control_complete,"no close-control event after its terminal");
            if(event.type==CAPSID_EVENT_RESPONSE_HEAD) {check(!control_head&&event.status==200,"one actual close-control response head");control_head=true;}
            else if(event.type==CAPSID_EVENT_RESPONSE_BODY) {check(control_head,"close-control body follows its head");control_body.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);}
            else if(event.type==CAPSID_EVENT_RESPONSE_END) {check(control_head,"close-control terminal follows its head");control_complete=true;}
            else check(false,"close-control response event kind");
        } else check(false,"all pressured frames retain their original owner");
    }
    check(control_body=="close-requested","CONNECTING close runs only after the observed blocked result ACK");
    check(ack && cancel && body.find("\"events\":[\"error\",\"close\"]")!=std::string::npos,"no spurious open under result ACK pressure");
    check(body.find("\"initial\":0")!=std::string::npos && body.find("\"state\":3")!=std::string::npos &&
          body.find("\"code\":1006")!=std::string::npos && body.find("\"clean\":false")!=std::string::npos,
          "cancelled constructor preserves initial/final state and abnormal close");
    check(filler==std::string(1024*1024,'f'),"complete filler bytes and RESPONSE_END survive the real writer stall");
    peer.begin(200,"/healthy");check(peer.response(200)=="healthy","HTTP remains healthy after the result ACK barrier");
    // Complete the documented graceful sequence, including retries when
    // the pressured writer still reports WOULD_BLOCK, before abortive cleanup.
    const auto exited=capsid_test::shutdown_and_wait(peer.worker,2500);
    const auto cleanup=capsid::destroy_worker_with_status(peer.worker);peer.worker=nullptr;
    std::printf("OUTBOUND_PRESSURE_EXIT clean=%d shutdown=%d flush=%d event=%d closed=%d reaped=%d status=%d wait=%d forced=%d errors=%u\n",
        exited.clean() ? 1 : 0,int(exited.shutdown_result),int(exited.flush_result),int(exited.event_result),
        exited.channel_closed ? 1 : 0,exited.reaped ? 1 : 0,exited.status,exited.wait_error,
        cleanup.destroy_forced ? 1 : 0,exited.error_events);
    check(exited.clean() && !cleanup.destroy_forced,"real writer fixture verifies the full-queue premise and exits naturally");
    check(std::remove(receipt.path.c_str())==0,"owned receipt removed after actual worker exit");receipt.path.clear();
    std::printf("writer pressure: observed ACCEPT cancels assigned identity before open; pre-ACCEPT delay=%u ms PASS\n",delay_before_accept_ms);
}

void result_ack_pressure(const char *worker_path) {
    result_ack_pressure_case(worker_path,0);
    result_ack_pressure_case(worker_path,350);
}

}
int run_main(int argc,char **argv) {
    check(argc==3,"worker plus writer fixture, or one explicit pressure case");
    if(std::string(argv[2])=="ack-pressure") {result_ack_pressure(argv[1]);return 0;}
    if(std::string(argv[2])=="head-pressure-v3" || std::string(argv[2])=="head-pressure-v4") {
        const std::string scenario=argv[2];
        check(scenario=="head-pressure-v3" || scenario=="head-pressure-v4","known pressure scenario");
        response_head_pressure(argv[1],scenario=="head-pressure-v3" ? 3 : 4);return 0;
    }
    capsid_worker_config config;capsid_worker_config_init(&config);config.worker_path=argv[1];
    capsid_egress_rule rules[2];for(auto &rule:rules)capsid_egress_rule_init(&rule);
    char allowed[]="localhost";rules[0].action=CAPSID_EGRESS_ALLOW;rules[0].target=allowed;
    rules[1].action=CAPSID_EGRESS_DENY;rules[1].target="127.0.0.0/8";
    capsid_egress_policy policy;capsid_egress_policy_init(&policy);policy.rules=rules;policy.rule_count=2;config.egress_policy=&policy;
    capsid_protocol_options options;capsid_protocol_options_init(&options,sizeof(options));
    options.wire_version=4;options.capabilities=27;options.max_channels=2;
    Peer peer;ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"v4 spawn");
    allowed[0]='X'; // descriptors can change after spawn; authority cannot.
    capsid::EgressPolicy snapshot;check(capsid::client_egress_snapshot(peer.worker,&snapshot),"host authority snapshot");
    check(snapshot.decide_host("localhost",443).allowed,"immutable domain grant");
    check(!snapshot.decide_host("127.0.0.1",443).allowed,"explicit CIDR deny for numeric host");
    check(!snapshot.decide_host("169.254.169.254",80).allowed,"protected address denied");
    sockaddr_in resolved{};resolved.sin_family=AF_INET;resolved.sin_port=htons(443);inet_pton(AF_INET,"127.0.0.1",&resolved.sin_addr);
    check(!snapshot.decide_resolved_address_authoritative(reinterpret_cast<sockaddr *>(&resolved),sizeof(resolved),443).allowed,"actual DNS result still obeys explicit CIDR deny");
    ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t *>(bundle),std::strlen(bundle)),"load");
    check(peer.next().type==CAPSID_EVENT_READY,"READY");
    peer.begin(1,"/success");auto dial=peer.channel(CAPSID_EVENT_CHANNEL_DIAL);
    check(dial.dial_id && !dial.channel_id && dial.owner_kind==4,"standalone dial has independent identity");
    ok(capsid_worker_dial_accept(peer.worker,dial.dial_id,1,1024*1024,"chat"),"provisional accept");
    check(capsid_worker_channel_send(peer.worker,1,reinterpret_cast<const uint8_t *>("early"),5,7)==CAPSID_PROTOCOL_ERROR,"host cannot send before result ACK");
    auto ack=peer.channel(CAPSID_EVENT_CHANNEL_ACK);check(ack.channel_id==1 && ack.dial_id==dial.dial_id,"ACK before open callback DATA");
    auto data=peer.channel(CAPSID_EVENT_CHANNEL_DATA);check(data.channel_id==1 && data.message.size==5 && std::memcmp(data.message.data,"hello",5)==0,"open callback sends owned text");
    ok(capsid_worker_channel_send(peer.worker,1,reinterpret_cast<const uint8_t *>("reply"),5,7),"server message");
    // Receive credit before the JS close, and drain both before sending
    // the opposed network Close marker.
    peer.channel(CAPSID_EVENT_CHANNEL_CREDIT);auto close=peer.channel(CAPSID_EVENT_CHANNEL_CLOSE);check(close.code==1000,"worker initiates Close");
    ok(capsid_worker_channel_close_ex(peer.worker,1,1,1000,"done"),"observed close");
    auto text=peer.response(1);check(text.find("\"events\":[\"open\",\"reply\",\"close\"]")!=std::string::npos && text.find("\"state\":3")!=std::string::npos,"open/message/clean close event sequence");
    uint64_t last_dial=dial.dial_id;uint32_t assigned=2;
    for(uint64_t request=2;request<22;++request) {
        const bool cancel=(request%2)==0;peer.begin(request,cancel?"/cancel":"/reject");
        auto pending=peer.channel(CAPSID_EVENT_CHANNEL_DIAL);check(pending.dial_id>last_dial,"monotonic dial IDs");last_dial=pending.dial_id;
        if(cancel) {
            // ACCEPT was queued before the host consumes the pending
            // CANCEL. The worker must suppress it and send no result ACK.
            ok(capsid_worker_dial_accept(peer.worker,pending.dial_id,assigned++,1024*1024,"chat"),"crossed provisional result");
            auto cancelled=peer.channel(CAPSID_EVENT_CHANNEL_CANCEL);check(!cancelled.channel_id && cancelled.dial_id==pending.dial_id,"pending cancel identity");
            ok(capsid_worker_channel_confirm(peer.worker,0,pending.dial_id,1),"cancel ACK after actual IO stopped");
        } else {
            ok(capsid_worker_dial_reject(peer.worker,pending.dial_id,1,"egress denied"),"reject");
            auto result=peer.channel(CAPSID_EVENT_CHANNEL_ACK);check(!result.channel_id && result.dial_id==pending.dial_id,"reject result confirmed");
        }
        const auto result=peer.response(request);
        check(result.find("\"events\":[\"error\",\"close\"]")!=std::string::npos && result.find("\"code\":1006")!=std::string::npos && result.find("\"initial\":0")!=std::string::npos,"failure/cancel has no spurious open or clean close");
    }
    peer.begin(22,"/healthy");check(peer.response(22)=="healthy","owners and pending quota reclaimed after repeated dial races");
    orphaned_http_dials(argv[1]);
    result_ack_pressure(argv[2]);
    std::puts("outbound worker: result barriers, pending cancel races, finite owners and authority snapshot PASS");
    return 0;
}

int main(int argc,char** argv) {
    try {return run_main(argc,argv);}
    catch(const std::exception& error) {std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
