import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const fetchProxyCases=Object.freeze(['direct','rewrap','upstream-first','large-binary',
    'client-abrupt','upstream-abrupt','server-close','accepted','reuse','cross-origin','slow-upstream',
    'upstream-first-binary','intrinsic-traps','source-lifetime','source-live-sibling','source-http',
    'source-parked-promise','source-http-request','source-http-object','request-info-upgrade-url',
    'request-info-upgrade-request','foreign-methods']);
export const fetchProxyInputPaths=Object.freeze({runner:'tests/test_websocket_fetch_proxy.mjs',
    fixture:'tests/fixtures/workerd-fetch-proxy.mjs',upstream:'tests/helpers/ws_fetch_proxy_peer.mjs',
    clientPeer:'tests/helpers/ws_fetch_proxy_client.mjs',hostBackend:'tests/helpers/ws_host_backend.mjs',
    contract:'tools/workerd-fetch-proxy-contract.mjs',
    listenerControl:'tests/helpers/workerd_listener_control.mjs',listenerContract:'tools/workerd-fetch-byob-contract.mjs',
});

const uuid=/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/;
const wireFields=['status','rawHeader','requestKey','rawTail','frames','peerTerminated','transportError','completion','closeReplySent'];
const caseFields=['kind','challenge','target','initial','headers','echo','early','earlyBinary','expectedBinary',
    'actualBinary','wire','secondHeaders','secondWire','parked','mark','checkpoint','checkpointUpstream','wake',
    'postWake','foreign','pauseCheckpoint','upstreamAfterClientEnd','observed','upstreamBeforeStop','shutdown',
    'stderr','upstreamAfterStop','failure','cleanupFailure'];
const crossRequest="Cannot perform I/O on behalf of a different request. I/O objects (such as streams, request/response bodies, and others) created in the context of one request handler cannot be accessed from a different request's handler. This is a limitation of Cloudflare Workers which allows us to improve overall performance. (I/O type: Native)";
const releasedAccept="Can't accept() WebSocket that was already used in a response.";
const fields=(value,keys,label)=>{
    assert.ok(value&&typeof value==='object'&&!Array.isArray(value),label);
    assert.deepEqual(Object.keys(value).sort(),[...keys].sort(),label);
};
function bytes(value) {
    assert.equal(typeof value,'string');const result=Buffer.from(value,'base64');
    assert.equal(result.toString('base64'),value,'canonical captured bytes');return result;
}
function nonce(value,seen) {
    assert.match(value,/^[A-Za-z0-9+/]{22}==$/);assert.equal(bytes(value).length,16);
    if(seen){assert.ok(!seen.has(value),'independent actual Upgrade nonce');seen.add(value);}
}
function headers(raw) {
    assert.equal(typeof raw,'string');return raw.split('\r\n').slice(1).map(line=>{
        const at=line.indexOf(':');assert.ok(at>0);return [line.slice(0,at).toLowerCase(),line.slice(at+1).trim()];
    }).sort(([a],[b])=>a.localeCompare(b));
}
const accept=key=>createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
function upgradeHeader(raw,key,challenge) {
    assert.equal(raw.split('\r\n')[0],'HTTP/1.1 101 Switching Protocols');
    assert.deepEqual(headers(raw),[['connection','Upgrade'],['sec-websocket-accept',accept(key)],
        ['set-cookie','first=1'],['set-cookie','second=2'],['upgrade','websocket'],['x-probe',challenge]]);
}
function frame(opcode,payload) {
    const data=Buffer.from(payload);let head;
    if(data.length<126)head=Buffer.from([128|opcode,data.length]);
    else if(data.length<=65535){head=Buffer.alloc(4);head[0]=128|opcode;head[1]=126;head.writeUInt16BE(data.length,2);}
    else{head=Buffer.alloc(10);head[0]=128|opcode;head[1]=127;head.writeBigUInt64BE(BigInt(data.length),2);}
    return Buffer.concat([head,data]);
}
const message=(opcode,payload)=>({opcode,payload:Buffer.from(payload).toString('base64')});
function exactMessages(actual,expected,label) {
    assert.ok(Array.isArray(actual),label);assert.equal(actual.length,expected.length,label+' count');
    for(let index=0;index<expected.length;index++){
        fields(actual[index],['opcode','payload'],label+' fields');
        assert.equal(actual[index].opcode,expected[index].opcode,label+' opcode '+index);
        assert.ok(actual[index].payload===expected[index].payload,label+' complete payload '+index);
    }
}
const close=(code,reason)=>{const codeBytes=Buffer.alloc(2);codeBytes.writeUInt16BE(code);return message(8,Buffer.concat([codeBytes,Buffer.from(reason)]));};
function payload(challenge,index=0) {
    const data=Buffer.alloc(180000);
    for(let at=0;at<data.length;at++)data[at]=(at*17+index*31+challenge.charCodeAt(at%challenge.length))%251;
    return data;
}
const binaryDigest=data=>({bytes:data.length,sha256:createHash('sha256').update(data).digest('hex')});
function downstream(value) {
    const {kind,challenge}=value;
    if(['accepted','cross-origin'].includes(kind))return [];
    const result=[];
    if(kind==='upstream-first')result.push(message(1,'early/'+challenge));
    if(kind==='upstream-first-binary')result.push(message(2,payload(challenge)));
    if(kind==='large-binary'||kind==='slow-upstream')for(let index=0;index<(kind==='slow-upstream'?12:1);index++)result.push(message(2,payload(challenge,index)));
    else result.push(message(1,challenge));
    if(!['client-abrupt','upstream-abrupt'].includes(kind))result.push(kind==='server-close'?close(1001,'bye/'+challenge):close(1000,'done'));
    return result;
}
function wire(header,observed,value,seen,status=101) {
    fields(header,wireFields,'captured HTTP response');fields(observed,wireFields,'captured final wire');
    nonce(header.requestKey,seen);assert.equal(header.status,status);assert.equal(observed.status,status);
    assert.equal(header.requestKey,observed.requestKey);assert.equal(header.rawHeader,observed.rawHeader);
    assert.deepEqual(header.frames,[]);assert.equal(header.peerTerminated,false);assert.equal(header.transportError,null);
    assert.equal(header.completion,null);assert.equal(header.closeReplySent,false);
    if(status===500){
        // The servers use different HTTP error-body framing. The failure is
        // required before Upgrade; no WebSocket data or Close may be emitted.
        assert.equal(header.rawHeader.split('\r\n')[0],'HTTP/1.1 500 Internal Server Error');
        assert.ok(headers(header.rawHeader).every(([name])=>!['upgrade','sec-websocket-accept'].includes(name)));
        bytes(header.rawTail);bytes(observed.rawTail);assert.deepEqual(observed.frames,[]);
        assert.equal(observed.completion,'http-status');assert.equal(observed.peerTerminated,false);
        assert.equal(observed.transportError,null);assert.equal(observed.closeReplySent,false);return;
    }
    upgradeHeader(header.rawHeader,header.requestKey,value.challenge);
    const expected=downstream(value);exactMessages(observed.frames,expected,'exact server message bytes and Close');
    const raw=Buffer.concat(expected.map(item=>frame(item.opcode,bytes(item.payload))));
    assert.ok(bytes(observed.rawTail).equals(raw),'complete canonical unmasked server frames');
    const prefix=bytes(header.rawTail);assert.ok(prefix.equals(raw.subarray(0,prefix.length)),'header snapshot contains only a real wire prefix');
    assert.equal(observed.closeReplySent,value.kind==='server-close');
    if(value.kind==='client-abrupt'){
        assert.equal(observed.peerTerminated,false);assert.equal(observed.transportError,null);assert.equal(observed.completion,'client-disposed');
    }else{
        assert.equal(observed.peerTerminated,true);assert.ok(['peer-ended','peer-reset'].includes(observed.completion));
        assert.equal(observed.transportError,observed.completion==='peer-reset'?'ECONNRESET':null);
    }
}
function state(value,stage) {
    const {kind,challenge}=value,source=kind.startsWith('source-'),sibling=kind==='source-live-sibling';
    const result={kind:kind==='cross-origin'?'park':kind,status:101,hasSocket:true,accepted:kind==='accepted',
        reused:kind==='cross-origin'||kind==='reuse',inputConversions:kind.startsWith('request-info-')?1:0,
        initOrder:kind.startsWith('request-info-')?['input','headers']:[]};
    if(kind==='intrinsic-traps')result.traps=0;
    if(sibling)Object.assign(result,{childClosed:stage==='mark'?0:1,childTimerFired:stage==='mark'?0:1,
        afterChildTimerFired:0,childEcho:stage==='mark'?null:'sibling/'+challenge,childFailure:null});
    if(kind.startsWith('source-http')){
        Object.assign(result,{sideInputConversions:1,sideInitOrder:kind==='source-http-object'?['string']:
            kind==='source-http-request'?['input','headers']:['input']});
        if(kind==='source-http-object')result.rejection={synchronous:false,identity:true,calls:1,hints:['string'],returnedPromise:true};
        Object.assign(result,{heldStatus:200,heldBodyFinished:false,heldBodyFailure:false});
    }
    if(kind==='source-parked-promise')Object.assign(result,{future:0,futureRequested:['wake','postWake','observed'].includes(stage)});
    if(source)Object.assign(result,{timerFired:sibling&&stage!=='mark'?1:0,timerDelayMs:500,listenerEvents:[],sawHeaderBeforeTimer:true});
    if(sibling&&stage!=='mark')result.afterTransfer={readyState:{returned:1},accept:{error:{name:'TypeError',message:releasedAccept}},
        send:{returned:{type:'undefined'}},close:{returned:{type:'undefined'}},readyStateAfterClose:1};
    if(kind==='foreign-methods')result.foreign=Object.fromEntries(['readyState','accept','send','close'].map(operation=>
        [operation,{error:{name:'Error',message:crossRequest}}]));
    return result;
}
function document(observed,realm,expected,label) {
    fields(observed,['status','text'],label);assert.equal(observed.status,200);
    assert.deepEqual(JSON.parse(observed.text),{realm,state:expected},label);
}
function messages(frames) {
    assert.ok(Array.isArray(frames));const result=[];let partial=null;
    for(const item of frames){
        fields(item,['opcode','fin','masked','mask','payload'],'actual masked upstream frame');
        assert.equal(item.masked,true);assert.match(item.mask,/^[a-f0-9]{8}$/);assert.equal(typeof item.fin,'boolean');
        const data=bytes(item.payload);assert.ok(data.length<=1048576);
        if(item.opcode===8){assert.equal(item.fin,true);assert.ok(data.length<=125);assert.equal(partial,null);result.push(message(8,data));continue;}
        if(item.opcode!==0){assert.ok([1,2].includes(item.opcode));assert.equal(partial,null);partial={opcode:item.opcode,chunks:[]};}
        assert.ok(partial,'continuation belongs to an actual message');partial.chunks.push(data);
        if(item.fin){result.push(message(partial.opcode,Buffer.concat(partial.chunks)));partial=null;}
    }
    assert.equal(partial,null,'complete observed client messages');return result;
}
function upstream(records,value,stage,seen) {
    const {kind,challenge,target}=value,sibling=kind==='source-live-sibling',held=kind.startsWith('source-http');
    assert.ok(Array.isArray(records));assert.equal(records.length,sibling||held?2:1,'actual independent source connections');
    for(const [index,item] of records.entries()){
        const main=index===0,http=held&&!main,keys=['localAddress','localPort','request','rawRequest','frames','sentFrames',
            'peerEnded','peerReset','fixtureEnded','fixtureDestroyed','responseStatus','responseHeader','events','error'];
        if(kind==='slow-upstream')keys.push('paused',...(stage==='pause'?[]:['resumed']));
        if(kind==='server-close')keys.push('initialCloseSent');
        fields(item,keys,'complete source transport observation');
        assert.equal(item.localAddress,'127.0.0.1');assert.equal(item.localPort,target.port);assert.equal(item.error,null);
        const route=http?'/held-http':'/proxy',suffix=sibling&&!main?'&sibling=1':'';
        assert.equal(item.request.split('\r\n')[0],`GET ${route}?challenge=${challenge}${suffix} HTTP/1.1`);
        assert.deepEqual(bytes(item.rawRequest),Buffer.from(item.request+'\r\n\r\n','latin1'));
        const actualHeaders=headers(item.request),expectedHeaders=[['host','localhost:'+target.port]];
        if(!http){
            const key=actualHeaders.find(([name])=>name==='sec-websocket-key')?.[1];nonce(key,seen);
            expectedHeaders.push(['connection','Upgrade'],['sec-websocket-key',key],['sec-websocket-version','13'],['upgrade','websocket']);
            upgradeHeader(item.responseHeader,key,challenge);
        }else assert.equal(item.responseHeader,'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close');
        assert.deepEqual(actualHeaders,expectedHeaders.sort(([a],[b])=>a.localeCompare(b)));
        assert.equal(item.responseStatus,http?200:101);
        let outgoing=[],incoming=[];
        if(!main&&!http){outgoing=[message(1,'sibling/'+challenge),close(1000,'sibling done')];incoming=outgoing;}
        else if(main&&stage!=='checkpoint'&&stage!=='pause'){
            incoming=downstream(value);outgoing=incoming.filter((_,at)=>!(at===0&&['upstream-first','upstream-first-binary'].includes(kind)));
        }
        exactMessages(messages(item.frames),outgoing,'exact upstream message and Close bytes');
        exactMessages(item.sentFrames,incoming,'actual fixture writes');
        const accepted=kind==='accepted',abrupt=kind==='upstream-abrupt',checkpointMain=main&&['checkpoint','pause'].includes(stage);
        const ended=http||!main||!accepted&&!abrupt&&!checkpointMain||accepted&&stage==='after-stop';
        assert.equal(typeof item.peerEnded,'boolean');assert.equal(typeof item.peerReset,'boolean');
        assert.equal(item.peerEnded||item.peerReset,ended,'actual peer terminal state at the required checkpoint');
        assert.equal(item.fixtureDestroyed,abrupt);
        const fixtureEnded=!http&&!checkpointMain&&!['accepted','client-abrupt','upstream-abrupt','cross-origin'].includes(kind);
        assert.equal(item.fixtureEnded,fixtureEnded,'peer EOF is not inferred from fixture teardown');
        const events=['request',http?'held-http':'upgrade'];
        if(fixtureEnded)events.push('close-reply');if(item.peerEnded)events.push('peer-ended');
        assert.deepEqual(item.events,events);
        if(kind==='slow-upstream'){assert.equal(item.paused,true);if(stage!=='pause')assert.equal(item.resumed,true);}
        if(kind==='server-close')assert.equal(item.initialCloseSent,true);
    }
}
export function assertFetchProxyCase(value,mode,seen=new Set()) {
    fields(value,caseFields,'complete proxy case');assert.ok(fetchProxyCases.includes(value.kind));assert.match(value.challenge,uuid);
    assert.equal(value.failure,null);assert.equal(value.cleanupFailure,null);
    fields(value.target,['host','port'],'source authority');assert.equal(value.target.host,'localhost');
    assert.ok(Number.isSafeInteger(value.target.port)&&value.target.port>0&&value.target.port<65536);
    fields(value.initial,['status','text'],'initial VM');assert.equal(value.initial.status,200);
    const initial=JSON.parse(value.initial.text);fields(initial,['realm','state'],'initial realm');assert.match(initial.realm,uuid);assert.equal(initial.state,null);
    const realm=initial.realm,kind=value.kind,source=kind.startsWith('source-'),binary=['large-binary','slow-upstream'].includes(kind);
    wire(value.headers,value.wire,value,seen,kind==='cross-origin'?500:101);
    if(kind==='reuse')wire(value.secondHeaders,value.secondWire,value,seen,500);
    else{assert.equal(value.secondHeaders,null);assert.equal(value.secondWire,null);}
    if(kind==='cross-origin')assert.deepEqual(value.parked,{status:200,text:'parked'});else assert.equal(value.parked,null);
    assert.equal(value.echo,binary||['accepted','cross-origin'].includes(kind)?null:value.challenge);
    assert.equal(value.early,kind==='upstream-first'?'early/'+value.challenge:null);
    assert.deepEqual(value.earlyBinary,kind==='upstream-first-binary'?binaryDigest(payload(value.challenge)):null);
    const expectedBinary=binary?Array.from({length:kind==='slow-upstream'?12:1},(_,index)=>binaryDigest(payload(value.challenge,index))):null;
    assert.deepEqual(value.expectedBinary,expectedBinary);assert.deepEqual(value.actualBinary,expectedBinary);
    for(const key of ['mark','checkpoint']){
        if(source)document(value[key],realm,state(value,key),key);else assert.equal(value[key],null);
    }
    if(source)upstream(value.checkpointUpstream,value,'checkpoint');else assert.equal(value.checkpointUpstream,null);
    for(const key of ['wake','postWake']){
        if(kind==='source-parked-promise')document(value[key],realm,state(value,key),key);else assert.equal(value[key],null);
    }
    if(kind==='foreign-methods')document(value.foreign,realm,state(value,'foreign'),'foreign request');else assert.equal(value.foreign,null);
    if(kind==='slow-upstream')upstream(value.pauseCheckpoint,value,'pause');else assert.equal(value.pauseCheckpoint,null);
    if(kind==='client-abrupt')upstream(value.upstreamAfterClientEnd,value,'before-stop');else assert.equal(value.upstreamAfterClientEnd,null);
    document(value.observed,realm,state(value,'observed'),'same VM after exchange');
    upstream(value.upstreamBeforeStop,value,'before-stop',seen);upstream(value.upstreamAfterStop,value,'after-stop');
    for(const key of ['checkpointUpstream','pauseCheckpoint','upstreamAfterClientEnd','upstreamAfterStop'])if(value[key])
        for(const [index,item] of value[key].entries())for(const field of ['localAddress','localPort','request','rawRequest','responseStatus','responseHeader'])
            assert.deepEqual(item[field],value.upstreamBeforeStop[index][field],'source connection identity is stable across checkpoints');
    fields(value.shutdown,['code','signal','forced'],'normal service shutdown');assert.equal(value.shutdown.forced,false);
    if(mode==='reference')assert.ok(value.shutdown.code===0&&value.shutdown.signal===null||value.shutdown.code===null&&value.shutdown.signal==='SIGTERM');
    else assert.deepEqual(value.shutdown,{code:0,signal:null,forced:false});
    assert.equal(typeof value.stderr,'string');assert.doesNotMatch(value.stderr,/AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON|worker_crash|terminal continuation/);
    return {kind,messages:downstream(value).filter(item=>item.opcode!==8).length,status:value.headers.status,source};
}
export function validateFetchProxyReport(report,expected) {
    fields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','cases'],'complete proxy report');
    assert.equal(report.schema,'capsid-fetch-proxy-v1');assert.equal(report.complete,true);
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.equal(typeof report.runId,'string');assert.ok(report.runId.length>0);assert.equal(report.runId,expected.runId);
    assert.deepEqual(report.referenceLock,expected.lock);assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});
    assert.deepEqual(report.inputs,expected.inputs);assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(fetchProxyInputPaths).sort());
    for(const digest of Object.values(report.inputs))assert.match(digest,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,worker:report.mode==='reference'?null:expected.worker});
    assert.match(report.binaries.host,/^[a-f0-9]{64}$/);
    if(report.mode!=='reference'){
        assert.match(report.binaries.worker,/^[a-f0-9]{64}$/);
        assert.notEqual(report.binaries.host,expected.lock.binary_sha256,'product cannot alias the reference executable');
        assert.notEqual(report.binaries.worker,expected.lock.binary_sha256);
        assert.notEqual(report.binaries.host,report.binaries.worker);
    }
    assert.deepEqual(report.cases.map(value=>value.kind),fetchProxyCases,'fixed complete case inventory');
    assert.equal(new Set(report.cases.map(value=>value.challenge)).size,fetchProxyCases.length);
    assert.equal(new Set(report.cases.map(value=>JSON.parse(value.initial.text).realm)).size,fetchProxyCases.length);
    const seen=new Set();return report.cases.map(value=>assertFetchProxyCase(value,report.mode,seen));
}
export function compareFetchProxyReports(reference,candidates,expected) {
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(value=>value.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateFetchProxyReport(reference,expected);
    for(const report of candidates)assert.deepEqual(validateFetchProxyReport(report,expected),baseline);
    return {complete:true,casesPerBackend:fetchProxyCases.length,backends:candidates.length};
}
