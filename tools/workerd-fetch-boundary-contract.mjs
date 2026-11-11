import assert from 'node:assert/strict';
import {createHash,X509Certificate} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const fetchBoundaryCases=Object.freeze(['tls-upgrade','tls-proxy','tls-ip-upgrade','tls-wrong-host',
    'tls-untrusted','tls-http','ipv6-upgrade','redirect-error','redirect-loop','redirect-cross',
    'redirect-denied','redirect-held-body','body-empty','body-eof','body-truncated','body-invalid-chunk',
    'body-truncated-coalesced','body-invalid-chunk-coalesced','body-204','body-205','body-304','request-redirect-values']);
export const fetchBoundaryInputPaths=Object.freeze({runner:'tests/test_websocket_fetch_boundaries.mjs',
    fixture:'tests/fixtures/workerd-fetch-boundary.mjs',upstream:'tests/helpers/ws_fetch_boundary_peer.mjs',
    controlPeer:'tests/helpers/ws_response_client.mjs',proxyPeer:'tests/helpers/ws_fetch_proxy_client.mjs',
    hostBackend:'tests/helpers/ws_host_backend.mjs',contract:'tools/workerd-fetch-boundary-contract.mjs'});
const redirectError='Invalid redirect value, must be one of "follow" or "manual" ("error" won\'t be implemented since it does not make sense at the edge; use "manual" and check the response status code).';
const uuid=/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
const accept=key=>createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
const sorted=values=>values.sort(([a],[b])=>a.localeCompare(b));
const failedBody=kind=>kind.startsWith('body-truncated')||kind.startsWith('body-invalid-chunk');
const delayed=kind=>['body-truncated','body-invalid-chunk'].includes(kind);
const badTLS=kind=>['tls-wrong-host','tls-untrusted'].includes(kind);
const upgrade=kind=>['tls-upgrade','tls-proxy','tls-ip-upgrade','ipv6-upgrade','redirect-cross','redirect-held-body'].includes(kind);
function fields(value,keys,label){assert.ok(value&&typeof value==='object'&&!Array.isArray(value),label);assert.deepEqual(Object.keys(value).sort(),[...keys].sort(),label);}
function bytes(value){assert.equal(typeof value,'string');const result=Buffer.from(value,'base64');assert.ok(result.toString('base64')===value,'canonical base64');return result;}
const chunks=values=>{assert.ok(Array.isArray(values));return Buffer.concat(values.map(bytes));};
function port(value){assert.ok(Number.isInteger(value)&&value>0&&value<=65535,'actual bounded port');}
function headers(raw){assert.equal(typeof raw,'string');return sorted(raw.split('\r\n').slice(1).map(line=>{const at=line.indexOf(':');assert.ok(at>0);return [line.slice(0,at).toLowerCase(),line.slice(at+1).trim()];}));}
function nonce(value,seen){assert.match(value,/^[A-Za-z0-9+/]{22}==$/);assert.equal(bytes(value).length,16);assert.ok(!seen.has(value),'fresh Upgrade nonce');seen.add(value);}
function encodedFrame(opcode,payload){
    const data=Buffer.from(payload);let head;
    if(data.length<126)head=Buffer.from([128|opcode,data.length]);
    else if(data.length<=65535){head=Buffer.alloc(4);head[0]=128|opcode;head[1]=126;head.writeUInt16BE(data.length,2);}
    else{head=Buffer.alloc(10);head[0]=128|opcode;head[1]=127;head.writeBigUInt64BE(BigInt(data.length),2);}
    return Buffer.concat([head,data]);
}
const message=(opcode,payload)=>({opcode,payload:Buffer.from(payload).toString('base64')});
const close=reason=>message(8,Buffer.concat([Buffer.from([3,232]),Buffer.from(reason)]));
function payload(challenge){const result=Buffer.alloc(180000);for(let i=0;i<result.length;i++)result[i]=(i*13+challenge.charCodeAt(i%challenge.length))%251;return result;}
function exactMessages(actual,expected,label){
    assert.ok(Array.isArray(actual));assert.equal(actual.length,expected.length,label+' count');
    actual.forEach((item,i)=>{fields(item,['opcode','payload'],label+' fields');assert.equal(item.opcode,expected[i].opcode,label+' opcode');assert.ok(item.payload===expected[i].payload,label+' complete bytes');});
}
// Parse the captured input independently of the fixture's frame observations.
// Fragmentation is allowed, but every byte, mask and terminal Close is checked.
function sourceFrames(raw,observed){
    const frames=[],messages=[];let at=0,pending=null,closed=false;
    while(at<raw.length){
        assert.ok(!closed&&raw.length-at>=2,'complete source frame after no Close');
        const first=raw[at++],second=raw[at++],opcode=first&15,fin=!!(first&128);
        assert.equal(first&0x70,0);assert.equal(second&128,128);let length=second&127;
        if(length===126){assert.ok(raw.length-at>=2);length=raw.readUInt16BE(at);at+=2;assert.ok(length>=126);}
        else if(length===127){assert.ok(raw.length-at>=8);const wide=raw.readBigUInt64BE(at);at+=8;assert.ok(wide>65535n&&wide<=1048576n);length=Number(wide);}
        assert.ok(raw.length-at>=4+length);const mask=raw.subarray(at,at+4);at+=4;
        const data=Buffer.from(raw.subarray(at,at+length));at+=length;for(let i=0;i<data.length;i++)data[i]^=mask[i%4];
        frames.push({opcode,fin,masked:true,mask:mask.toString('hex'),payload:data.toString('base64')});
        assert.ok([0,1,2,8].includes(opcode),'only expected source data and Close');
        if(opcode===8){assert.ok(fin&&length<=125&&pending===null);messages.push(message(opcode,data));closed=true;}
        else{
            if(opcode!==0){assert.equal(pending,null);pending={opcode,parts:[]};}
            assert.ok(pending);pending.parts.push(data);if(fin){messages.push(message(pending.opcode,Buffer.concat(pending.parts)));pending=null;}
        }
    }
    assert.equal(pending,null);assert.ok(JSON.stringify(frames)===JSON.stringify(observed),'raw source frames agree with independent capture');
    return messages;
}
function status(kind){return upgrade(kind)?101:kind==='body-empty'?200:kind.startsWith('body-2')||kind==='body-304'?Number(kind.slice(5)):409;}
const reasons={101:'Switching Protocols',200:'OK',204:'No Content',205:'Reset Content',302:'Found',304:'Not Modified',409:'Conflict'};
function response(value,index,key){
    const {kind,challenge}=value,redirect=kind.startsWith('redirect-')&&(kind==='redirect-loop'||index===0);
    const code=redirect?302:status(kind),head=[],body=[];
    if(code===101)head.push(['Connection','Upgrade'],['Upgrade','websocket'],['Sec-WebSocket-Accept',accept(key)]);
    else if(redirect){
        const location=kind==='redirect-loop'?`/loop?challenge=${challenge}&hop=${index+1}`:`http://${['redirect-cross','redirect-denied'].includes(kind)?'127.0.0.2':'127.0.0.1'}:${value.targetPort}/land?challenge=${challenge}`;
        head.push(['Location',location],['Content-Length',kind==='redirect-held-body'?'100':'0']);
        if(kind==='redirect-held-body')body.push(Buffer.from('first'),Buffer.alloc(95,46));
    }else if(kind==='tls-http'){head.push(['Content-Length','41']);body.push(Buffer.from('body/'+challenge));}
    else if(kind==='body-eof')body.push(Buffer.from('body/'+challenge));
    else if(failedBody(kind)){
        const chunked=kind.startsWith('body-invalid-chunk');head.push(chunked?['Transfer-Encoding','chunked']:['Content-Length','20']);
        body.push(Buffer.from(chunked?'5\r\nfirst\r\nZ\r\nlate\r\n':'first'));
    }else if(code!==204)head.push(['Content-Length','0']);
    head.push(['X-Probe',challenge]);
    if(code===101)head.push(['Set-Cookie','first=1'],['Set-Cookie','second=2']);else head.push(['Connection','close']);
    return {code,raw:`HTTP/1.1 ${code} ${reasons[code]}\r\n`+head.map(([name,value])=>name+': '+value).join('\r\n'),headers:sorted(head.map(([name,value])=>[name.toLowerCase(),value])),body:Buffer.concat(body)};
}
function assertState(observation,realm,state,label){fields(observation,['status','text'],label);assert.equal(observation.status,200);assert.deepEqual(JSON.parse(observation.text),{realm,state},label);}
function internalFailure(failure,stderr){fields(failure,['name','message'],'redacted error');assert.equal(failure.name,'Error');assert.match(failure.message,/^internal error; reference = [a-v0-9]{24}$/);assert.ok(stderr.includes(failure.message.slice(-24)),'actual internal error is correlated with stderr');}
function parameterState(value){
    const constructors=['follow','manual','undefined','null','empty','error','bogus','zero','symbol','object-error','object-manual','throwing-object','throwing-getter'].map(label=>{
        const conversion=['object-error','object-manual','throwing-object'].includes(label),throws=['throwing-object','throwing-getter'].includes(label);
        const valid=['follow','manual','undefined','object-manual'].includes(label);
        return {label,conversions:conversion?1:0,hints:conversion?['string']:[],getterCalls:label==='throwing-getter'?1:0,
            redirect:valid?label==='undefined'?'follow':label==='object-manual'?'manual':label:null,
            error:valid?null:throws?{name:null,message:null}:{name:'TypeError',message:label==='symbol'?'Cannot convert a Symbol value to a string':redirectError},sameThrown:throws};
    });
    const fetches=['error','bogus','throwing-getter'].map(label=>({label,synchronous:false,returnedPromise:true,resolved:false,
        error:label==='throwing-getter'?{name:null,message:null}:{name:'TypeError',message:redirectError},sameThrown:label==='throwing-getter',getterCalls:label==='throwing-getter'?1:0}));
    const responses=['constructor','json'].flatMap(method=>['error','bogus','symbol','object','getter'].map(label=>({method,label,status:200,conversions:0,getterCalls:0,error:null})));
    return {kind:value.kind,challenge:value.challenge,constructors,fetches,responses};
}
function wire(value,seen){
    const {headers:initial,wire:final,kind,challenge}=value;
    const keys=['status','rawHeader','requestKey','rawTail','frames','peerTerminated','transportError','completion','closeReplySent'];
    fields(initial,keys,'initial raw client');fields(final,keys,'terminal raw client');nonce(initial.requestKey,seen);
    assert.equal(initial.status,101);assert.equal(final.status,101);assert.equal(final.requestKey,initial.requestKey);assert.equal(final.rawHeader,initial.rawHeader);
    const expected=[['connection','Upgrade'],['sec-websocket-accept',accept(initial.requestKey)],['upgrade','websocket']];
    if(kind==='tls-proxy')expected.push(['set-cookie','first=1'],['set-cookie','second=2'],['x-probe',challenge]);
    assert.equal(initial.rawHeader.split('\r\n')[0],'HTTP/1.1 101 Switching Protocols');assert.deepEqual(headers(initial.rawHeader),sorted(expected));
    assert.deepEqual(initial.frames,[]);assert.equal(initial.rawTail,'');assert.equal(initial.peerTerminated,false);assert.equal(initial.transportError,null);assert.equal(initial.completion,null);assert.equal(initial.closeReplySent,false);
    const frames=kind==='tls-proxy'?[message(2,payload(challenge)),close('done')]:[
        message(1,'started'),...(failedBody(kind)?[message(1,'body-first')]:[]),message(1,'done'),close('done')];
    exactMessages(final.frames,frames,'downstream');
    assert.ok(bytes(final.rawTail).equals(Buffer.concat(frames.map(item=>encodedFrame(item.opcode,bytes(item.payload))))),'complete actual downstream wire');
    assert.equal(final.peerTerminated,true);assert.ok(['peer-ended','peer-reset'].includes(final.completion));assert.equal(final.transportError,final.completion==='peer-reset'?'ECONNRESET':null);
    assert.equal(final.closeReplySent,kind!=='tls-proxy');
    if(kind==='tls-proxy')assert.deepEqual(value.binary,{bytes:180000,sha256:digest(payload(challenge))});else assert.equal(value.binary,null);
}
function upstream(value,seen){
    const {kind,challenge}=value,snapshot=value.upstreamBeforeStop;
    fields(snapshot,['records','tcpRecords','sni','tlsErrors'],'complete upstream snapshot');
    assert.deepEqual(value.upstreamAfterStop,snapshot,'all source observations terminated before service shutdown');assert.equal(value.allSourcesEndedBeforeStop,true);
    const count=['redirect-error','request-redirect-values'].includes(kind)?0:kind==='redirect-loop'?21:['redirect-cross','redirect-held-body'].includes(kind)?2:1;
    assert.equal(snapshot.tcpRecords.length,count,'exact actual TCP inventory');
    const requests=snapshot.records.filter(item=>item.request!==null);assert.equal(requests.length,badTLS(kind)?0:count,'exact HTTP inventory (secure callbacks are not requests)');
    assert.ok(snapshot.records.length===requests.length||badTLS(kind)&&snapshot.records.length<=1,'no extra secure session');
    const tcpIds=new Set();
    snapshot.tcpRecords.forEach((item,index)=>{
        fields(item,['role','localAddress','localPort','remoteAddress','remotePort','ended','closed','error',...(Object.hasOwn(item,'tlsError')?['tlsError']:[])],'TCP fields');
        assert.equal(item.role,index===1&&kind!=='redirect-loop'?'target':'source');port(item.localPort);port(item.remotePort);
        assert.equal(item.localPort,item.role==='source'?value.sourcePort:value.targetPort);
        const address=kind==='ipv6-upgrade'?'::1':item.role==='target'&&kind==='redirect-cross'?'127.0.0.2':item.localAddress==='::1'&&kind.startsWith('tls-')?'::1':'127.0.0.1';
        assert.equal(item.localAddress,address);assert.ok(['127.0.0.1','::1'].includes(item.remoteAddress));assert.equal(item.closed,true);assert.equal(typeof item.ended,'boolean');assert.equal(item.error,null);
        const identity=[item.role,item.localAddress,item.localPort,item.remotePort].join('/');assert.ok(!tcpIds.has(identity));tcpIds.add(identity);
        if(item.tlsError)assert.ok(snapshot.tlsErrors.some(error=>JSON.stringify(error)===JSON.stringify(item.tlsError)));
    });
    assert.deepEqual(snapshot.sni,kind.startsWith('tls-')?[{role:'source',servername:['tls-ip-upgrade','tls-wrong-host'].includes(kind)?'127.0.0.1':'localhost'}]:[],'actual TLS SNI');
    if(!badTLS(kind))assert.deepEqual(snapshot.tlsErrors,[]);
    else for(const error of snapshot.tlsErrors){fields(error,['code','message','remotePort'],'TLS alert');assert.equal(error.code,kind==='tls-untrusted'?'ERR_SSL_TLSV1_ALERT_UNKNOWN_CA':'ERR_SSL_TLSV1_ALERT_INTERNAL_ERROR');assert.equal(typeof error.message,'string');assert.ok(error.remotePort===null||snapshot.tcpRecords.some(item=>item.remotePort===error.remotePort));}
    const results=[];
    snapshot.records.forEach(item=>{
        fields(item,['role','localAddress','localPort','remoteAddress','remotePort','servername','request','rawRequest','responseHeader','responseStatus','frames','sentFrames','receivedChunks','sentChunks','events','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','error'],'upstream connection fields');
        assert.ok(snapshot.tcpRecords.some(tcp=>tcp.role===item.role&&tcp.localAddress===item.localAddress&&tcp.localPort===item.localPort&&tcp.remoteAddress===item.remoteAddress&&tcp.remotePort===item.remotePort),'secure/request session belongs to observed TCP');
        assert.equal(item.servername,kind.startsWith('tls-')?['tls-ip-upgrade','tls-wrong-host'].includes(kind)?'127.0.0.1':'localhost':null);
        assert.equal(item.fixtureDestroyed,false);assert.equal(item.error,null);assert.ok(item.peerEnded||item.peerReset,'actual source termination');
        if(!kind.startsWith('tls-')){assert.equal(item.peerEnded,true);assert.equal(item.peerReset,false);}
        if(item.request===null){
            assert.ok(badTLS(kind));for(const key of ['rawRequest','responseHeader','responseStatus'])assert.equal(item[key],null);
            for(const key of ['frames','sentFrames','receivedChunks','sentChunks'])assert.deepEqual(item[key],[]);assert.equal(item.fixtureEnded,false);
            assert.deepEqual(item.events,item.peerEnded?['peer-ended']:[]);return;
        }
        const index=results.length;const target=index>0&&kind!=='redirect-loop';assert.equal(item.role,target?'target':'source');
        const requestTarget=target?`/land?challenge=${challenge}`:kind==='redirect-loop'?`/loop?challenge=${challenge}${index?'&hop='+index:''}`:`/probe?challenge=${challenge}`;
        assert.equal(item.request.split('\r\n')[0],`GET ${requestTarget} HTTP/1.1`);
        const actualHeaders=headers(item.request),keys=actualHeaders.filter(([name])=>name==='sec-websocket-key');assert.equal(keys.length,1);const key=keys[0][1];nonce(key,seen);
        const authority=target?`${kind==='redirect-cross'?'127.0.0.2':'127.0.0.1'}:${value.targetPort}`:new URL(value.target).host;
        const expectedHeaders=[['connection','Upgrade'],['host',authority],['sec-websocket-key',key],['sec-websocket-version','13'],['upgrade','websocket'],['x-probe',challenge]];
        if(kind.startsWith('redirect-')){expectedHeaders.push(['proxy-authorization','proxy/'+challenge],['cookie','cookie='+challenge]);if(!target)expectedHeaders.push(['authorization','auth/'+challenge]);}
        assert.deepEqual(actualHeaders,sorted(expectedHeaders),'literal outgoing headers, including redirect credential handling');
        const request=Buffer.from(item.request+'\r\n\r\n','latin1'),received=chunks(item.receivedChunks);
        assert.ok(bytes(item.rawRequest).equals(request));assert.ok(received.subarray(0,request.length).equals(request));
        const expected=response(value,index,key);assert.equal(item.responseHeader,expected.raw);assert.equal(item.responseStatus,expected.code);assert.equal(item.fixtureEnded,true);
        const sent=chunks(item.sentChunks),prefix=Buffer.from(expected.raw+'\r\n\r\n','latin1');assert.ok(sent.subarray(0,prefix.length).equals(prefix),'complete source response header');
        const sourceMessages=sourceFrames(received.subarray(request.length),item.frames);
        if(expected.code===101){
            const messages=kind==='tls-proxy'?[message(2,payload(challenge)),close('done')]:[message(1,'echo/'+challenge),message(2,Buffer.from([1,2,3,4,5])),close('child')];
            exactMessages(sourceMessages,messages,'source received');exactMessages(item.sentFrames,messages,'source sent');
            assert.ok(sent.subarray(prefix.length).equals(Buffer.concat(messages.map(item=>encodedFrame(item.opcode,bytes(item.payload))))),'complete source response data');
        }else{
            assert.deepEqual(sourceMessages,[]);assert.deepEqual(item.sentFrames,[]);assert.ok(sent.subarray(prefix.length).equals(expected.body),'exact HTTP response body, including partial/error input');
        }
        const events=['request',expected.code===101?'upgrade':'response'];
        if(expected.code===101)events.push('close-reply');
        if(kind==='redirect-held-body'&&!target||delayed(kind))events.push('release-body');
        if(kind.endsWith('-coalesced'))events.push('coalesced-finish');
        if(item.peerEnded)events.push('peer-ended');assert.deepEqual(item.events,events,'complete source lifecycle');
        results.push(expected);
    });
    return results;
}
export function assertFetchBoundaryCase(value,mode){
    assert.ok(['reference','single-worker','static-pool','managed'].includes(mode));
    fields(value,['kind','challenge','target','sourcePort','targetPort','allowedAuthorities','initial','headers','wire','observed','allSourcesEndedBeforeStop','upstreamBeforeStop','shutdown','stderr','upstreamAfterStop','checkpoint','checkpointUpstream','followedBeforeRelease','binary','failure','cleanupFailure'],'complete boundary case');
    assert.ok(fetchBoundaryCases.includes(value.kind));assert.match(value.challenge,uuid);port(value.sourcePort);if(value.targetPort!==null)port(value.targetPort);
    assert.equal(value.failure,null);assert.equal(value.cleanupFailure,null);assert.equal(typeof value.stderr,'string');assert.doesNotMatch(value.stderr,/AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON/);
    const {kind,challenge}=value,tls=kind.startsWith('tls-'),redirect=kind.startsWith('redirect-');
    const host=['tls-ip-upgrade','tls-wrong-host'].includes(kind)?'127.0.0.1':tls?'localhost':kind==='ipv6-upgrade'?'[::1]':'127.0.0.1';
    assert.equal(value.target,`${tls?'https':'http'}://${host}:${value.sourcePort}/${kind==='redirect-loop'?'loop':'probe'}?challenge=${challenge}`);
    assert.equal(value.targetPort!==null,redirect);const allowed=[host+':'+value.sourcePort];
    if(redirect&&kind!=='redirect-denied')allowed.push(`${kind==='redirect-cross'?'127.0.0.2':'127.0.0.1'}:${value.targetPort}`);
    assert.deepEqual(value.allowedAuthorities,allowed,'actual Host and App permission intersection');
    fields(value.initial,['status','text'],'initial state');assert.equal(value.initial.status,200);const initial=JSON.parse(value.initial.text);fields(initial,['realm','state'],'initial document');assert.match(initial.realm,uuid);assert.equal(initial.state,null);
    const state=JSON.parse(value.observed.text).state,seen=new Set(),responses=upstream(value,seen);wire(value,seen);
    if(kind==='request-redirect-values')assertState(value.observed,initial.realm,parameterState(value),'literal Request/fetch conversion, async rejection and thrown identity');
    else{
        const failed=badTLS(kind)||['redirect-error','redirect-loop','redirect-denied'].includes(kind)||failedBody(kind);
        let failure=null;
        if(badTLS(kind)||kind==='redirect-denied'||kind.startsWith('body-invalid-chunk')){internalFailure(state.failure,value.stderr);failure=state.failure;}
        else if(kind==='redirect-error')failure={name:'TypeError',message:redirectError};
        else if(kind==='redirect-loop')failure={name:'TypeError',message:'Too many redirects.'+Array.from({length:21},(_,i)=>value.target+(i?'&hop='+i:'')).join(', ')};
        else if(kind.startsWith('body-truncated'))failure={name:'Error',message:'Network connection lost.'};
        const fetched=responses.at(-1),successfulHeaders=!badTLS(kind)&&!['redirect-error','redirect-loop','redirect-denied'].includes(kind);
        const nullBody=upgrade(kind)||['body-204','body-205','body-304'].includes(kind);
        const result=successfulHeaders?{status:fetched.code,statusText:reasons[fetched.code],
            url:['redirect-cross','redirect-held-body'].includes(kind)?`http://${kind==='redirect-cross'?'127.0.0.2':'127.0.0.1'}:${value.targetPort}/land?challenge=${challenge}`:value.target,
            redirected:['redirect-cross','redirect-held-body'].includes(kind),type:'default',bodyNull:nullBody,bodyUsed:!nullBody&&!failedBody(kind),
            headers:fetched.headers,cookies:upgrade(kind)?['first=1','second=2']:[],hasWebSocket:upgrade(kind),
            body:upgrade(kind)||failedBody(kind)?null:{bytes:fetched.body.length,sha256:digest(fetched.body)}}:null;
        const closed={code:1000,reason:'child',wasClean:true},events=[{type:'close',...closed}];
        // Preserve the already characterized pinned-reference close race only
        // when its complete matching Close/data exchange is present.
        if(mode==='reference'&&state.socket?.events?.length===2)events.unshift({type:'error',message:'Uncaught Error: Network connection lost.',error:'Error: Network connection lost.'});
        const socket=upgrade(kind)&&kind!=='tls-proxy'?{before:1,messages:[{type:'text',value:'echo/'+challenge},{type:'binary',bytes:5,sha256:digest(Buffer.from([1,2,3,4,5]))}],events,closed}:null;
        const expected={kind,challenge,trace:kind==='tls-proxy'?['returned']:['fetch',...(successfulHeaders?['headers']:[]),...(failedBody(kind)?['body-first']:[]),failed?'failure':'complete','parent-close'],
            result,firstBody:failedBody(kind)?{done:false,bytes:5,sha256:digest(Buffer.from('first'))}:null,failure,socket};
        if(kind==='redirect-error')expected.requestProbe={error:{name:'TypeError',message:redirectError}};
        assertState(value.observed,initial.realm,expected,'complete same-realm fetch result');
    }
    if(delayed(kind)||kind==='redirect-held-body'){
        const checkpoint=JSON.parse(value.checkpoint.text);assert.equal(value.checkpoint.status,200);assert.equal(checkpoint.realm,initial.realm);
        const expected=JSON.parse(value.observed.text).state;expected.trace=kind==='redirect-held-body'?['fetch']:['fetch','headers','body-first'];expected.failure=null;expected.socket=null;if(kind==='redirect-held-body')expected.result=null;
        assert.deepEqual(checkpoint.state,expected,'actual intermediate state before release');
        fields(value.checkpointUpstream,['records','tcpRecords','sni','tlsErrors'],'checkpoint upstream');
        assert.equal(value.checkpointUpstream.records.length,1);assert.equal(value.checkpointUpstream.tcpRecords.length,1);
        const held=value.checkpointUpstream.records[0],final=value.upstreamBeforeStop.records[0];assert.equal(held.request,final.request);assert.equal(held.rawRequest,final.rawRequest);
        for(const key of ['role','localAddress','localPort','remoteAddress','remotePort','servername','responseHeader','responseStatus'])assert.equal(held[key],final[key],'same pending source '+key);
        assert.equal(held.fixtureEnded,false);assert.equal(held.fixtureDestroyed,false);assert.equal(held.peerEnded,false);assert.equal(held.peerReset,false);assert.equal(held.error,null);
        assert.deepEqual(held.events,['request','response']);assert.deepEqual(held.frames,[]);assert.deepEqual(held.sentFrames,[]);
        assert.ok(chunks(held.receivedChunks).equals(bytes(held.rawRequest)),'complete pending request bytes');
        const pendingBody=kind==='body-invalid-chunk'?'5\r\nfirst\r\n':'first';
        assert.ok(chunks(held.sentChunks).equals(Buffer.from(held.responseHeader+'\r\n\r\n'+pendingBody,'latin1')),'complete pending response and valid prefix');
        const tcp=value.checkpointUpstream.tcpRecords[0],finalTCP=value.upstreamBeforeStop.tcpRecords[0];
        for(const key of ['role','localAddress','localPort','remoteAddress','remotePort'])assert.equal(tcp[key],finalTCP[key]);
        assert.equal(tcp.closed,false);assert.equal(tcp.ended,false);assert.equal(tcp.error,null);
        assert.deepEqual(value.checkpointUpstream.sni,[]);assert.deepEqual(value.checkpointUpstream.tlsErrors,[]);
        if(kind==='redirect-held-body'){assert.equal(value.followedBeforeRelease,false);assert.deepEqual(value.checkpointUpstream.tcpRecords.map(row=>row.role),['source'],'no target TCP before redirect body completes');}
        else assert.equal(value.followedBeforeRelease,null);
    }else{assert.equal(value.checkpoint,null);assert.equal(value.checkpointUpstream,null);assert.equal(value.followedBeforeRelease,null);}
    fields(value.shutdown,['code','signal','forced'],'normal service shutdown');assert.equal(value.shutdown.forced,false);
    if(mode==='reference')assert.ok(value.shutdown.code===0&&value.shutdown.signal===null||value.shutdown.code===null&&value.shutdown.signal==='SIGTERM');else assert.deepEqual(value.shutdown,{code:0,signal:null,forced:false});
    return {kind,status:state.result?.status??null,failed:state.failure!==null&&kind!=='request-redirect-values'};
}
function certificates(value){
    fields(value,['opensslVersion','opensslSHA256','caSHA256','ipServerSHA256','serverSHA256','untrustedSHA256','serverSAN','ipServerSAN','untrustedSAN','caPEM','serverPEM','ipServerPEM','untrustedPEM'],'certificate evidence');
    assert.match(value.opensslVersion,/^OpenSSL \S+/);assert.match(value.opensslSHA256,/^[a-f0-9]{64}$/);
    const certs={};for(const [name,hashName] of [['ca','ca'],['server','server'],['ipServer','ipServer'],['untrusted','untrusted']]){
        const pem=value[name+'PEM'];assert.equal(digest(pem),value[hashName+'SHA256']);certs[name]=new X509Certificate(pem);
    }
    assert.equal(certs.ca.ca,true);assert.equal(certs.server.verify(certs.ca.publicKey),true);assert.equal(certs.ipServer.verify(certs.ca.publicKey),true);assert.equal(certs.untrusted.verify(certs.ca.publicKey),false);
    for(const [name,san] of [['server','DNS:localhost'],['ipServer','IP Address:127.0.0.1'],['untrusted','DNS:localhost']]){assert.equal(certs[name].subjectAltName,san);assert.equal(value[name+'SAN'],san);}
    assert.equal(certs.server.checkIP('127.0.0.1'),undefined);assert.equal(certs.ipServer.checkIP('127.0.0.1'),'127.0.0.1');assert.equal(certs.server.checkHost('localhost'),'localhost');
}
export function validateFetchBoundaryReport(report,expected){
    fields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','certificateInputs','cases'],'boundary report');
    assert.equal(report.schema,'capsid-fetch-boundary-v1');assert.equal(report.complete,true);assert.equal(report.runId,expected.runId);assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));assert.deepEqual(report.referenceLock,expected.lock);
    assert.deepEqual(report.inputs,expected.inputs);assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(fetchBoundaryInputPaths).sort());for(const hash of Object.values(report.inputs))assert.match(hash,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,worker:report.mode==='reference'?null:expected.worker});
    certificates(report.certificateInputs);assert.deepEqual(report.cases.map(value=>value.kind),fetchBoundaryCases);
    assert.equal(new Set(report.cases.map(value=>value.challenge)).size,fetchBoundaryCases.length);assert.equal(new Set(report.cases.map(value=>JSON.parse(value.initial.text).realm)).size,fetchBoundaryCases.length);
    return report.cases.map(value=>assertFetchBoundaryCase(value,report.mode));
}
export function compareFetchBoundaryReports(reference,candidates,expected){
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(value=>value.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateFetchBoundaryReport(reference,expected);for(const candidate of candidates)assert.deepEqual(validateFetchBoundaryReport(candidate,expected),baseline);
    return {complete:true,casesPerBackend:fetchBoundaryCases.length,backends:candidates.length};
}
