#include "capsid/runtime.h"
#include "win32_compat.h"
#include "graceful_worker_exit.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <set>
#include <vector>

namespace {
void check(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, capsid_last_error()); std::exit(1); }
}
void ok(capsid_result result, const char *message) { check(result == CAPSID_OK, message); }
struct Peer {
    capsid_worker *worker = nullptr;
    ~Peer() { capsid_worker_destroy(worker); }
    capsid_event next() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            auto flush = capsid_worker_flush(worker);
            check(flush == CAPSID_OK || flush == CAPSID_WOULD_BLOCK, "IPC flush");
            capsid_event event{}; event.struct_size = sizeof(event);
            auto result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK) {
                if (event.type == CAPSID_EVENT_ERROR || event.type == CAPSID_EVENT_EXIT) {
                    std::fprintf(stderr, "unexpected terminal: %.*s\n", int(event.payload.size), event.payload.data);
                    check(false, "worker remains alive");
                }
                return event;
            }
            check(result == CAPSID_WOULD_BLOCK && std::chrono::steady_clock::now() < deadline, "event deadline");
            capsid_pollfd fd{}; fd.fd = capsid_worker_fd(worker); fd.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&fd, 1, 20);
        }
    }
    capsid_event_type type() { return next().type; }
};
const char bundle[] = R"JS(
let closed = 0, late = 0;
export default { async fetch(request) {
    const path = new URL(request.url).pathname;
    if (path === '/status') return Response.json({closed});
    if (path === '/wait-status') { await new Promise(resolve=>setTimeout(resolve,75)); return Response.json({closed,late}); }
    if (path === '/reject') return new Response('no upgrade', {status: 403});
    if (path === '/selftest') {
        const pair = new WebSocketPair();
        const socket = pair[1];
        let before = '', code = '', reason = '', transfer = '';
        try { socket.send('x'); } catch (e) { before = e.name; }
        socket.accept(); socket.accept();
        try { socket.close(1006); } catch (e) { code = e.name; }
        try { socket.close(1000, '界'.repeat(42)); } catch (e) { reason = e.name; }
        // C14: 101 construction succeeds regardless of endpoint state;
        // commit readiness is validated at send time, so transfer stays ''
        try { new Response(null, {status:101, webSocket:socket}); } catch (e) { transfer = e.name; }
        socket.binaryType = 'invalid';
        return Response.json({keys:Object.keys(pair), iterator:[...pair].length, before, code, reason, transfer,
            ready:socket.readyState, binary:socket.binaryType, close:typeof CloseEvent});
    }
    const pair = new WebSocketPair();
    const server = pair[1];
    server.accept({allowHalfOpen:path === '/half'});
    server.binaryType = 'arraybuffer';
    server.addEventListener('message', async event => {
        if (path === '/pending') {
            server.send('started');
            await new Promise(resolve=>setTimeout(resolve,30));
            try { server.send('after await'); late++; } catch {}
            return;
        }
        await new Promise(resolve => setTimeout(resolve, 3));
        if (event.data === 'copy') {
            const original = new Uint8Array([1,2,3,4]);
            const result = server.send(original.subarray(1,3));
            original.fill(9);
            server.send(String(result));
        } else server.send(event.data);
    });
    server.addEventListener('close', event => {
        closed++;
        // This probe tests cancellation after the application ends its local
        // direction. EOF alone retains incoming timers, as workerd does.
        if (path === '/pending') server.close(1000, 'local');
        if (path === '/half') {
            server.send(JSON.stringify({code:event.code,reason:event.reason,clean:event.wasClean,state:server.readyState}));
            server.close(1000, 'half done');
        }
    });
    if (path === '/preclose') { server.send('before close'); server.close(3000, 'planned'); }
    return new Response(null, {status:101, webSocket:pair[0], headers:{'x-ws-test':'yes'}});
} };
)JS";

void begin(Peer &peer, uint64_t request, uint32_t channel, const char *path) {
    ok(capsid_worker_upgrade_begin(peer.worker, request, channel, 1024*1024), "reserve upgrade");
    std::string url = std::string("https://example.test") + path;
    if (std::strcmp(path,"/half")==0) {
        ok(capsid_worker_begin_request(peer.worker,request,"GET",url.c_str(),nullptr,0),"upgrade through original begin API");
        check(capsid_worker_end_request(peer.worker,request)==CAPSID_INVALID_ARGUMENT,"upgrade ends the bodyless request in both begin variants");
    } else ok(capsid_worker_begin_bodyless_request(peer.worker, request, "GET", url.c_str(), nullptr, 0), "upgrade request");
    auto event = peer.next();
    check(event.type == CAPSID_EVENT_RESPONSE_HEAD && event.status == 101 &&
        (event.flags & 32), "real 101 is an HTTP terminal");
    check(event.credit == channel, "upgraded channel identity");
    check(capsid_worker_grant_response_credit(peer.worker, request, 1) == CAPSID_INVALID_ARGUMENT,
          "101 releases HTTP request while channel stays live");
}

std::vector<uint8_t> message(Peer &peer, uint32_t channel, bool grant = true) {
    std::vector<uint8_t> result;
    bool started = false;
    for (;;) {
        auto event = peer.next();
        if (event.type == CAPSID_EVENT_CHANNEL_CREDIT) continue;
        if (event.type != CAPSID_EVENT_CHANNEL_DATA) std::fprintf(stderr, "channel %u unexpected event=%u status=%u flags=%u\n", channel, unsigned(event.type), event.status, event.flags);
        check(event.type == CAPSID_EVENT_CHANNEL_DATA, "message is channel DATA, never HTTP body/end");
        capsid_channel_event view; capsid_channel_event_init(&view);
        ok(capsid_channel_event_decode(&event, &view, sizeof(view)), "data decode");
        check(view.channel_id == channel && bool(event.flags & 1) == !started, "message fragment order");
        started = true;
        result.insert(result.end(), view.message.data, view.message.data + view.message.size);
        if (view.message.size && grant) ok(capsid_worker_channel_grant(peer.worker, channel, view.message.size), "return worker byte credit");
        if (event.flags & 2) return result;
    }
}
void closed(Peer &peer, uint32_t channel, uint16_t code) {
    for (;;) {
        auto event = peer.next();
        if (event.type == CAPSID_EVENT_CHANNEL_CREDIT) continue;
        check(event.type == CAPSID_EVENT_CHANNEL_CLOSE, "single close event");
        capsid_channel_event view; capsid_channel_event_init(&view);
        ok(capsid_channel_event_decode(&event, &view, sizeof(view)), "close decode");
        check(view.channel_id == channel && view.code == code, "close code and identity");
        return;
    }
}

void response_commit_controls(const char *worker_path) {
    const char response_bundle[] = R"JS(
let invalidCallbacks=0,accepted=false,messages=0,closed=0,release;
export default {async fetch(request) {
 const path=new URL(request.url).pathname;
 if(path==='/state') {
  await new Promise(resolve=>setTimeout(resolve,10));
  return Response.json({invalidCallbacks,accepted,messages,closed});
 }
 if(path==='/release'){release();release=null;return new Response('released')}
 const pair=new WebSocketPair(),server=pair[1];
 if(['/accepted','/both','/closed'].includes(path)) {
  for(const socket of Object.values(pair)) {
   socket.onmessage=()=>invalidCallbacks++;socket.onclose=()=>invalidCallbacks++;
  }
  server.accept();if(path==='/both')pair[0].accept();
  if(path==='/closed')server.close(1000,'local');
  return new Response(null,{status:101,webSocket:path==='/both'?pair[0]:server});
 }
 if(path.startsWith('/unaccepted')) {
  server.onmessage=()=>invalidCallbacks++;server.onclose=()=>invalidCallbacks++;
  return new Response(null,{status:101,webSocket:pair[0]});
 }
 if(path==='/delayed' || path==='/delayed-close') {
  accepted=false;
  new Promise(resolve=>release=resolve).then(()=>{server.accept();accepted=true});
 } else server.accept();
 server.onmessage=e=>{messages++;if(path==='/delayed-close')return;server.send('reply:'+e.data);server.close(1000,'done')};
 server.onclose=()=>closed++;
 return new Response(null,{status:101,webSocket:pair[0]});
}};
)JS";
    capsid_worker_config config;capsid_worker_config_init(&config);
    config.worker_path=worker_path;config.request_timeout_ms=2000;
    capsid_protocol_options options;capsid_protocol_options_init(&options,sizeof(options));
    options.wire_version=4;options.capabilities=27;options.max_channels=2;
    Peer peer;ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"Response control spawn");
    ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t *>(response_bundle),sizeof(response_bundle)-1),"Response control load");
    check(peer.type()==CAPSID_EVENT_READY,"Response control READY");
    size_t diagnostics=0;
    const auto next=[&]() {
        for(;;) {
            auto event=peer.next();
            if(event.type!=CAPSID_EVENT_LOG)return event;
            const std::string text(reinterpret_cast<const char *>(event.payload.data),event.payload.size);
            check(text.find("TypeError")!=std::string::npos,"coupling failure logs its scoped TypeError");
            ++diagnostics;
        }
    };
    const auto start=[&](uint64_t request,uint32_t channel,const char *path) {
        ok(capsid_worker_upgrade_begin(peer.worker,request,channel,1024*1024),"Response reserve");
        const std::string url=std::string("https://example.test")+path;
        ok(capsid_worker_begin_bodyless_request(peer.worker,request,"GET",url.c_str(),nullptr,0),"Response begin");
        const auto event=next();
        check(event.type==CAPSID_EVENT_RESPONSE_HEAD && event.status==101 && (event.flags&32) && event.credit==channel,
              "Response 101 commits before coupling or delayed accept");
        check(capsid_worker_grant_response_credit(peer.worker,request,1)==CAPSID_INVALID_ARGUMENT,
              "committed Response 101 is an HTTP terminal");
    };
    const auto state=[&](uint64_t id,const char *expected) {
        ok(capsid_worker_begin_bodyless_request(peer.worker,id,"GET","https://example.test/state",nullptr,0),"Response state request");
        bool head=false,end=false;std::string body;
        while(!end) {
            const auto event=next();
            if(event.type==CAPSID_EVENT_CHANNEL_CREDIT)continue;
            check(event.request_id==id,"Response state belongs to its HTTP owner");
            if(event.type==CAPSID_EVENT_RESPONSE_HEAD){check(!head && event.status==200,"Response state head");head=true;}
            else if(event.type==CAPSID_EVENT_RESPONSE_BODY)body.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);
            else {check(event.type==CAPSID_EVENT_RESPONSE_END && head,"no message or Close before peer accept");end=true;}
        }
        check(body==expected,"Response callback counts and acceptance state");
    };
    for(uint32_t i=0;i<20;++i) {
        const char *path=i%3==0?"/accepted":i%3==1?"/both":"/closed";
        const uint32_t channel=100+i;start(100+i,channel,path);
        const auto event=next();check(event.type==CAPSID_EVENT_CHANNEL_CLOSE,"coupling failure aborts its transport after 101");
        capsid_channel_event view;capsid_channel_event_init(&view);
        ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"Response abort decode");
        check(view.channel_id==channel && view.close_kind==2 && view.code==1006,"coupling failure is transport abort, never a wire Close");
        ok(capsid_worker_channel_confirm(peer.worker,channel,0,CAPSID_CHANNEL_ACK_TRANSPORT_CLOSE),"stopped simulated transport ACK");
    }
    state(1000,"{\"invalidCallbacks\":0,\"accepted\":false,\"messages\":0,\"closed\":0}");
    check(diagnostics==20,"each failed coupling records exactly one TypeError");
    start(1001,1000,"/delayed");
    ok(capsid_worker_channel_send(peer.worker,1000,reinterpret_cast<const uint8_t *>("early"),5,7),"early network message before peer accept");
    state(1002,"{\"invalidCallbacks\":0,\"accepted\":false,\"messages\":0,\"closed\":0}");
    ok(capsid_worker_begin_bodyless_request(peer.worker,1003,"GET","https://example.test/release",nullptr,0),"release peer acceptance");
    bool head=false,end=false,echo=false,close=false;std::string body;
    while(!end || !echo || !close) {
        const auto event=next();
        if(event.type==CAPSID_EVENT_CHANNEL_CREDIT)continue;
        if(event.type==CAPSID_EVENT_CHANNEL_DATA) {
            capsid_channel_event view;capsid_channel_event_init(&view);ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"delayed echo decode");
            check(!echo && view.channel_id==1000 && view.message.size==11 && std::memcmp(view.message.data,"reply:early",11)==0,"queued message dispatches exactly once after accept");echo=true;
        } else if(event.type==CAPSID_EVENT_CHANNEL_CLOSE) {
            capsid_channel_event view;capsid_channel_event_init(&view);ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"delayed Close decode");
            check(echo && !close && view.channel_id==1000 && view.close_kind==0 && view.code==1000,"delayed response clean Close follows echo");close=true;
            ok(capsid_worker_channel_close(peer.worker,1000,1000,"done"),"observed network Close reply");
        } else {
            check(event.request_id==1003,"release HTTP identity across interleaved channel events");
            if(event.type==CAPSID_EVENT_RESPONSE_HEAD){check(!head && event.status==200,"release HTTP head");head=true;}
            else if(event.type==CAPSID_EVENT_RESPONSE_BODY)body.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);
            else {check(event.type==CAPSID_EVENT_RESPONSE_END && head,"release HTTP end");end=true;}
        }
    }
    check(body=="released","release body completes independently of channel events");
    state(1004,"{\"invalidCallbacks\":0,\"accepted\":true,\"messages\":1,\"closed\":1}");
    start(1005,1001,"/delayed-close");
    ok(capsid_worker_channel_send(peer.worker,1001,reinterpret_cast<const uint8_t *>("early"),5,7),"DATA preceding pre-accept Close");
    ok(capsid_worker_channel_close(peer.worker,1001,1000,"done"),"ordinary Close before peer accept");
    state(1006,"{\"invalidCallbacks\":0,\"accepted\":false,\"messages\":1,\"closed\":1}");
    ok(capsid_worker_begin_bodyless_request(peer.worker,1007,"GET","https://example.test/release",nullptr,0),"release peer after DATA and Close");
    head=false;end=false;close=false;body.clear();
    while(!end || !close) {
        const auto event=next();if(event.type==CAPSID_EVENT_CHANNEL_CREDIT)continue;
        if(event.type==CAPSID_EVENT_CHANNEL_CLOSE) {
            capsid_channel_event view;capsid_channel_event_init(&view);ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"pre-accept Close decode");
            check(!close && view.channel_id==1001 && view.close_kind==0 && view.code==1000,"buffered ordinary Close remains clean after accept");close=true;
        } else {
            check(event.request_id==1007,"release identity after pre-accept Close");
            if(event.type==CAPSID_EVENT_RESPONSE_HEAD){check(!head && event.status==200,"second release head");head=true;}
            else if(event.type==CAPSID_EVENT_RESPONSE_BODY)body.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);
            else {check(event.type==CAPSID_EVENT_RESPONSE_END && head,"no unexpected DATA for close-only fixture");end=true;}
        }
    }
    check(body=="released","second release body completes");
    state(1008,"{\"invalidCallbacks\":0,\"accepted\":true,\"messages\":2,\"closed\":2}");
    start(1010,1003,"/normal");start(1011,1004,"/normal");
    for(uint32_t channel:{1003u,1004u}) {
        ok(capsid_worker_channel_close(peer.worker,channel,1000,"done"),"close two reused live slots");
        const auto event=next();check(event.type==CAPSID_EVENT_CHANNEL_CLOSE,"reused slot clean Close");
        capsid_channel_event view;capsid_channel_event_init(&view);ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"reused Close decode");
        check(view.channel_id==channel && view.code==1000,"both quota slots return after failed couplings");
    }
    state(1012,"{\"invalidCallbacks\":0,\"accepted\":true,\"messages\":2,\"closed\":4}");
    // An unaccepted peer is a bounded provisional owner, including after a
    // buffered ordinary Close. Neither path may dispatch application events.
    const auto waiting_since=std::chrono::steady_clock::now();
    start(1020,1010,"/unaccepted");start(1021,1011,"/unaccepted-close");
    for(uint32_t channel:{1010u,1011u})
        ok(capsid_worker_channel_send(peer.worker,channel,reinterpret_cast<const uint8_t *>("early"),5,7),"unaccepted peer buffers DATA");
    ok(capsid_worker_channel_close(peer.worker,1011,1000,"done"),"unaccepted peer buffers clean Close");
    bool timeout_first=false,timeout_second=false;
    while(!timeout_first || !timeout_second) {
        const auto event=next();if(event.type==CAPSID_EVENT_CHANNEL_CREDIT)continue;
        check(event.type==CAPSID_EVENT_CHANNEL_CANCEL,"unaccepted coupling expires through native cancellation");
        capsid_channel_event view;capsid_channel_event_init(&view);ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"coupling timeout identity");
        check(view.channel_id==1010 || view.channel_id==1011,"coupling deadline only cancels its own channels");
        bool &seen=view.channel_id==1010?timeout_first:timeout_second;
        check(!seen,"each coupling deadline cancels exactly once");seen=true;
        uint32_t reason=99;ok(capsid_channel_cancel_reason(&event,&reason),"coupling timeout reason");
        check(reason==CAPSID_CANCEL_REASON_TIMEOUT,"coupling uses the original request deadline");
        ok(capsid_worker_channel_confirm(peer.worker,view.channel_id,0,CAPSID_CHANNEL_ACK_CANCEL),"simulated I/O stopped before timeout ACK");
    }
    check(std::chrono::steady_clock::now()-waiting_since<std::chrono::seconds(5),"native coupling deadline precedes the ten-second JS fallback");
    state(1022,"{\"invalidCallbacks\":0,\"accepted\":true,\"messages\":2,\"closed\":4}");
    start(1023,1012,"/unaccepted-disconnect");
    ok(capsid_worker_channel_cancel(peer.worker,1012,CAPSID_CANCEL_REASON_DISCONNECT),"transport disconnect before accept");
    const auto disconnected=next();check(disconnected.type==CAPSID_EVENT_CHANNEL_ACK,"unaccepted disconnect promptly acknowledges stopped I/O");
    uint32_t ack_kind=99;ok(capsid_channel_ack_kind(&disconnected,&ack_kind),"unaccepted disconnect ACK kind");
    check(ack_kind==CAPSID_CHANNEL_ACK_CANCEL,"unaccepted disconnect has a cancellation barrier");
    state(1024,"{\"invalidCallbacks\":0,\"accepted\":true,\"messages\":2,\"closed\":4}");
    start(1025,1013,"/normal");start(1026,1014,"/normal");
    for(uint32_t channel:{1013u,1014u}) {
        ok(capsid_worker_channel_close(peer.worker,channel,1000,"done"),"reuse both slots after coupling timeout and disconnect");
        const auto event=next();check(event.type==CAPSID_EVENT_CHANNEL_CLOSE,"post-timeout reused slot Close");
        capsid_channel_event view;capsid_channel_event_init(&view);ok(capsid_channel_event_decode(&event,&view,sizeof(view)),"post-timeout reused identity");
        check(view.channel_id==channel && view.code==1000,"post-timeout slots are live");
    }
    state(1027,"{\"invalidCallbacks\":0,\"accepted\":true,\"messages\":2,\"closed\":6}");
    ok(capsid_worker_shutdown(peer.worker),"Response control shutdown");
    std::puts("Response: committed coupling aborts, delayed DATA/Close, deadlines, disconnect and two-slot reuse PASS");
}

void response_precommit_timeout_control(const char *worker_path) {
    const char pressure_bundle[]=R"JS(
export default {async fetch(request) {
 if(new URL(request.url).pathname==='/healthy') {
  await new Promise(resolve=>setTimeout(resolve,30));return new Response('healthy');
 }
 const pair=new WebSocketPair();
 return new Response(null,{status:101,webSocket:pair[0],headers:{'x-pressure':'x'.repeat(2048)}});
}};
)JS";
    capsid_worker_config config;capsid_worker_config_init(&config);
    config.worker_path=worker_path;config.request_timeout_ms=200;
    config.max_queued_bytes=1024;config.max_header_bytes=8192;
    capsid_protocol_options options;capsid_protocol_options_init(&options,sizeof(options));
    options.wire_version=4;options.capabilities=27;options.max_channels=2;
    Peer peer;ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"precommit timeout spawn");
    ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t *>(pressure_bundle),sizeof(pressure_bundle)-1),"precommit timeout bundle");
    check(peer.type()==CAPSID_EVENT_READY,"precommit timeout READY");
    for(uint32_t i=0;i<3;++i) {
        const uint32_t id=2000+i;
        ok(capsid_worker_upgrade_begin(peer.worker,id,id,1024*1024),"repeated uncommitted reservation reuses two slots");
        ok(capsid_worker_begin_bodyless_request(peer.worker,id,"GET","https://example.test/blocked",nullptr,0),"valid head exceeds native output capacity");
        const auto event=peer.next();
        if(event.type!=CAPSID_EVENT_REQUEST_TIMEOUT || event.request_id!=id)
            std::fprintf(stderr,"uncommitted timeout: event=%u request=%llu flags=%u\n",unsigned(event.type),static_cast<unsigned long long>(event.request_id),event.flags);
        check(event.type==CAPSID_EVENT_REQUEST_TIMEOUT && event.request_id==id,
              "uncommitted 101 expires as HTTP timeout, without channel CANCEL or premature Upgrade");
        ok(capsid_worker_begin_bodyless_request(peer.worker,2010+i,"GET","https://example.test/healthy",nullptr,0),"HTTP after uncommitted timeout");
        check(peer.type()==CAPSID_EVENT_RESPONSE_HEAD,"precommit timeout keeps worker healthy");
        const auto body=peer.next();check(body.type==CAPSID_EVENT_RESPONSE_BODY && body.payload.size==7 && std::memcmp(body.payload.data,"healthy",7)==0,"precommit timeout healthy body");
        check(peer.type()==CAPSID_EVENT_RESPONSE_END,"no duplicate timeout or late channel control");
    }
    ok(capsid_worker_shutdown(peer.worker),"precommit timeout shutdown");
    std::puts("Response: oversized uncommitted head retains HTTP timeout and releases its reservation PASS");
}
void disconnected_local_owner_controls(const char *worker_path, bool lifetime) {
    const char retained_bundle[]=R"JS(
const records=[];
export default {async fetch(request) {
 const url=new URL(request.url),path=url.pathname;
 if(path==='/state') {
  await new Promise(resolve=>setTimeout(resolve,30));
  return Response.json(records.map(({release,...record})=>record));
 }
 if(path==='/release') {records[Number(url.searchParams.get('id'))].release();return new Response('released');}
 const pair=new WebSocketPair(),server=pair[1],record={started:false,ticks:0,closed:0,before:null,after:null,microtasks:0,late:0};
 records.push(record);server.accept({allowHalfOpen:true});
 server.onmessage=async()=>{
  record.started=true;setInterval(()=>record.ticks++,2);server.send('started');
  await new Promise(resolve=>record.release=resolve);
  record.before=server.readyState;server.close(1000,'local');record.after=server.readyState;
  Promise.resolve().then(()=>{record.microtasks++;return Promise.resolve();}).then(()=>record.microtasks++);
  setTimeout(()=>record.late++,10);
 };
 server.onclose=event=>{record.closed++;record.code=event.code;};
 return new Response(null,{status:101,webSocket:pair[0]});
}};
)JS";
    capsid_worker_config config;capsid_worker_config_init(&config);
    config.worker_path=worker_path;config.strict_sandbox=0;config.request_timeout_ms=2000;
    capsid_protocol_options options;capsid_protocol_options_init(&options,sizeof(options));
    options.wire_version=4;options.capabilities=27|(lifetime?CAPSID_PROTOCOL_CAP_CONNECTION_LIFETIME:0);options.max_channels=2;
    Peer peer;ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"retained local owner spawn");
    ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t *>(retained_bundle),sizeof(retained_bundle)-1),"retained local owner bundle");
    check(peer.type()==CAPSID_EVENT_READY,"retained local owner READY");
    capsid_protocol_info info;ok(capsid_worker_get_protocol_info(peer.worker,&info,sizeof(info)),"retained lifetime negotiation");
    check(info.effective_capabilities==(lifetime?155u:27u),"retained lifetime is explicitly negotiated; legacy profile unchanged");
    std::set<uint32_t> retained_ids,cancelled_ids;
    auto next=[&]() {
        for(;;) {
            const auto event=peer.next();
            if(event.type!=CAPSID_EVENT_CONNECTION_LIFETIME)return event;
            check(lifetime && event.request_id==0 && event.flags==0 && event.payload.size==8,"typed lifetime event only on negotiated profile");
            const auto *bytes=event.payload.data;
            const uint32_t id=uint32_t(bytes[0])|(uint32_t(bytes[1])<<8)|(uint32_t(bytes[2])<<16)|(uint32_t(bytes[3])<<24);
            const uint32_t retain=uint32_t(bytes[4])|(uint32_t(bytes[5])<<8)|(uint32_t(bytes[6])<<16)|(uint32_t(bytes[7])<<24);
            check(id && retain<=1,"exact lifetime payload identity and state");
            if(retain)check(cancelled_ids.count(id)==1 && retained_ids.insert(id).second,"one lifetime pin per actually cancelled incoming owner");
            else check(retained_ids.erase(id)==1,"release matches an observed retained identity");
            check(event.credit==retained_ids.size(),"validated lifetime aggregate follows actual retain and release transitions");
        }
    };
    uint64_t request=3000;
    auto http=[&](const char *path,uint32_t status) {
        const auto id=request++;const std::string url=std::string("https://example.test")+path;
        ok(capsid_worker_begin_bodyless_request(peer.worker,id,"GET",url.c_str(),nullptr,0),"retained owner control request");
        auto head=next();
        if(head.type!=CAPSID_EVENT_RESPONSE_HEAD || head.status!=status)
            std::fprintf(stderr,"retained owner HTTP: event=%u status=%u expected=%u\n",unsigned(head.type),head.status,status);
        check(head.type==CAPSID_EVENT_RESPONSE_HEAD && head.status==status,"retained owner control HTTP status");
        std::string body;
        for(;;) {
            const auto event=next();check(event.request_id==id,"retained owner control response identity");
            if(event.type==CAPSID_EVENT_RESPONSE_END)return body;
            check(event.type==CAPSID_EVENT_RESPONSE_BODY,"retained owner control response body");
            body.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);
        }
    };
    auto disconnect=[&](uint32_t channel) {
        begin(peer,request++,channel,"/hold");
        ok(capsid_worker_channel_send(peer.worker,channel,reinterpret_cast<const uint8_t *>("go"),2,7),"enter retained incoming callback");
        auto started=message(peer,channel);check(std::string(started.begin(),started.end())=="started","retained callback has actually started");
        ok(capsid_worker_channel_cancel(peer.worker,channel,CAPSID_CANCEL_REASON_DISCONNECT),"retained incoming transport EOF");
        cancelled_ids.insert(channel);
        const auto ack=next();check(ack.type==CAPSID_EVENT_CHANNEL_ACK,"retained owner promptly acknowledges transport cancellation");
        check(!lifetime || retained_ids.count(channel)==1,"lifetime pin arrives before transport cancellation ACK");
        uint32_t kind=99;ok(capsid_channel_ack_kind(&ack,&kind),"retained owner ACK kind");check(kind==CAPSID_CHANNEL_ACK_CANCEL,"retained owner cancellation barrier");
    };
    disconnect(3000);disconnect(3001);
    auto held=http("/state",200);
    check(held.find("\"closed\":1")!=std::string::npos && held.find("\"code\":1006")!=std::string::npos,"actual abnormal Close callback is delivered while local direction stays open");
    auto ticks=[](const std::string &json,unsigned index) {
        size_t position=0;
        for(unsigned i=0;i<=index;++i) {
            position=json.find("\"ticks\":",position);check(position!=std::string::npos,"actual retained owner timer counter");position+=8;
        }
        return std::strtoull(json.c_str()+position,nullptr,10);
    };
    const auto held_again=http("/state",200);
    check(ticks(held_again,0)>ticks(held,0) && ticks(held_again,1)>ticks(held,1),"both original native timers advance after incoming EOF");
    // The two wire slots have already been acknowledged. A third incoming
    // transfer must still respect the separate bounded retained owner count.
    const auto rejected=request++;
    ok(capsid_worker_upgrade_begin(peer.worker,rejected,3002,1024*1024),"retained owner does not occupy a wire reservation");
    ok(capsid_worker_begin_bodyless_request(peer.worker,rejected,"GET","https://example.test/hold",nullptr,0),"try transfer at retained owner limit");
    const auto rejected_head=next();
    if(rejected_head.type!=CAPSID_EVENT_RESPONSE_HEAD || rejected_head.status!=500)
        std::fprintf(stderr,"retained owner rejection: event=%u status=%u\n",unsigned(rejected_head.type),rejected_head.status);
    check(rejected_head.type==CAPSID_EVENT_RESPONSE_HEAD && rejected_head.status==500,"retained local owner admission is bounded after wire ACK");
    bool rejected_end=false;
    while(!rejected_end) {
        const auto event=next();check(event.request_id==rejected,"retained owner rejection identity");
        check(event.type==CAPSID_EVENT_RESPONSE_BODY || event.type==CAPSID_EVENT_RESPONSE_END,"retained owner rejection never commits 101 or sends channel DATA");
        rejected_end=event.type==CAPSID_EVENT_RESPONSE_END;
    }
    check(http("/release?id=0",200)=="released","resolve original incoming callback from its captured promise");
    auto released=http("/state",200);
    check(!lifetime || retained_ids.size()==1,"local close releases exactly one native lifetime pin");
    check(released.find("\"before\":2,\"after\":3,\"microtasks\":2,\"late\":0,\"code\":1006")!=std::string::npos,
          "local close preserves both queued microtasks and cancels a later native timer");
    auto stable=http("/state",200);
    const auto first_end=released.find('}');
    check(first_end!=std::string::npos && released.substr(0,first_end+1)==stable.substr(0,stable.find('}')+1),"released last owner timer becomes quiescent");
    disconnect(3003);
    check(http("/release?id=1",200)=="released","release second retained owner");
    check(http("/release?id=3",200)=="released","release reused retained owner");
    const auto drained=http("/state",200);const auto final=http("/state",200);
    check(retained_ids.empty(),"all explicitly released lifetime pins return");
    check(drained==final,"all released origins become quiescent after reuse");
    // Leave one incoming endpoint genuinely retained when shutting down; it
    // must not require a local close or a fixture destructor to stop timers.
    disconnect(3004);
    check(!lifetime || retained_ids.size()==1,"one actual retained pin is left for explicit shutdown");
    const auto shutdown=capsid_test::shutdown_and_wait(peer.worker,5000);
    check(shutdown.clean(),"retained local owner exits cleanly before destroy, without a kill fallback");
    std::puts("incoming EOF: bounded local owners, delayed close, microtasks, native timer cancellation, reuse and shutdown PASS");
}
void initial_upgrade_scope_controls(const char *worker_path) {
    const char source[]=R"JS(
const records=[];
export default {async fetch(request) {
 const url=new URL(request.url);
 if(url.pathname==='/state') {await new Promise(resolve=>setTimeout(resolve,30));return Response.json(records.map(({release,...record})=>record));}
 if(url.pathname==='/resolve') {records[Number(url.searchParams.get('id'))].release();return new Response('resolved');}
 const mode=url.searchParams.get('mode'),record={mode,entered:false,timer:0,after:0,closed:0};records.push(record);
 const initial=new Promise(resolve=>record.release=resolve),pair=new WebSocketPair(),server=pair[1];server.accept();
 if(mode==='timer')setTimeout(()=>{record.timer++;record.release();},100);
 server.onclose=()=>record.closed++;
 server.onmessage=async()=>{record.entered=true;server.send('started');if(mode==='promise')setTimeout(()=>record.release(),10);await initial;record.after++;server.send('ready');server.close(1000,'done');};
 return new Response(null,{status:101,webSocket:pair[0]});
}};
)JS";
    capsid_worker_config config;capsid_worker_config_init(&config);config.worker_path=worker_path;config.strict_sandbox=0;config.request_timeout_ms=2000;
    capsid_protocol_options options;capsid_protocol_options_init(&options,sizeof(options));options.wire_version=4;options.capabilities=27;options.max_channels=2;
    Peer peer;ok(capsid_worker_spawn_ex(&config,&options,&peer.worker),"initial scope spawn");
    ok(capsid_worker_load_bundle(peer.worker,reinterpret_cast<const uint8_t *>(source),sizeof(source)-1),"initial scope bundle");check(peer.type()==CAPSID_EVENT_READY,"initial scope READY");
    uint64_t request=5000;
    auto http=[&](const char *path) {
        const auto id=request++;const std::string url=std::string("https://example.test")+path;
        ok(capsid_worker_begin_bodyless_request(peer.worker,id,"GET",url.c_str(),nullptr,0),"initial scope HTTP control");
        auto head=peer.next();check(head.type==CAPSID_EVENT_RESPONSE_HEAD&&head.status==200,"initial scope control status");std::string body;
        for(;;){const auto event=peer.next();check(event.request_id==id,"initial scope response identity");if(event.type==CAPSID_EVENT_RESPONSE_END)return body;check(event.type==CAPSID_EVENT_RESPONSE_BODY,"initial scope response body");body.append(reinterpret_cast<const char *>(event.payload.data),event.payload.size);}
    };
    for(uint32_t index=0;index<3;index++) {
        const auto channel=5000+index;const std::string path=std::string("/probe?mode=")+(index==0?"promise":index==1?"timer":"parked");begin(peer,request++,channel,path.c_str());
        ok(capsid_worker_channel_send(peer.worker,channel,reinterpret_cast<const uint8_t *>("go"),2,7),"initial scope message");
        const auto started=message(peer,channel);check(std::string(started.begin(),started.end())=="started","initial scope actual event entered");
        if(index<2) {
            const auto ready=message(peer,channel);check(std::string(ready.begin(),ready.end())=="ready","initial HTTP Promise survives into its accepted socket");
            closed(peer,channel,1000);ok(capsid_worker_channel_close(peer.worker,channel,1000,"done"),"initial scope Close reply");
        } else {
            ok(capsid_worker_channel_close(peer.worker,channel,1000,"done"),"close final owner before resolving");closed(peer,channel,1000);
            check(http("/resolve?id=2")=="resolved","foreign request actually calls the saved resolver");
        }
    }
    const auto final=http("/state");
    check(final=="[{\"mode\":\"promise\",\"entered\":true,\"timer\":0,\"after\":1,\"closed\":1},{\"mode\":\"timer\",\"entered\":true,\"timer\":1,\"after\":1,\"closed\":1},{\"mode\":\"parked\",\"entered\":true,\"timer\":0,\"after\":0,\"closed\":1}]","initial source continuations run only while the accepted origin is live");
    check(http("/state")==final,"expired initial source remains inert across unrelated requests");
    const auto shutdown=capsid_test::shutdown_and_wait(peer.worker,5000);
    check(shutdown.clean(),"initial scope exits normally before destruction, without a kill fallback");
    std::puts("initial 101 scope: original Promise/timer, Close, foreign resolver, reuse and clean exit PASS");
}
} // namespace

int main(int argc, char **argv) {
    check(argc == 2, "worker path");
    capsid_worker_config config; capsid_worker_config_init(&config);
    config.worker_path = argv[1]; config.strict_sandbox = 0; config.request_timeout_ms = 2000;
    capsid_protocol_options options; ok(capsid_protocol_options_init(&options, sizeof(options)), "options");
    options.wire_version = 4; options.capabilities = 31; options.max_channels = 2;
    Peer peer; ok(capsid_worker_spawn_ex(&config, &options, &peer.worker), "spawn");
    ok(capsid_worker_load_bundle(peer.worker, reinterpret_cast<const uint8_t *>(bundle), sizeof(bundle)-1), "load app");
    check(peer.type() == CAPSID_EVENT_READY, "ready");
    capsid_protocol_info info; ok(capsid_worker_get_protocol_info(peer.worker, &info, sizeof(info)), "negotiation");
    check(info.effective_capabilities == 27, "only implemented inbound capabilities");
    begin(peer, 1, 1, "/echo");
    // Both directions exceed the initial 64 KiB window. Send credit is
    // returned per fragment, not only when the whole message is assembled.
    std::vector<uint8_t> bytes(180000);
    for (size_t i=0; i<bytes.size(); ++i) bytes[i] = uint8_t(i*17);
    size_t sent = 0; uint64_t credit = 65536;
    while (sent < bytes.size()) {
        size_t amount = std::min<size_t>(16000, std::min<uint64_t>(bytes.size()-sent, credit));
        if (!amount) {
            auto event = peer.next(); check(event.type == CAPSID_EVENT_CHANNEL_CREDIT, "large inbound fragment credit");
            capsid_channel_event view; capsid_channel_event_init(&view);
            ok(capsid_channel_event_decode(&event, &view, sizeof(view)), "credit decode"); credit += view.credit;
            continue;
        }
        ok(capsid_worker_channel_send(peer.worker, 1, bytes.data()+sent, amount,
            (sent == 0 ? 1 : 0) | (sent+amount == bytes.size() ? 2 : 0)), "large inbound fragment");
        sent += amount; credit -= amount;
    }
    auto echoed = message(peer, 1);
    if (echoed != bytes) {
        std::fprintf(stderr, "echo size=%zu expected=%zu prefix=%.*s\n", echoed.size(), bytes.size(), int(std::min<size_t>(100,echoed.size())), echoed.data());
        for (size_t i=0;i<std::min(echoed.size(),bytes.size());++i) if(echoed[i]!=bytes[i]) {
            std::fprintf(stderr,"first different byte %zu: got %u expected %u\n",i,echoed[i],bytes[i]); break;
        }
    }
    check(echoed == bytes, "post-HTTP async echo bytes");
    ok(capsid_worker_channel_send(peer.worker, 1, reinterpret_cast<const uint8_t *>("copy"), 4, 7), "copy probe");
    check(message(peer, 1) == std::vector<uint8_t>({2,3}), "send copies typed-array view synchronously");
    auto returned = message(peer, 1); check(std::string(returned.begin(), returned.end()) == "undefined", "send returns undefined");
    ok(capsid_worker_channel_close(peer.worker, 1, 1000, "done"), "peer close"); closed(peer, 1, 1000);
    begin(peer, 2, 2, "/half");
    ok(capsid_worker_channel_close(peer.worker, 2, 3001, "peer done"), "half-open close");
    auto report = message(peer, 2, false); std::string text(report.begin(), report.end());
    check(text == "{\"code\":3001,\"reason\":\"peer done\",\"clean\":true,\"state\":2}", "allowHalfOpen close handler can send under independent owner");
    closed(peer, 2, 1000);
    begin(peer, 3, 3, "/preclose");
    auto first = message(peer, 3); check(std::string(first.begin(), first.end()) == "before close", "pre-transfer send precedes Close");
    closed(peer, 3, 3000);
    ok(capsid_worker_channel_close(peer.worker, 3, 3000, "planned"), "complete server initiated close");
    ok(capsid_worker_begin_bodyless_request(peer.worker, 4, "GET", "https://example.test/status", nullptr, 0), "unrelated HTTP still works");
    check(peer.type() == CAPSID_EVENT_RESPONSE_HEAD, "normal HTTP head after channels");
    auto body = peer.next(); check(body.type == CAPSID_EVENT_RESPONSE_BODY, "status body");
    std::string status(reinterpret_cast<const char *>(body.payload.data), body.payload.size);
    check(status == "{\"closed\":3}", "terminal close delivered exactly once per connection");
    check(peer.type() == CAPSID_EVENT_RESPONSE_END, "ordinary HTTP end");
    // Cancellation before JS can commit a 101, then a 101 already queued
    // in the opposite IPC direction. Neither may revive the HTTP owner.
    for (uint64_t request=5;request<=6;++request) {
        const uint32_t channel=uint32_t(request-1);
        ok(capsid_worker_upgrade_begin(peer.worker,request,channel,1024*1024),"cancel reservation");
        ok(capsid_worker_begin_bodyless_request(peer.worker,request,"GET","https://example.test/echo",nullptr,0),"cancelled handshake");
        if (request==6) {
            ok(capsid_worker_flush(peer.worker),"queue crossed 101");
            capsid_pollfd fd{};fd.fd=capsid_worker_fd(peer.worker);fd.events=POLLIN;
            check(capsid::win32::capsid_poll(&fd,1,2000)>0,"worker queued 101 before host cancel");
        }
        ok(capsid_worker_cancel(peer.worker,request),"HTTP cancellation uses channel barrier");
        auto ack=peer.next();
        check(ack.type==CAPSID_EVENT_CHANNEL_ACK,"cancelled 101 is suppressed before terminal ACK");
        uint32_t kind=99;ok(capsid_channel_ack_kind(&ack,&kind),"ACK kind");check(kind==1,"handshake CANCEL confirmation");
    }
    ok(capsid_worker_begin_bodyless_request(peer.worker,7,"GET","https://example.test/status",nullptr,0),"HTTP after cancelled upgrades");
    check(peer.type()==CAPSID_EVENT_RESPONSE_HEAD,"cancel barriers leave worker healthy");
    check(peer.type()==CAPSID_EVENT_RESPONSE_BODY,"healthy status body");
    check(peer.type()==CAPSID_EVENT_RESPONSE_END,"healthy status end");
    begin(peer,8,6,"/pending");
    ok(capsid_worker_channel_send(peer.worker,6,reinterpret_cast<const uint8_t *>("x"),1,7),"start async connection callback");
    auto started=message(peer,6);check(std::string(started.begin(),started.end())=="started","callback has entered its independent owner");
    ok(capsid_worker_channel_cancel(peer.worker,6,CAPSID_CANCEL_REASON_DISCONNECT),"cancel an awaiting message callback");
    auto ack=peer.next();check(ack.type==CAPSID_EVENT_CHANNEL_ACK,"pending callback does not delay transport barrier");
    ok(capsid_worker_begin_bodyless_request(peer.worker,9,"GET","https://example.test/wait-status",nullptr,0),"HTTP while cancelled callback unwinds");
    check(peer.type()==CAPSID_EVENT_RESPONSE_HEAD,"one cancellation does not poison unrelated HTTP");
    auto summary=peer.next();check(summary.type==CAPSID_EVENT_RESPONSE_BODY,"cancellation status body");
    std::string final(reinterpret_cast<const char *>(summary.payload.data),summary.payload.size);
    check(final=="{\"closed\":5,\"late\":0}","terminal event once, late native side effect blocked and cleanup owner reclaimed");
    check(peer.type()==CAPSID_EVENT_RESPONSE_END,"cleanup status end");
    ok(capsid_worker_begin_bodyless_request(peer.worker,10,"GET","https://example.test/selftest",nullptr,0),"JS validation selftest");
    check(peer.type()==CAPSID_EVENT_RESPONSE_HEAD,"selftest head");
    auto validation=peer.next();check(validation.type==CAPSID_EVENT_RESPONSE_BODY,"selftest body");
    const std::string contract(reinterpret_cast<const char *>(validation.payload.data),validation.payload.size);
    check(contract=="{\"keys\":[\"0\",\"1\"],\"iterator\":2,\"before\":\"TypeError\",\"code\":\"InvalidAccessError\",\"reason\":\"SyntaxError\",\"transfer\":\"\",\"ready\":1,\"binary\":\"blob\",\"close\":\"function\"}","executed JS boundary assertions");
    check(peer.type()==CAPSID_EVENT_RESPONSE_END,"selftest end");
    response_commit_controls(argv[1]);
    response_precommit_timeout_control(argv[1]);
    disconnected_local_owner_controls(argv[1],false);
    disconnected_local_owner_controls(argv[1],true);
    initial_upgrade_scope_controls(argv[1]);
    ok(capsid_worker_shutdown(peer.worker), "clean shutdown");
    std::puts("worker WebSocket: 101 ownership, asynchronous large echo, copying and close PASS");
}
