import assert from 'node:assert/strict';
import {createHash,X509Certificate} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const fetchTransitionCases=Object.freeze(["tls-ipv6-upgrade", "tls-ipv6-proxy", "tls-ipv6-wrong-host", "redirect-http-to-https", "redirect-https-to-http", "redirect-https-to-https", "redirect-https-denied", "redirect-http-to-https-wrong-host", "tls-body-reader-cancel", "tls-body-abort", "tls-body-abort-before-read", "tls-upgrade-abort-pending", "tls-handshake-abort", "redirect-held-abort", "tls-body-context-end", "http-created-promise", "event-created-promise", "http-created-timer", "http-created-promise-after-close", "http-created-timer-after-close", "tls-body-reader-probe-object", "tls-body-reader-probe-default", "tls-body-reader-probe-string", "tls-body-reader-probe-error", "tls-body-reader-probe-null", "tls-body-reader-probe-no-pending", "tls-body-reader-probe-before-first", "tls-body-reader-probe-eof", "standard-reader-probe", "tls-body-reader-probe-pending-two",
    "tls-body-reader-probe-direct-object", "tls-body-reader-probe-direct-before-first", "tls-body-reader-probe-direct-pending-two", "standard-reader-probe-direct-pending-two",
    "tls-body-reader-probe-prototype-object", "tls-body-reader-probe-prototype-before-first", "tls-body-reader-probe-prototype-pending-two", "standard-reader-probe-prototype-pending-two",
    "tls-body-reader-probe-subclass-object", "tls-body-reader-probe-subclass-before-first", "tls-body-reader-probe-subclass-pending-two", "standard-reader-probe-subclass-pending-two",
    "tls-body-reader-probe-reflect-object", "tls-body-reader-probe-reflect-before-first", "tls-body-reader-probe-reflect-pending-two", "standard-reader-probe-reflect-pending-two"]);
export const fetchTransitionInputPaths=Object.freeze({runner:'tests/test_websocket_fetch_transitions.mjs',fixture:'tests/fixtures/workerd-fetch-transitions.mjs',upstream:'tests/helpers/ws_fetch_transition_peer.mjs',controlPeer:'tests/helpers/ws_response_client.mjs',proxyPeer:'tests/helpers/ws_fetch_proxy_client.mjs',hostBackend:'tests/helpers/ws_host_backend.mjs',contract:'tools/workerd-fetch-transition-contract.mjs'});
const uuid=/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
const accept=key=>createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
const sorted=values=>values.sort(([a],[b])=>a.localeCompare(b));
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
function profile(kind){
    const ipv6=kind.startsWith('tls-ipv6-'),redirect=kind.startsWith('redirect-');
    const scope=['http-created-promise','event-created-promise','http-created-timer','http-created-promise-after-close','http-created-timer-after-close'].includes(kind);
    const reader=kind.startsWith('tls-body-reader-probe-')||kind.startsWith('standard-reader-probe');
    const sourceTLS=kind.startsWith('tls-')||['redirect-https-to-http','redirect-https-to-https','redirect-https-denied','redirect-held-abort'].includes(kind);
    const targetTLS=['redirect-http-to-https','redirect-https-to-https','redirect-http-to-https-wrong-host'].includes(kind);
    const sourceHost=ipv6?'[::1]':sourceTLS?'localhost':'127.0.0.1';
    const targetHost=kind==='redirect-https-denied'?'127.0.0.2':kind==='redirect-http-to-https-wrong-host'?'127.0.0.1':targetTLS?'localhost':'127.0.0.1';
    const badTLS=['tls-ipv6-wrong-host','redirect-http-to-https-wrong-host'].includes(kind);
    const goodUpgrade=['tls-ipv6-upgrade','tls-ipv6-proxy','redirect-http-to-https','redirect-https-to-http','redirect-https-to-https'].includes(kind);
    const contextEnd=kind==='tls-body-context-end',scopeEnd=scope&&kind.endsWith('-after-close');
    const bodyActor=['tls-body-reader-cancel','tls-body-abort','tls-body-abort-before-read', 'tls-body-context-end'].includes(kind);
    const pendingActor=['tls-upgrade-abort-pending','tls-handshake-abort','redirect-held-abort'].includes(kind);
    return {ipv6,redirect,scope,reader,sourceTLS,targetTLS,sourceHost,targetHost,badTLS,goodUpgrade,contextEnd,scopeEnd,bodyActor,pendingActor};
}
const emptySnapshot=()=>({records:[],tcpRecords:[],sni:[],tlsErrors:[]});
const part=()=>({done:false,bytes:5,sha256:digest(Buffer.from('first'))});
function assertState(observation,realm,state,label){fields(observation,['status','text'],label);assert.equal(observation.status,200);assert.deepEqual(JSON.parse(observation.text),{realm,state},label);}
function failure(value,stderr){
    fields(value,['type','name','message','primitive'],'serialized internal error');assert.equal(value.type,'object');assert.equal(value.name,'Error');assert.equal(value.primitive,null);
    assert.match(value.message,/^internal error; reference = [a-v0-9]{24}$/);assert.ok(stderr.includes(value.message.slice(-24)),'actual diagnostic identifier correlated with stderr');return value;
}
function response(value,role,key){
    const p=profile(value.kind),{kind,challenge}=value,redirect=p.redirect&&role==='source';
    if(kind==='tls-upgrade-abort-pending'||kind==='tls-handshake-abort')return null;
    const code=redirect?302:p.goodUpgrade?101:kind==='tls-body-reader-probe-eof'?200:409;
    const head=[],body=[];
    if(code===101)head.push(['Connection','Upgrade'],['Upgrade','websocket'],['Sec-WebSocket-Accept',accept(key)]);
    else if(redirect){head.push(['Location',`${p.targetTLS?'https':'http'}://${p.targetHost}:${value.targetPort}/land?challenge=${challenge}`],['Content-Length',kind==='redirect-held-abort'?'100':'0']);if(kind==='redirect-held-abort')body.push(Buffer.from('first'));}
    else{head.push(['Content-Length',code===200?'5':'100']);body.push(Buffer.from('first'));}
    head.push(['X-Probe',challenge]);if(code===101)head.push(['Set-Cookie','first=1'],['Set-Cookie','second=2']);else head.push(['Connection','close']);
    const reason={101:'Switching Protocols',200:'OK',302:'Found',409:'Conflict'}[code];
    return {code,reason,raw:`HTTP/1.1 ${code} ${reason}\r\n`+head.map(([n,v])=>n+': '+v).join('\r\n'),headers:sorted(head.map(([n,v])=>[n.toLowerCase(),v])),body:Buffer.concat(body)};
}
function wire(value,seen){
    const {headers:initial,wire:final,kind,challenge}=value,p=profile(kind);
    const keys=['status','rawHeader','requestKey','rawTail','frames','peerTerminated','transportError','completion','closeReplySent'];
    fields(initial,keys,'initial raw client');fields(final,keys,'terminal raw client');nonce(initial.requestKey,seen);
    assert.equal(initial.status,101);assert.equal(final.status,101);assert.equal(final.requestKey,initial.requestKey);assert.equal(final.rawHeader,initial.rawHeader);
    const expected=[['connection','Upgrade'],['sec-websocket-accept',accept(initial.requestKey)],['upgrade','websocket']];
    if(kind==='tls-ipv6-proxy')expected.push(['set-cookie','first=1'],['set-cookie','second=2'],['x-probe',challenge]);
    assert.equal(initial.rawHeader.split('\r\n')[0],'HTTP/1.1 101 Switching Protocols');assert.deepEqual(headers(initial.rawHeader),sorted(expected));
    assert.deepEqual(initial.frames,[]);assert.equal(initial.rawTail,'');assert.equal(initial.peerTerminated,false);assert.equal(initial.transportError,null);assert.equal(initial.completion,null);assert.equal(initial.closeReplySent,false);
    const done=!p.contextEnd&&!p.scopeEnd;
    const frames=kind==='tls-ipv6-proxy'?[message(2,payload(challenge)),close('done')]:[message(1,'started'),
        ...(p.bodyActor?[message(1,kind==='tls-body-abort-before-read'?'headers-ready':'body-first')]:[]),...(done?[message(1,'done')]:[]),close('done')];
    exactMessages(final.frames,frames,'downstream');assert.ok(bytes(final.rawTail).equals(Buffer.concat(frames.map(item=>encodedFrame(item.opcode,bytes(item.payload))))),'complete actual downstream wire');
    assert.equal(final.peerTerminated,true);assert.ok(['peer-ended','peer-reset'].includes(final.completion));assert.equal(final.transportError,final.completion==='peer-reset'?'ECONNRESET':null);
    assert.equal(final.closeReplySent,done&&kind!=='tls-ipv6-proxy');
    assert.deepEqual(value.binary,kind==='tls-ipv6-proxy'?{bytes:180000,sha256:digest(payload(challenge))}:null);
}
// The handshake-abort peer is raw TCP. Parse the actual ClientHello rather
// than treating a socket count or the fixture's SNI callback as handshake proof.
function clientHello(raw){
    const pieces=[];let at=0;
    while(at<raw.length){assert.ok(raw.length-at>=5);assert.equal(raw[at],22,'only ClientHello records before cancellation');assert.equal(raw[at+1],3);const size=raw.readUInt16BE(at+3);at+=5;assert.ok(size>0&&at+size<=raw.length);pieces.push(raw.subarray(at,at+size));at+=size;}
    const hello=Buffer.concat(pieces);assert.ok(hello.length>=38);assert.equal(hello[0],1);assert.equal(hello.readUIntBE(1,3),hello.length-4);
    at=4;assert.equal(hello[at++],3);assert.ok([1,2,3].includes(hello[at++]));at+=32;
    const vector=(width)=>{assert.ok(at+width<=hello.length);const size=hello.readUIntBE(at,width);at+=width;assert.ok(at+size<=hello.length);const result=hello.subarray(at,at+size);at+=size;return result;};
    vector(1);const ciphers=vector(2);assert.ok(ciphers.length>0&&ciphers.length%2===0);assert.ok(vector(1).includes(0));const extensions=vector(2);assert.equal(at,hello.length);
    const names=[],types=new Set();at=0;
    while(at<extensions.length){assert.ok(at+4<=extensions.length);const type=extensions.readUInt16BE(at),size=extensions.readUInt16BE(at+2);at+=4;assert.ok(!types.has(type));types.add(type);assert.ok(at+size<=extensions.length);
        if(type===0){const data=extensions.subarray(at,at+size);assert.ok(data.length>=5);assert.equal(data.readUInt16BE(0),data.length-2);let n=2;while(n<data.length){assert.ok(n+3<=data.length);assert.equal(data[n],0);const length=data.readUInt16BE(n+1);n+=3;assert.ok(length>0&&n+length<=data.length);names.push(data.subarray(n,n+length).toString('ascii'));n+=length;}}
        at+=size;
    }assert.deepEqual(names,['localhost'],'actual raw ClientHello SNI');
}
function upstream(value,seen,pending=false){
    const {kind,challenge}=value,p=profile(kind),snapshot=pending?value.checkpointUpstream:value.upstreamBeforeStop;
    fields(snapshot,['records','tcpRecords','sni','tlsErrors'],'complete upstream snapshot');
    if(!pending){assert.deepEqual(value.upstreamAfterStop,snapshot,'all source observations terminated before service shutdown');assert.equal(value.allSourcesEndedBeforeStop,true);}
    const quiet=p.scope||kind.startsWith('standard-reader-probe');
    const count=quiet?0:p.redirect&&!['redirect-https-denied','redirect-held-abort'].includes(kind)?2:1;
    assert.equal(snapshot.tcpRecords.length,count,'exact actual TCP inventory');
    const requestCount=quiet||kind==='tls-ipv6-wrong-host'||kind==='tls-handshake-abort'?0:kind==='redirect-http-to-https-wrong-host'?1:count;
    const requests=snapshot.records.filter(item=>item.request!==null);assert.equal(requests.length,requestCount,'exact HTTP inventory');
    const optionalSecure=p.badTLS?1:0;assert.ok(snapshot.records.length===requestCount+(kind==='tls-handshake-abort'?1:0)||optionalSecure&&snapshot.records.length===requestCount+1,'secure callbacks are distinct from HTTP requests');
    const ids=new Set();
    snapshot.tcpRecords.forEach((item,index)=>{
        fields(item,['role','localAddress','localPort','remoteAddress','remotePort','ended','closed','error',...(Object.hasOwn(item,'tlsError')?['tlsError']:[])],'TCP fields');
        const role=index===0?'source':'target',tls=role==='source'?p.sourceTLS:p.targetTLS;assert.equal(item.role,role);port(item.localPort);port(item.remotePort);
        assert.equal(item.localPort,role==='source'?value.sourcePort:value.targetPort);assert.ok((p.ipv6&&role==='source'?['::1']:tls?['127.0.0.1','::1']:['127.0.0.1']).includes(item.localAddress));assert.equal(item.remoteAddress,item.localAddress);
        assert.equal(item.closed,!pending);assert.equal(typeof item.ended,'boolean');if(pending)assert.equal(item.ended,false);assert.equal(item.error,null);
        const identity=[item.role,item.localAddress,item.localPort,item.remotePort].join('/');assert.ok(!ids.has(identity));ids.add(identity);
        if(item.tlsError)assert.ok(snapshot.tlsErrors.some(error=>JSON.stringify(error)===JSON.stringify(item.tlsError)));
    });
    const sni=[];
    if(!quiet&&p.sourceTLS&&kind!=='tls-handshake-abort')sni.push({role:'source',servername:p.ipv6?'::1':'localhost'});
    if(!quiet&&count===2&&p.targetTLS)sni.push({role:'target',servername:p.targetHost});assert.deepEqual(snapshot.sni,sni,'actual TLS SNI');
    if(!p.badTLS||pending)assert.deepEqual(snapshot.tlsErrors,[]);
    else {assert.ok(snapshot.tlsErrors.length<=1);for(const error of snapshot.tlsErrors){fields(error,['code','message','remotePort'],'TLS alert');assert.equal(error.code,'ERR_SSL_TLSV1_ALERT_INTERNAL_ERROR');assert.equal(typeof error.message,'string');assert.ok(error.remotePort===null||snapshot.tcpRecords.some(item=>item.remotePort===error.remotePort));}}
    const responses=[];
    snapshot.records.forEach(item=>{
        fields(item,['role','localAddress','localPort','remoteAddress','remotePort','servername','request','rawRequest','responseHeader','responseStatus','frames','sentFrames','receivedChunks','sentChunks','events','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','error'],'upstream connection fields');
        assert.ok(snapshot.tcpRecords.some(tcp=>tcp.role===item.role&&tcp.localAddress===item.localAddress&&tcp.localPort===item.localPort&&tcp.remoteAddress===item.remoteAddress&&tcp.remotePort===item.remotePort),'session belongs to observed TCP');
        const isSource=item.role==='source',tls=isSource?p.sourceTLS:p.targetTLS;
        assert.equal(item.servername,kind==='tls-handshake-abort'?null:tls?isSource?p.ipv6?'::1':'localhost':p.targetHost:null);
        assert.equal(item.fixtureDestroyed,false);assert.equal(item.error,null);assert.equal(typeof item.peerEnded,'boolean');assert.equal(typeof item.peerReset,'boolean');
        if(pending){assert.equal(item.peerEnded,false);assert.equal(item.peerReset,false);}else assert.ok(item.peerEnded||item.peerReset,'actual peer termination before fixture stop');
        if(item.request===null){
            assert.ok(p.badTLS||kind==='tls-handshake-abort');for(const key of ['rawRequest','responseHeader','responseStatus'])assert.equal(item[key],null);
            for(const key of ['frames','sentFrames','sentChunks'])assert.deepEqual(item[key],[]);assert.equal(item.fixtureEnded,false);
            if(kind==='tls-handshake-abort')clientHello(chunks(item.receivedChunks));else assert.deepEqual(item.receivedChunks,[]);
            assert.deepEqual(item.events,item.peerEnded?['peer-ended']:[]);return;
        }
        const route=isSource?'/probe':'/land';assert.equal(item.request.split('\r\n')[0],`GET ${route}?challenge=${challenge} HTTP/1.1`);
        const actualHeaders=headers(item.request),keys=actualHeaders.filter(([name])=>name==='sec-websocket-key');assert.equal(keys.length,1);const key=keys[0][1];nonce(key,seen);
        const authority=(isSource?p.sourceHost:p.targetHost)+':'+(isSource?value.sourcePort:value.targetPort);
        const expectedHeaders=[['connection','Upgrade'],['host',authority],['sec-websocket-key',key],['sec-websocket-version','13'],['upgrade','websocket'],['x-probe',challenge]];
        if(p.redirect){expectedHeaders.push(['proxy-authorization','proxy/'+challenge],['cookie','cookie='+challenge]);if(isSource)expectedHeaders.push(['authorization','auth/'+challenge]);}
        assert.deepEqual(actualHeaders,sorted(expectedHeaders),'literal outgoing headers and redirect credential handling');
        const request=Buffer.from(item.request+'\r\n\r\n','latin1'),received=chunks(item.receivedChunks);assert.ok(bytes(item.rawRequest).equals(request));assert.ok(received.subarray(0,request.length).equals(request));
        const expected=response(value,item.role,key),sent=chunks(item.sentChunks);const sourceMessages=sourceFrames(received.subarray(request.length),item.frames);
        const events=['request'];
        if(expected===null){assert.equal(kind,'tls-upgrade-abort-pending');assert.equal(item.responseHeader,null);assert.equal(item.responseStatus,null);assert.equal(sent.length,0);assert.equal(item.fixtureEnded,false);assert.deepEqual(sourceMessages,[]);assert.deepEqual(item.sentFrames,[]);events.push('headers-held');}
        else{
            assert.equal(item.responseHeader,expected.raw);assert.equal(item.responseStatus,expected.code);
            const finished=expected.code===101||expected.code===200||expected.code===302&&kind!=='redirect-held-abort';assert.equal(item.fixtureEnded,finished);
            const prefix=Buffer.from(expected.raw+'\r\n\r\n','latin1');assert.ok(sent.subarray(0,prefix.length).equals(prefix),'complete source response header');
            if(expected.code===101){
                assert.equal(pending,false);const messages=kind==='tls-ipv6-proxy'?[message(2,payload(challenge)),close('done')]:[message(1,'echo/'+challenge),message(2,Buffer.from([1,2,3,4,5])),close('child')];
                exactMessages(sourceMessages,messages,'source received');exactMessages(item.sentFrames,messages,'source sent');assert.ok(sent.subarray(prefix.length).equals(Buffer.concat(messages.map(item=>encodedFrame(item.opcode,bytes(item.payload))))),'complete source response data');events.push('upgrade','close-reply');
            }else{assert.deepEqual(sourceMessages,[]);assert.deepEqual(item.sentFrames,[]);assert.ok(sent.subarray(prefix.length).equals(expected.body),'exact held body prefix or complete body');events.push('response');}
            responses.push({...expected,role:item.role});
        }
        if(item.peerEnded)events.push('peer-ended');assert.deepEqual(item.events,events,'complete source lifecycle');
    });
    return responses;
}
function readerState(value){
    const {kind,challenge}=value,noPending=['-no-pending','-before-first','-eof'].some(s=>kind.endsWith(s)),standard=kind.startsWith('standard-reader-probe'),second=kind.endsWith('-pending-two');
    const form=/(?:reader-probe)-(direct|prototype|subclass|reflect)-/.exec(kind)?.[1];
    const error={type:'object',name:kind.endsWith('-default')||kind.endsWith('-string')||kind.endsWith('-error')||kind.endsWith('-null')?'Error':null,
        message:kind.endsWith('-default')?'Stream was cancelled.':kind.endsWith('-string')||kind.endsWith('-error')?'cancel/'+challenge:kind.endsWith('-null')?'null':null,primitive:null,sameReason:false};
    return {kind,challenge,first:kind.endsWith('-before-first')?null:part(),pending:noPending?null:standard?{value:{done:true,bytes:0}}:{error},
        second:second?(standard?{value:{done:true,bytes:0}}:{error:{type:'object',name:'TypeError',message:'This ReadableStream only supports a single pending read request at a time.',primitive:null,sameReason:false}}):null,
        closed:{fulfilled:true},cancel:{fulfilled:true,returnedUndefined:true},after:{value:{done:true,bytes:0}},again:{fulfilled:true},
        trace:standard?['read',...(second?['second']:[]),'closed','cancel']:[...(second?['second-error']:[]),'closed','cancel',...(noPending?[]:['read-error'])],bodyUsed:true,locked:true,unexpected:null,lockedAfterRelease:false,
        ...(form?{construction:{mode:form,instanceOfBase:true,expectedPrototype:true,expectedConstructor:true,prototypeConstructorIsBase:true}}:{})};
}
function scopeState(value,pending=false){
    const {kind,challenge}=value,ended=kind.endsWith('-after-close');return {kind,challenge,timerFired:!pending&&!ended,resolverCalled:!pending&&(!ended||kind==='http-created-promise-after-close'),future:!pending&&!ended,parentClosed:!pending};
}
function networkState(value,mode,responses){
    const {kind,challenge}=value,p=profile(kind),observed=JSON.parse(value.observed.text).state,proxy=kind==='tls-ipv6-proxy';
    const aborted=kind.includes('abort')&&kind!=='tls-ipv6-wrong-host',cancelled=kind==='tls-body-reader-cancel',failed=p.badTLS||kind==='redirect-https-denied'||aborted||cancelled;
    const received=p.goodUpgrade||p.bodyActor,last=responses.at(-1),hasPrefix=p.bodyActor&&kind!=='tls-body-abort-before-read';
    const result=received?{status:last.code,statusText:last.reason,url:p.redirect?`${p.targetTLS?'https':'http'}://${p.targetHost}:${value.targetPort}/land?challenge=${challenge}`:value.target,
        redirected:p.redirect,type:'default',bodyNull:p.goodUpgrade,bodyUsed:false,headers:last.headers,cookies:p.goodUpgrade?['first=1','second=2']:[],hasWebSocket:p.goodUpgrade,body:null}:null;
    const closed={code:1000,reason:'child',wasClean:true},events=[{type:'close',...closed}];
    if(mode==='reference'&&observed.socket?.events?.length===2)events.unshift({type:'error',message:'Uncaught Error: Network connection lost.',error:'Error: Network connection lost.'});
    const socket=p.goodUpgrade&&!proxy?{before:1,messages:[{type:'text',value:'echo/'+challenge},{type:'binary',bytes:5,sha256:digest(Buffer.from([1,2,3,4,5]))}],events,closed}:null;
    return {kind,challenge,trace:proxy?['returned']:['fetch',...(received?['headers']:[]),...(hasPrefix?['body-first']:[]),...(aborted?['abort']:cancelled?['cancel']:[]),...(p.contextEnd?[]:[failed?'failure':'complete']),'parent-close'],
        result,firstBody:hasPrefix?part():null,failure:p.badTLS||kind==='redirect-https-denied'?failure(observed.failure,value.stderr):failed?{type:'object',name:null,message:null,primitive:null}:null,
        sameReason:false,socket,signal:{aborted,reasonIsSentinel:aborted},action:aborted||cancelled?{name:aborted?'abort':'cancel',returned:true,error:null}:null,
        future:!proxy&&!p.contextEnd,bodyUsedAtFirst:hasPrefix?true:null,bodyUsedAtEnd:proxy||p.contextEnd||!received?null:p.goodUpgrade?false:true};
}
export function assertFetchTransitionCase(value,mode){
    assert.ok(['reference','single-worker','static-pool','managed'].includes(mode));
    fields(value,['kind','challenge','target','sourcePort','targetPort','allowedAuthorities','initial','headers','wire','observed','allSourcesEndedBeforeStop','upstreamBeforeStop','shutdown','stderr','upstreamAfterStop','checkpoint','checkpointUpstream','followedBeforeRelease','binary','foreign','failure','cleanupFailure'],'complete transition case');
    assert.ok(fetchTransitionCases.includes(value.kind));assert.match(value.challenge,uuid);port(value.sourcePort);if(value.targetPort!==null)port(value.targetPort);
    assert.equal(value.failure,null);assert.equal(value.cleanupFailure,null);assert.equal(typeof value.stderr,'string');assert.doesNotMatch(value.stderr,/AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON/);
    const {kind,challenge}=value,p=profile(kind);assert.equal(value.target,`${p.sourceTLS?'https':'http'}://${p.sourceHost}:${value.sourcePort}/probe?challenge=${challenge}`);
    assert.equal(value.targetPort!==null,p.redirect);const allowed=[p.sourceHost+':'+value.sourcePort];if(p.redirect&&kind!=='redirect-https-denied')allowed.push(p.targetHost+':'+value.targetPort);assert.deepEqual(value.allowedAuthorities,allowed,'actual Host and App permission intersection');
    fields(value.initial,['status','text'],'initial state');assert.equal(value.initial.status,200);const initial=JSON.parse(value.initial.text);fields(initial,['realm','state'],'initial document');assert.match(initial.realm,uuid);assert.equal(initial.state,null);
    const seen=new Set(),responses=upstream(value,seen);wire(value,seen);
    const expected=p.scope?scopeState(value):p.reader?readerState(value):networkState(value,mode,responses);
    assertState(value.observed,initial.realm,expected,'literal same-realm result, reason type and Promise reaction order');
    assert.equal(value.followedBeforeRelease,null);assert.deepEqual(value.foreign,kind==='http-created-promise-after-close'?{status:200,text:'resolved'}:null);
    if(p.scopeEnd||p.bodyActor||p.pendingActor){
        let state;
        if(p.scopeEnd)state=scopeState(value,true);
        else{state=structuredClone(expected);Object.assign(state,{trace:['fetch',...(p.bodyActor?['headers']:[]),...(p.bodyActor&&kind!=='tls-body-abort-before-read'?['body-first']:[])],failure:null,sameReason:false,socket:null,signal:{aborted:false,reasonIsSentinel:false},action:null,future:false,bodyUsedAtEnd:null});}
        assertState(value.checkpoint,initial.realm,state,'actual pending phase before external cancellation');upstream(value,new Set(),true);
        // The pending bytes, socket identities and HTTP/SNI observations must
        // survive into the terminal capture; fixture shutdown contributes none.
        for(const [i,row] of value.checkpointUpstream.records.entries()){
            const final=value.upstreamBeforeStop.records[i];for(const key of ['role','localAddress','localPort','remoteAddress','remotePort','servername','request','rawRequest','responseHeader','responseStatus'])assert.equal(row[key],final[key],'same pending source '+key);
            for(const key of ['receivedChunks','sentChunks'])assert.ok(chunks(row[key]).equals(chunks(final[key])),'pending and terminal raw bytes agree');
        }
        assert.deepEqual(value.checkpointUpstream.sni,value.upstreamBeforeStop.sni);
    }else{assert.equal(value.checkpoint,null);assert.equal(value.checkpointUpstream,null);}
    fields(value.shutdown,['code','signal','forced'],'normal service shutdown');assert.equal(value.shutdown.forced,false);
    if(mode==='reference')assert.ok(value.shutdown.code===0&&value.shutdown.signal===null||value.shutdown.code===null&&value.shutdown.signal==='SIGTERM');else assert.deepEqual(value.shutdown,{code:0,signal:null,forced:false});
    return {kind,status:expected.result?.status??null,failed:expected.failure!==undefined&&expected.failure!==null};
}
function certificates(value){
    fields(value,['opensslVersion','opensslSHA256','caSHA256','ipv6ServerSHA256','ipServerSHA256','serverSHA256','untrustedSHA256','serverSAN','ipv6ServerSAN','ipServerSAN','untrustedSAN','caPEM','serverPEM','ipv6ServerPEM','ipServerPEM','untrustedPEM'],'certificate evidence');
    assert.match(value.opensslVersion,/^OpenSSL \S+/);assert.match(value.opensslSHA256,/^[a-f0-9]{64}$/);
    const certs={};for(const [name,hashName] of [['ca','ca'],['server','server'],['ipv6Server','ipv6Server'],['ipServer','ipServer'],['untrusted','untrusted']]){const pem=value[name+'PEM'];assert.equal(digest(pem),value[hashName+'SHA256']);certs[name]=new X509Certificate(pem);}
    assert.equal(certs.ca.ca,true);for(const name of ['server','ipv6Server','ipServer'])assert.equal(certs[name].verify(certs.ca.publicKey),true);assert.equal(certs.untrusted.verify(certs.ca.publicKey),false);
    for(const [name,san] of [['server','DNS:localhost'],['ipv6Server','IP Address:0:0:0:0:0:0:0:1'],['ipServer','IP Address:127.0.0.1'],['untrusted','DNS:localhost']]){assert.equal(certs[name].subjectAltName,san);assert.equal(value[name+'SAN'],san);}
    assert.equal(certs.server.checkIP('127.0.0.1'),undefined);assert.equal(certs.server.checkIP('::1'),undefined);assert.equal(certs.ipServer.checkIP('::1'),undefined);assert.equal(certs.ipv6Server.checkIP('::1'),'::1');assert.equal(certs.server.checkHost('localhost'),'localhost');
}
export {certificates as assertFetchTransitionCertificates};
export function validateFetchTransitionReport(report,expected){
    fields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','certificateInputs','cases'],'transition report');
    assert.equal(report.schema,'capsid-fetch-transition-v1');assert.equal(report.complete,true);assert.equal(report.runId,expected.runId);assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));assert.deepEqual(report.referenceLock,expected.lock);
    assert.deepEqual(report.inputs,expected.inputs);assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(fetchTransitionInputPaths).sort());for(const hash of Object.values(report.inputs))assert.match(hash,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,worker:report.mode==='reference'?null:expected.worker});
    certificates(report.certificateInputs);assert.deepEqual(report.cases.map(value=>value.kind),fetchTransitionCases);assert.equal(new Set(report.cases.map(value=>value.challenge)).size,fetchTransitionCases.length);assert.equal(new Set(report.cases.map(value=>JSON.parse(value.initial.text).realm)).size,fetchTransitionCases.length);
    return report.cases.map(value=>assertFetchTransitionCase(value,report.mode));
}
export function compareFetchTransitionReports(reference,candidates,expected){
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(value=>value.mode).sort(),referenceBackendModes(expected.lock).sort());const baseline=validateFetchTransitionReport(reference,expected);
    for(const candidate of candidates)assert.deepEqual(validateFetchTransitionReport(candidate,expected),baseline);return {complete:true,casesPerBackend:fetchTransitionCases.length,backends:candidates.length};
}
