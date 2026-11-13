import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const fetchUpgradeLifetimeCases = Object.freeze(['close-pending','close-body','close-unread','close-unaccepted',
    'accepted-independent','two-owners','cancel-reader','abort-body','same-origin-parent-pending','same-origin-child-pending',
    'same-origin-parent-complete','same-origin-child-complete','await-open-parent','await-close-parent','await-abrupt-parent','await-abrupt-last','fetched-disconnect-close','await-deferred-abrupt-parent','await-deferred-abrupt-last']);
export const fetchUpgradeLifetimeInputPaths = Object.freeze({runner:'tests/test_websocket_fetch_upgrade_lifetime.mjs',
    fixture:'tests/fixtures/workerd-fetch-upgrade-lifetime.mjs',upstream:'tests/helpers/ws_fetch_upgrade_lifetime_peer.mjs',
    hostBackend:'tests/helpers/ws_host_backend.mjs',clientPeer:'tests/helpers/ws_response_client.mjs',contract:'tools/workerd-fetch-upgrade-lifetime-contract.mjs',
    listenerControl:'tests/helpers/workerd_listener_control.mjs',listenerContract:'tools/workerd-fetch-byob-contract.mjs',
});
const uuid=/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/;
const wireFields=['status','rawHeader','requestKey','rawTail','frames','peerTerminated','transportError','completion','closeReplySent'];
function fields(value,keys,label) {assert.ok(value&&typeof value==='object',label);assert.deepEqual(Object.keys(value).sort(),[...keys].sort(),label);}
function requestHeaders(request) {return request.split('\r\n').slice(1).map(line=>{const colon=line.indexOf(':');assert.ok(colon>0);return [line.slice(0,colon).toLowerCase(),line.slice(colon+1).trim()];}).sort(([a],[b])=>a.localeCompare(b));}
function shortFrame(opcode,payload) {const bytes=Buffer.from(payload);assert.ok(bytes.length<126);return Buffer.concat([Buffer.from([128|opcode,bytes.length]),bytes]);}
function controlHeader(header) {
    fields(header,wireFields,'complete control Upgrade observation');assert.equal(header.status,101);assert.match(header.requestKey,/^[A-Za-z0-9+/]{22}==$/);
    assert.equal(Buffer.from(header.requestKey,'base64').toString('base64'),header.requestKey);
    const lines=header.rawHeader.split('\r\n');assert.equal(lines[0],'HTTP/1.1 101 Switching Protocols');
    assert.deepEqual(requestHeaders(header.rawHeader),[['connection','Upgrade'],['sec-websocket-accept',createHash('sha1').update(header.requestKey+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64')],['upgrade','websocket']]);
    assert.equal(header.rawTail,'');assert.deepEqual(header.frames,[]);assert.equal(header.peerTerminated,false);assert.equal(header.transportError,null);assert.equal(header.completion,null);assert.equal(header.closeReplySent,false);
}
function controlWire(value,role,texts) {
    const header=value.headers[role],wire=value.wires[role];controlHeader(header);fields(wire,wireFields,'complete actual control wire');
    assert.equal(wire.status,101);assert.equal(wire.requestKey,header.requestKey);assert.equal(wire.rawHeader,header.rawHeader);
    const frames=[...texts.map(text=>({opcode:1,payload:Buffer.from(text).toString('base64')})),{opcode:8,payload:'A+hkb25l'}];
    assert.deepEqual(wire.frames,frames,'exact parent messages and Close');assert.equal(wire.rawTail,Buffer.concat(frames.map(frame=>shortFrame(frame.opcode,Buffer.from(frame.payload,'base64')))).toString('base64'));
    assert.equal(wire.closeReplySent,true);assert.equal(wire.peerTerminated,true);assert.ok(['peer-ended','peer-reset'].includes(wire.completion));assert.equal(wire.transportError,wire.completion==='peer-reset'?'ECONNRESET':null);
}
function owner(value,role,stage,mode,observed) {
    const {kind,challenge}=value,index=role==='a'?0:1,target=value.targets[index];
    const result={trace:['go','fetch'],headers:null,first:null,second:null,failure:null,socket:null};
    const two=kind==='two-owners',same=kind.startsWith('same-origin-'),complete=same&&kind.endsWith('-complete');
    const pending=kind==='close-pending'||two&&role==='a'||two&&role==='b'&&['before','closed','checkpoint'].includes(stage)||
        same&&role==='b'&&(!complete||['before','closed','checkpoint'].includes(stage));
    if(complete&&role==='b') {assert.ok(Number.isSafeInteger(observed?.timerTicks)&&observed.timerTicks>=0,'actual timer counter');result.timerTicks=observed.timerTicks;}
    if(two&&role==='b'&&['checkpoint','final'].includes(stage))result.trace.push('checkpoint');
    if(!pending) {
        const socket=kind==='accepted-independent'||kind==='close-unaccepted'||two||same&&role==='a';
        result.trace.push('headers');result.headers={status:socket?101:409,url:`http://localhost:${target.port}/hold?challenge=${challenge}&role=${role}`,bodyNull:socket,bodyUsed:false,webSocket:socket};
        if(socket) {
            const accepted=kind!=='close-unaccepted',messages=accepted?['ready/'+challenge+'/'+role]:[];
            const closed=accepted&&stage==='final'?{code:1000,reason:'child',wasClean:true}:null;
            const events=closed?[{type:'close',state:3,...closed}]:[];
            // The exact normal-Close reference race is also pinned by the
            // constructor and basic Upgrade contracts. Retain its full event;
            // no product backend receives this exception.
            if(mode==='reference'&&closed&&observed?.socket?.events?.length===2)events.unshift({type:'error',state:3,
                message:'Uncaught Error: Network connection lost.',error:'Error: Network connection lost.',errorType:'object',errorIsError:true,errorName:'Error',errorMessage:'Network connection lost.'});
            if(closed)messages.push('after/'+challenge+'/'+role,'echo/'+challenge+'/'+role);
            result.socket={before:1,accepted,messages,events,closed};
        } else if(complete&&role==='b') {
            result.trace.push('complete');result.first={value:'complete/'+challenge+'/'+role,done:true,bodyUsed:true};
        } else if(kind!=='close-unread') {
            result.trace.push('first');result.first={value:'first/'+challenge+'/'+role,done:false,bodyUsed:true};
            if(stage!=='before'&&kind==='cancel-reader')result.trace.push('cancel/before','cancel/after');
            if(stage!=='before'&&kind==='abort-body') {result.trace.push('abort','failure');result.failure={name:'Custom',message:challenge,sameReason:false};}
        }
    }
    if(role==='a'&&stage!=='before'||two&&role==='b'&&stage==='final')result.trace.push('finish','close');
    return result;
}
function state(value,stage,mode,actual) {
    const other=value.kind==='two-owners'||value.kind.startsWith('same-origin-');
    return {kind:value.kind,challenge:value.challenge,owners:{a:owner(value,'a',stage,mode,actual?.owners?.a),b:other?owner(value,'b',stage,mode,actual?.owners?.b):null}};
}
function http(document,realm,expected,label) {fields(document,['status','text'],label);assert.equal(document.status,200);assert.deepEqual(JSON.parse(document.text),{realm,state:expected},label);}
function upstream(records,value,stage,nonces) {
    assert.equal(records.length,value.kind==='two-owners'||value.kind.startsWith('same-origin-')?2:1,'complete actual upstream inventory');
    for(const [index,entry] of records.entries()) {
        const role=index?'b':'a',target=value.targets[index],kind=value.kind,two=kind==='two-owners',same=kind.startsWith('same-origin-'),complete=same&&kind.endsWith('-complete');
        fields(entry,['localAddress','localPort','request','rawRequest','responseStatus','frames','events','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','error'],'complete upstream record');
        assert.equal(entry.localAddress,'127.0.0.1');assert.equal(entry.localPort,target.port);assert.equal(target.host,'localhost');
        assert.equal(entry.request.split('\r\n')[0],`GET /hold?challenge=${value.challenge}&role=${role} HTTP/1.1`);
        assert.equal(Buffer.from(entry.rawRequest,'base64').toString('latin1'),entry.request+'\r\n\r\n');
        const headers=requestHeaders(entry.request),keys=headers.filter(([name])=>name==='sec-websocket-key');assert.equal(keys.length,1);const key=keys[0][1];
        assert.match(key,/^[A-Za-z0-9+/]{22}==$/);assert.equal(Buffer.from(key,'base64').toString('base64'),key);
        assert.deepEqual(headers,[['connection','Upgrade'],['host','localhost:'+target.port],['sec-websocket-key',key],['sec-websocket-version','13'],['upgrade','websocket']]);
        if(stage==='before') {assert.equal(nonces.has(key),false,'fresh actual nonce');nonces.add(key);}
        assert.equal(entry.fixtureDestroyed,false);assert.equal(entry.error,null);assert.equal(entry.peerReset,false,'causal coalesced-header probe ends with actual EOF');
        const pending=kind==='close-pending'||two&&role==='a'||two&&role==='b'&&stage!=='final'||same&&role==='b'&&(!complete||['before','closed','checkpoint'].includes(stage));
        const socket=kind==='accepted-independent'||kind==='close-unaccepted'||two&&role==='b'&&stage==='final'||same&&role==='a';
        assert.equal(entry.responseStatus,pending?null:socket?101:409);
        const completedSocket=stage==='final'&&(kind==='accepted-independent'||two&&role==='b'||same&&role==='a');
        const completedHttp=complete&&role==='b'&&['released','final'].includes(stage);
        const beforeClose=stage==='before'||stage==='closed'&&(kind==='accepted-independent'||two&&role==='b'||same);
        let ended=stage==='final'||stage==='checkpoint'&&two&&role==='a'||completedHttp;
        if(stage==='closed')ended=entry.peerEnded;
        if(stage==='closed'&&['cancel-reader','abort-body'].includes(kind))ended=true;
        if(beforeClose)ended=false;
        assert.equal(entry.peerEnded,ended,'exact upstream lifetime phase');assert.equal(entry.fixtureEnded,completedSocket||completedHttp);
        const events=['request',...(pending||two&&role==='b'||same&&role==='b'?['response-held']:[])];
        if(socket)events.push('upgrade');else if(completedHttp)events.push('response-complete');else if(!pending)events.push('first-body');
        if(completedSocket)events.push('send-after-parent','close-reply');if(ended)events.push('peer-ended');
        assert.deepEqual(entry.events,events,'exact causal upstream events');
        const ready=kind==='accepted-independent'||same&&role==='a',messages=ready?['ready/'+value.challenge+'/'+role]:[];
        if(completedSocket) {if(!ready)messages.push('ready/'+value.challenge+'/'+role);messages.push('echo/'+value.challenge+'/'+role);}
        const payloads=messages.map(text=>({opcode:1,payload:Buffer.from(text).toString('base64')}));
        if(completedSocket)payloads.push({opcode:8,payload:Buffer.concat([Buffer.from([3,232]),Buffer.from('child')]).toString('base64')});
        assert.equal(entry.frames.length,payloads.length,'exact independently received frame inventory');
        entry.frames.forEach((frame,index)=>{fields(frame,['opcode','fin','masked','mask','payload'],'complete masked frame');assert.equal(frame.fin,true);assert.equal(frame.masked,true);assert.match(frame.mask,/^[a-f0-9]{8}$/);
            assert.equal(Buffer.from(frame.payload,'base64').toString('base64'),frame.payload);assert.deepEqual({opcode:frame.opcode,payload:frame.payload},payloads[index]);});
    }
}
function assertAwaitLifetimeCase(value,mode,nonces) {
    const deferred=value.kind.startsWith('await-deferred-'),last=value.kind.endsWith('-last'),open=value.kind==='await-open-parent',abrupt=value.kind.startsWith('await-abrupt')||deferred;
    fields(value,['kind','challenge','targets','initial','header','wire','beforeAction','beforeUpstream','afterAction','actionUpstream',
        'checkpoint','checkpointUpstream','pendingTerminal','afterRelease','releaseUpstream','observed','finalState','upstream','shutdown','stderr'],'complete awaiting-handler case');
    assert.equal(value.targets.length,last?1:2);
    for(const [index,target] of value.targets.entries()) {
        fields(target,['host','port','role'],'await target');assert.equal(target.host,'localhost');assert.equal(target.role,!last&&index===0?'a':'b');
        assert.ok(Number.isInteger(target.port)&&target.port>0&&target.port<=65535);
    }
    fields(value.initial,['status','text'],'initial response');assert.equal(value.initial.status,200);
    const initial=JSON.parse(value.initial.text);fields(initial,['realm','state'],'initial realm');assert.match(initial.realm,uuid);assert.equal(initial.state,null);
    controlHeader(value.header);fields(value.wire,wireFields,'parent termination observation');
    assert.equal(value.wire.status,101);assert.equal(value.wire.requestKey,value.header.requestKey);assert.equal(value.wire.rawHeader,value.header.rawHeader);
    const texts=[...(!last?['socket-ready']:[]),'waiting',...(open?['ack/finish']:[])];
    const frames=texts.map(text=>({opcode:1,payload:Buffer.from(text).toString('base64')}));if(!abrupt)frames.push({opcode:8,payload:'A+hkb25l'});
    assert.deepEqual(value.wire.frames,frames,'actual parent data and optional Close');
    assert.equal(value.wire.rawTail,Buffer.concat(frames.map(frame=>shortFrame(frame.opcode,Buffer.from(frame.payload,'base64')))).toString('base64'));
    assert.equal(value.wire.closeReplySent,open);assert.equal(value.wire.peerTerminated,!abrupt);
    assert.equal(value.wire.transportError,null);assert.equal(value.wire.completion,abrupt?'client-disposed':'peer-ended');
    const stages=[['beforeAction','before'],['afterAction','action'],['checkpoint','checkpoint'],['afterRelease','release'],['observed','final'],['finalState','final']];
    const states=[];
    for(const [key,stage] of stages) {
        const document=value[key];fields(document,['status','text'],'same-realm state');assert.equal(document.status,200);
        const observed=JSON.parse(document.text);fields(observed,['realm','state'],'state realm');assert.equal(observed.realm,initial.realm);
        const state=observed.state;assert.ok(Number.isSafeInteger(state?.timerTicks)&&state.timerTicks>=0,'actual native timer counter');states.push(state);
        const released=(!last||deferred)&&['release','final'].includes(stage),closed=stage!=='before'&&(!open||stage==='final');
        const trace=['go',...(!last?['socket','ready']:[]),'await'];
        if(!open&&closed)trace.push('close');if(released)trace.push('headers','resumed');if(deferred&&released)trace.push('local-close');if(open&&closed)trace.push('finish','close');
        const parentClose=closed?{code:abrupt?1006:1000,reason:abrupt?'WebSocket disconnected without sending Close frame.':'done',wasClean:!abrupt,before:abrupt?1:3,...(deferred?(released?{localBefore:2,localAfter:3}:{}):{after:abrupt?2:3})}:null;
        let socket=null;
        if(!last) {
            const messages=['ready/'+value.challenge+'/a'];if(stage==='final')messages.push('after/'+value.challenge+'/a','echo/'+value.challenge+'/a');
            const closed=stage==='final'?{code:1000,reason:'child',wasClean:true}:null;
            const events=closed?[{type:'close',...closed}]:[];
            if(mode==='reference'&&closed&&state.socket?.events?.length===2)events.unshift({type:'error',state:3,message:'Uncaught Error: Network connection lost.',
                error:'Error: Network connection lost.',errorType:'object',errorIsError:true,errorName:'Error',errorMessage:'Network connection lost.'});
            socket={status:101,accepted:true,messages,events,closed};
        }
        assert.deepEqual(state,{kind:value.kind,challenge:value.challenge,trace,timerTicks:state.timerTicks,parentClose,
            response:released?{status:409,text:'complete/'+value.challenge+'/b',bodyUsed:true}:null,failure:null,socket},'exact asynchronous handler continuation and endpoint state');
    }
    const ticks=states.map(state=>state.timerTicks);assert.ok(ticks[1]>=ticks[0]);
    if(last&&!deferred)assert.ok(ticks.slice(1).every(tick=>tick===ticks[1]),'last acknowledged endpoint cancels the native timer');
    else assert.ok(ticks[2]>ticks[1]&&ticks[3]>=ticks[2]&&ticks[4]>=ticks[3]&&ticks[5]===ticks[4],'timer survives parent closure and stops after final child closure');
    assert.deepEqual(states[4],states[5],'quiescent same-realm final state');assert.equal(value.pendingTerminal,last&&!deferred,'pending fetch remains owned by its incoming local direction or promoted sibling');
    for(const [key,stage] of [['beforeUpstream','before'],['actionUpstream','action'],['checkpointUpstream','checkpoint'],['releaseUpstream','release'],['upstream','final']]) {
        const entries=value[key];assert.equal(entries.length,value.targets.length,'exact upstream inventory');
        for(const [index,entry] of entries.entries()) {
            const target=value.targets[index],socket=target.role==='a',ended=socket?stage==='final':stage!=='before'&&(last&&!deferred||['release','final'].includes(stage));
            fields(entry,['localAddress','localPort','request','rawRequest','responseStatus','frames','events','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','error'],'actual independent upstream record');
            assert.equal(entry.localAddress,'127.0.0.1');assert.equal(entry.localPort,target.port);
            assert.equal(entry.request.split('\r\n')[0],`GET /hold?challenge=${value.challenge}&role=${target.role} HTTP/1.1`);
            assert.equal(Buffer.from(entry.rawRequest,'base64').toString('latin1'),entry.request+'\r\n\r\n');
            const headers=requestHeaders(entry.request),keys=headers.filter(([name])=>name==='sec-websocket-key');assert.equal(keys.length,1);
            const nonce=keys[0][1];assert.match(nonce,/^[A-Za-z0-9+/]{22}==$/);assert.equal(Buffer.from(nonce,'base64').toString('base64'),nonce);
            assert.deepEqual(headers,[['connection','Upgrade'],['host','localhost:'+target.port],['sec-websocket-key',nonce],['sec-websocket-version','13'],['upgrade','websocket']]);
            if(stage==='before'){assert.equal(nonces.has(nonce),false,'fresh actual nonce');nonces.add(nonce);}
            assert.equal(entry.peerEnded,ended);assert.equal(entry.peerReset,false);assert.equal(entry.fixtureDestroyed,false);assert.equal(entry.error,null);
            const released=!socket&&(!last||deferred)&&['release','final'].includes(stage);
            assert.equal(entry.responseStatus,socket?101:released?409:null);assert.equal(entry.fixtureEnded,socket?stage==='final':released);
            assert.deepEqual(entry.events,['request',socket?'upgrade':'response-held',...(released?['response-complete']:[]),...(socket&&ended?['send-after-parent','close-reply']:[]),...(ended?['peer-ended']:[])]);
            const payloads=socket?[{opcode:1,payload:Buffer.from('ready/'+value.challenge+'/a').toString('base64')}]:[];
            if(socket&&ended)payloads.push({opcode:1,payload:Buffer.from('echo/'+value.challenge+'/a').toString('base64')},{opcode:8,payload:'A+hjaGlsZA=='});
            assert.equal(entry.frames.length,payloads.length);
            entry.frames.forEach((frame,index)=>{fields(frame,['opcode','fin','masked','mask','payload'],'complete masked frame');assert.equal(frame.fin,true);assert.equal(frame.masked,true);assert.match(frame.mask,/^[a-f0-9]{8}$/);
                assert.deepEqual({opcode:frame.opcode,payload:frame.payload},payloads[index]);});
            assert.deepEqual([entry.localAddress,entry.localPort,entry.request,entry.rawRequest],
                [value.beforeUpstream[index].localAddress,value.beforeUpstream[index].localPort,value.beforeUpstream[index].request,value.beforeUpstream[index].rawRequest],'immutable request identity');
            assert.deepEqual(entry.frames,value.upstream[index].frames.slice(0,entry.frames.length),'immutable received frame prefix');
        }
    }
    fields(value.shutdown,['code','signal','forced'],'shutdown');assert.equal(value.shutdown.forced,false);
    if(mode==='reference')assert.ok(value.shutdown.code===0||value.shutdown.code===null&&value.shutdown.signal==='SIGTERM');else assert.deepEqual(value.shutdown,{code:0,signal:null,forced:false});
    assert.equal(typeof value.stderr,'string');assert.doesNotMatch(value.stderr,/AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON/);
    return {kind:value.kind,owners:value.targets.length,independent:!last};
}
function assertFetchedDisconnectCase(value,mode,nonces) {
    fields(value,['kind','challenge','target','initial','header','wire','before','upstreamBefore','upstreamAction','observed','finalState','upstream','shutdown','stderr'],'complete fetched-disconnect case');
    fields(value.target,['host','port'],'actual fetch target');assert.equal(value.target.host,'localhost');assert.ok(Number.isInteger(value.target.port)&&value.target.port>0&&value.target.port<=65535);
    fields(value.initial,['status','text'],'initial state');assert.equal(value.initial.status,200);const initial=JSON.parse(value.initial.text);fields(initial,['realm','state'],'initial realm');assert.match(initial.realm,uuid);assert.equal(initial.state,null);
    controlWire({headers:{a:value.header},wires:{a:value.wire}},'a',['socket-ready','closed']);
    const expectedBase={kind:value.kind,challenge:value.challenge,status:101,beforeAccept:1,trace:['headers','ready/'+value.challenge+'/a'],close:null,states:null,errors:[],failure:null};
    for(const [key,final] of [['before',false],['observed',true],['finalState',true]]) {
        fields(value[key],['status','text'],'same-realm response');assert.equal(value[key].status,200);
        const document=JSON.parse(value[key].text);fields(document,['realm','state'],'same-realm state');assert.equal(document.realm,initial.realm);
        assert.deepEqual(document.state,{...expectedBase,...(final?{close:{code:1006,reason:'WebSocket disconnected without sending Close frame.',wasClean:false},
            states:{before:1,after:1,microtask:2,timer:2}}:{})},'fetched endpoint retains outgoing close semantics after transport loss');
    }
    assert.deepEqual(JSON.parse(value.observed.text),JSON.parse(value.finalState.text),'quiescent fetched-disconnect state');
    assert.deepEqual(value.upstreamAction,{kind:'fixture-initiated-transport-disposal'},'intentional upstream action is explicit');
    assert.equal(value.upstreamBefore.length,1);assert.deepEqual(value.upstream,value.upstreamBefore,'local fixture disposal does not invent remote EOF or Close bytes');
    const entry=value.upstream[0];fields(entry,['localAddress','localPort','request','rawRequest','responseStatus','frames','events','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','error'],'independent upstream observation');
    assert.equal(entry.localAddress,'127.0.0.1');assert.equal(entry.localPort,value.target.port);assert.equal(entry.request.split('\r\n')[0],`GET /hold?challenge=${value.challenge}&role=a HTTP/1.1`);
    assert.equal(Buffer.from(entry.rawRequest,'base64').toString('latin1'),entry.request+'\r\n\r\n');
    const headers=requestHeaders(entry.request),keys=headers.filter(([name])=>name==='sec-websocket-key');assert.equal(keys.length,1);const nonce=keys[0][1];
    assert.match(nonce,/^[A-Za-z0-9+/]{22}==$/);assert.equal(Buffer.from(nonce,'base64').toString('base64'),nonce);assert.equal(nonces.has(nonce),false,'fresh fetched handshake nonce');nonces.add(nonce);
    assert.deepEqual(headers,[['connection','Upgrade'],['host','localhost:'+value.target.port],['sec-websocket-key',nonce],['sec-websocket-version','13'],['upgrade','websocket']]);
    assert.equal(entry.responseStatus,101);assert.deepEqual(entry.events,['request','upgrade']);assert.equal(entry.peerEnded,false);assert.equal(entry.peerReset,false);assert.equal(entry.fixtureEnded,false);assert.equal(entry.fixtureDestroyed,false);assert.equal(entry.error,null);
    assert.equal(entry.frames.length,1);const frame=entry.frames[0];fields(frame,['opcode','fin','masked','mask','payload'],'complete received masked frame');
    assert.equal(frame.opcode,1);assert.equal(frame.fin,true);assert.equal(frame.masked,true);assert.match(frame.mask,/^[a-f0-9]{8}$/);assert.equal(frame.payload,Buffer.from('ready/'+value.challenge+'/a').toString('base64'));
    fields(value.shutdown,['code','signal','forced'],'shutdown');assert.equal(value.shutdown.forced,false);
    if(mode==='reference')assert.ok(value.shutdown.code===0||value.shutdown.code===null&&value.shutdown.signal==='SIGTERM');else assert.deepEqual(value.shutdown,{code:0,signal:null,forced:false});
    assert.equal(typeof value.stderr,'string');assert.doesNotMatch(value.stderr,/AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON/);
    return {kind:value.kind,owners:1,independent:true};
}

export function assertFetchUpgradeLifetimeCase(value,mode,nonces=new Set()) {
    assert.ok(['reference','single-worker','static-pool','managed'].includes(mode));assert.ok(fetchUpgradeLifetimeCases.includes(value.kind));assert.match(value.challenge,uuid);
    if(value.kind==='fetched-disconnect-close')return assertFetchedDisconnectCase(value,mode,nonces);
    if(value.kind.startsWith('await-'))return assertAwaitLifetimeCase(value,mode,nonces);
    const same=value.kind.startsWith('same-origin-'),complete=same&&value.kind.endsWith('-complete');
    fields(value,['kind','challenge','targets','initial','headers','wires','checkpoint','checkpointUpstream','beforeAction','beforeUpstream','closedParent','closedUpstream','terminalAfterParent','observed','finalState','upstream','shutdown','stderr',
        ...(complete?['afterRelease','afterReleaseUpstream']:[])],'complete lifetime case');
    assert.equal(value.targets.length,value.kind==='two-owners'||same?2:1);for(const target of value.targets) {fields(target,['host','port'],'target');assert.equal(target.host,'localhost');assert.ok(Number.isInteger(target.port)&&target.port>0&&target.port<=65535);}
    fields(value.initial,['status','text'],'initial response');assert.equal(value.initial.status,200);const initial=JSON.parse(value.initial.text);fields(initial,['realm','state'],'initial realm');assert.match(initial.realm,uuid);assert.equal(initial.state,null);
    fields(value.headers,value.kind==='two-owners'?['a','b']:['a'],'exact control owners');fields(value.wires,value.kind==='two-owners'?['a','b']:['a'],'exact control completions');
    const messages=['started',...(['close-body','cancel-reader','abort-body'].includes(value.kind)?['reading']:[]),
        ...(['close-unread','close-unaccepted'].includes(value.kind)?['headers']:[]),...(value.kind==='accepted-independent'||same?['socket-ready']:[]),
        ...(value.kind==='cancel-reader'?['cancelled']:[]),...(value.kind==='abort-body'?['ack/abort','failed']:[]),'ack/finish'];
    controlWire(value,'a',messages);
    const check=(document,stage,label)=>http(document,initial.realm,state(value,stage,mode,JSON.parse(document.text).state),label);
    check(value.beforeAction,'before','before parent action');upstream(value.beforeUpstream,value,'before',nonces);
    check(value.closedParent,'closed','closed parent state');upstream(value.closedUpstream,value,'closed',nonces);
    assert.equal(value.terminalAfterParent,value.kind!=='accepted-independent'&&!same,'last promoted connection retires its I/O origin resources');
    if(value.kind==='two-owners'||same) {if(!same)controlWire(value,'b',['started','ack/checkpoint','socket-ready','ack/finish']);check(value.checkpoint,'checkpoint','remaining owner retains its I/O origin');upstream(value.checkpointUpstream,value,'checkpoint',nonces);}
    else {assert.equal(value.checkpoint,null);assert.equal(value.checkpointUpstream,null);}
    if(complete) {check(value.afterRelease,'released','original continuation consumes the released response');upstream(value.afterReleaseUpstream,value,'released',nonces);}
    const observed=JSON.parse(value.observed.text).state,finalState=JSON.parse(value.finalState.text).state;
    http(value.observed,initial.realm,state(value,'final',mode,observed),'complete final state');http(value.finalState,initial.realm,state(value,'final',mode,finalState),'quiescent final state');assert.deepEqual(observed,finalState);
    if(complete) {
        const ticks=[value.beforeAction,value.closedParent,value.checkpoint,value.afterRelease,value.observed,value.finalState].map(document=>JSON.parse(document.text).state.owners.b.timerTicks);
        assert.ok(ticks[1]>=ticks[0]&&ticks[2]>ticks[1]&&ticks[3]>=ticks[2]&&ticks[4]>=ticks[3]&&ticks[5]===ticks[4],
            'native timer continues across parent close, then stops when the last promoted connection closes');
    }
    upstream(value.upstream,value,'final',nonces);fields(value.shutdown,['code','signal','forced'],'shutdown');assert.equal(value.shutdown.forced,false);
    for(const snapshot of [value.closedUpstream,value.checkpointUpstream,value.afterReleaseUpstream,value.upstream].filter(Boolean)) {
        assert.deepEqual(snapshot.map(entry=>[entry.localAddress,entry.localPort,entry.request,entry.rawRequest]),
            value.beforeUpstream.map(entry=>[entry.localAddress,entry.localPort,entry.request,entry.rawRequest]),'one immutable actual request per connection');
        snapshot.forEach((entry,index)=>assert.deepEqual(entry.frames,value.upstream[index].frames.slice(0,entry.frames.length),'observed frame prefixes remain immutable'));
    }
    if(mode==='reference')assert.ok(value.shutdown.code===0||value.shutdown.code===null&&value.shutdown.signal==='SIGTERM');else assert.deepEqual(value.shutdown,{code:0,signal:null,forced:false});
    assert.equal(typeof value.stderr,'string');assert.doesNotMatch(value.stderr,/AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON/);
    return {kind:value.kind,owners:value.targets.length,independent:value.kind==='accepted-independent'||value.kind==='two-owners'||same};
}
export function validateFetchUpgradeLifetimeReport(report,expected) {
    fields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','cases'],'complete lifetime report');
    assert.equal(report.schema,'capsid-fetch-upgrade-lifetime-v1');assert.equal(report.complete,true);assert.equal(report.runId,expected.runId);assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));assert.deepEqual(report.referenceLock,expected.lock);
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});
    assert.deepEqual(report.inputs,expected.inputs);assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(fetchUpgradeLifetimeInputPaths).sort());for(const digest of Object.values(report.inputs))assert.match(digest,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,worker:report.mode==='reference'?null:expected.worker});
    assert.deepEqual(report.cases.map(value=>value.kind),fetchUpgradeLifetimeCases);assert.equal(new Set(report.cases.map(value=>value.challenge)).size,fetchUpgradeLifetimeCases.length);
    assert.equal(new Set(report.cases.map(value=>JSON.parse(value.initial.text).realm)).size,fetchUpgradeLifetimeCases.length);
    const nonces=new Set();return report.cases.map(value=>assertFetchUpgradeLifetimeCase(value,report.mode,nonces));
}
export function compareFetchUpgradeLifetimeReports(reference,candidates,expected) {
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(value=>value.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateFetchUpgradeLifetimeReport(reference,expected);
    for(const report of candidates)assert.deepEqual(validateFetchUpgradeLifetimeReport(report,expected),baseline);
    return {complete:true,casesPerBackend:fetchUpgradeLifetimeCases.length,backends:candidates.length};
}
