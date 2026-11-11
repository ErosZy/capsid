import assert from 'node:assert/strict';
import {standardCloseCases,assertStandardCloseState,standardCloseDiagnosticReferences} from './workerd-standard-close-contract.mjs';
import {createHash} from 'node:crypto';
import {gunzipSync} from 'node:zlib';
import {referenceBackendModes} from './workerd-platform.mjs';
import {assertFetchTransitionCertificates} from './workerd-fetch-transition-contract.mjs';

const routes=['getReader','direct','prototype','subclass','reflect'];
const views=['Uint8Array','Uint16Array','Int32Array','Float64Array','DataView'];
const methods=['default','min2','min3','least2','least3'];
const additionalViews=['Int8Array','Uint8ClampedArray','Int16Array','Uint32Array','Float16Array','Float32Array','BigInt64Array','BigUint64Array'];
const allCases=['native','standard'].flatMap(source=>
    (source==='native'?routes:routes.slice(0,2)).flatMap(route=>views.flatMap(view=>methods.flatMap(method=>
        ['held','eof'].map(phase=>`${source}-${route}-${view}-${method}-${phase}`)))));
const widths={Int8Array:1,Uint8Array:1,Uint8ClampedArray:1,Int16Array:2,Uint16Array:2,
    Int32Array:4,Uint32Array:4,Float16Array:2,Float32Array:4,Float64Array:8,BigInt64Array:8,BigUint64Array:8,DataView:1};
// Keep the original435 cases in their reviewed order. The last36 cases
// complete the standard-source Cartesian matrix and have their own shard.
const originalDeferred=allCases.filter(kind=>{
    const [source,,view,method]=kind.split('-');return source==='standard'&&(method==='default'?1:Number(method.at(-1)))*widths[view]>5;
});
const capacityCases=originalDeferred.filter(kind=>{const [,,view,method]=kind.split('-');return Number(method.at(-1))*widths[view]>16;});
const standardEdges=originalDeferred.filter(kind=>!capacityCases.includes(kind));
export const fetchBYOBDeferredStandardCases=Object.freeze([]);
export const fetchBYOBCases=Object.freeze([
    ...allCases.filter(kind=>!originalDeferred.includes(kind)),
    ...additionalViews.flatMap(view=>methods.flatMap(method=>['held','eof'].map(phase=>`native-getReader-${view}-${method}-${phase}`))),
    ...routes.flatMap(route=>['default','byob'].map(reader=>`brand-${route}-${reader}-held`)),
    ...capacityCases,
    ...routes.flatMap(route=>['read','least'].map(method=>`validation-${route}-${method}`)),
    ...routes.flatMap(route=>['empty','small'].map(phase=>`body-${route}-default-${phase}`)),
    ...routes.flatMap(route=>['minimum','least'].map(method=>`body-${route}-${method}-large`)),
    'body-getReader-default-large',
    ...standardEdges,
    ...standardCloseCases,
]);
export const fetchBYOBShardNames=Object.freeze(['readers','constructors','reflect-standard','views-brands','bodies','standard-edges','standard-close']);
export const fetchBYOBShardCases=Object.freeze({
    readers:Object.freeze(fetchBYOBCases.slice(0,100)),constructors:Object.freeze(fetchBYOBCases.slice(100,200)),
    'reflect-standard':Object.freeze(fetchBYOBCases.slice(200,306)),'views-brands':Object.freeze(fetchBYOBCases.slice(306,414)),
    bodies:Object.freeze(fetchBYOBCases.slice(414,435)),
    'standard-edges':Object.freeze(fetchBYOBCases.slice(435,471)),
    'standard-close':Object.freeze(fetchBYOBCases.slice(471)),
});
export const fetchBYOBInputPaths=Object.freeze({runner:'tests/test_websocket_fetch_byob.mjs',
    fixture:'tests/fixtures/workerd-fetch-byob.mjs',upstream:'tests/helpers/ws_fetch_byob_peer.mjs',
    upstreamBase:'tests/helpers/ws_fetch_transition_peer.mjs',controlPeer:'tests/helpers/ws_response_client.mjs',
    hostBackend:'tests/helpers/ws_host_backend.mjs',contract:'tools/workerd-fetch-byob-contract.mjs',
    certificateContract:'tools/workerd-fetch-transition-contract.mjs',standardCloseContract:'tools/workerd-standard-close-contract.mjs'});
export function parseWorkerdListenPort(line,socketName='http') {
    const event=JSON.parse(line);
    fields(event,['event','socket','port'],'workerd listener control event');
    assert.equal(event.event,'listen','workerd listener control event type');
    assert.equal(event.socket,socketName,'workerd listener control socket');
    assert.ok(Number.isInteger(event.port)&&event.port>0&&event.port<=65535,'workerd listener bound port');
    return event.port;
}
const uuid=/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
function fields(value,keys,label){assert.ok(value&&typeof value==='object'&&!Array.isArray(value),label);
    assert.deepEqual(Object.keys(value).sort(),[...keys].sort(),label);}
function bytes(value){assert.equal(typeof value,'string');const data=Buffer.from(value,'base64');
    assert.equal(data.toString('base64'),value,'canonical observed bytes');return data;}
function bodyBytes(value){
    if(typeof value==='string')return bytes(value); // Retained pre-compression captures remain reviewable.
    fields(value,['encoding','rawBytes','sha256','payload'],'lossless actual body transcript');
    assert.equal(value.encoding,'gzip-base64');assert.ok(Number.isInteger(value.rawBytes)&&value.rawBytes>0&&value.rawBytes<=1048613);
    assert.match(value.sha256,/^[a-f0-9]{64}$/);
    const raw=gunzipSync(bytes(value.payload),{maxOutputLength:1048613});
    assert.equal(raw.length,value.rawBytes);assert.equal(createHash('sha256').update(raw).digest('hex'),value.sha256);return raw;
}
function port(value){assert.ok(Number.isInteger(value)&&value>0&&value<65536);}
const error=(name,message)=>({error:{type:'object',name,message,primitive:null}});
const data=[102,105,114,115,116];
function readValue(type,offset,length,bufferBytes,content,backing,done=false){
    return {done,type,byteOffset:offset,byteLength:length,bufferBytes,bytes:content,backing};
}
const illegalInvocation='Illegal invocation: function called with incorrect `this` reference. '
    +'See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.';
const nativeBodyContents=new Map();
function nativeBodyContent(length){
    if(!nativeBodyContents.has(length)){
        const bytes=Buffer.alloc(length);for(let i=0;i<length;i++)bytes[i]=(i*29+17)%251;
        nativeBodyContents.set(length,{bytes,sha256:createHash('sha256').update(bytes).digest('hex')});
    }
    return nativeBodyContents.get(length);
}
function expectedBrandState(kind,challenge){
    const [,route,reader]=kind.split('-'),byob=reader==='byob';
    const attempts=(byob?['read','closed','releaseLock','cancel']:['read','readAtLeast','closed','releaseLock','cancel'])
        .map(name=>({name,synchronous:true,...error('TypeError',illegalInvocation)}));
    return {kind,challenge,read:error('Error','valid/'+challenge),closed:{fulfilled:true},cancel:{returnedUndefined:true},unexpected:null,
        construction:{instanceOfReader:true,constructorMatches:true,directPrototype:!['subclass','reflect'].includes(route),
            prototypeParent:['subclass','reflect'].includes(route)},
        prime:{done:false,type:'Uint8Array',bytes:byob?[0]:[0,...data]},optionGets:0,
        attempts,afterReleaseAttempts:attempts,wrongInputBytes:16,releasedWrongInputBytes:16,
        beforeCancel:{readSettled:false,closedSettled:false,locked:true},bodyUsed:true,lockedAfterRelease:false};
}
function unalignedStandardEOF(kind){
    if(!kind.startsWith('standard-'))return false;
    const [source,,view,method,phase]=kind.split('-'),width=widths[view];
    const minimum=(method==='default'?1:Number(method.at(-1)))*width;
    return source==='standard'&&phase==='eof'&&minimum>5&&minimum<=16&&width<=5&&5%width!==0;
}
function linkedEOFDiagnostic(value){
    if(!unalignedStandardEOF(value.kind))return undefined;
    const state=JSON.parse(value.observed.text).state;
    const message=state.read?.error?.message;
    assert.equal(typeof message,'string');
    const match=/^internal error; reference = ([0-9a-v]{24})$/.exec(message);
    assert.ok(match,'fresh120-bit opaque EOF diagnostic');
    // Both engines must connect the exposed reference to a real diagnostic.
    const logged=[...value.stderr.matchAll(/(?:wdErrId = |reference = )([0-9a-v]{24})(?![0-9a-v])/g)].map(m=>m[1]);
    assert.deepEqual(logged,[match[1]],'exactly one matching EOF diagnostic in captured stderr');
    return message;
}
function uniqueEOFDiagnostics(cases){
    const messages=cases.flatMap(value=>value.kind.startsWith('standard-close-')?
        standardCloseDiagnosticReferences(JSON.parse(value.observed.text).state,value.kind):
        unalignedStandardEOF(value.kind)?[linkedEOFDiagnostic(value).slice(-24)]:[]);
    assert.equal(new Set(messages).size,messages.length,'independent errors receive fresh diagnostic references');
}
function expectedState(kind,challenge,diagnosticMessage){
    if(kind.startsWith('brand-'))return expectedBrandState(kind,challenge);
    const [source,route,view,method,phase]=kind.split('-'),width=widths[view];
    const minimum=(method==='default'?1:Number(method.at(-1)))*width,invalid=minimum>16,pending=phase==='held'&&minimum>5&&!invalid;
    const backing=Array(32).fill(165);backing.splice(8,5,...data);
    const read=invalid?error('TypeError','Minimum bytes to read (24) exceeds size of buffer (16).'):
        pending?error(null,null):{value:readValue('Uint8Array',8,5,32,data,backing)};
    const state={kind,challenge,trace:pending?['closed','cancel','read-error']:[invalid?'read-error':'read','closed','cancel'],
        read,eofRead:phase==='eof'&&!invalid?{value:readValue('Uint8Array',0,0,16,[],Array(16).fill(0),true)}:null,
        closed:{fulfilled:true},cancel:{returnedUndefined:true},unexpected:null,
        construction:{instanceOfReader:true,constructorMatches:true,directPrototype:!['subclass','reflect'].includes(route),
            prototypeParent:['subclass','reflect'].includes(route)},
        prime:readValue('Uint8Array',0,1,1,[0],[0]),input:{type:view,byteOffset:8,byteLength:16,bufferBytes:32},
        afterCall:{bufferBytes:invalid?32:0,viewBytes:invalid?16:view==='DataView'?{error:{name:'TypeError'}}:0},
        beforeCancel:{readSettled:!pending,closedSettled:false},bodyUsed:true,lockedAfterRelease:false};
    if(source==='standard'){
        state.bodyUsed=null;
        if(invalid)return state;
        const length=Math.floor(5/width)*width,alignedBacking=Array(32).fill(165);alignedBacking.splice(8,length,...data.slice(0,length));
        state.read={value:readValue(view,8,length,32,data.slice(0,length),alignedBacking)};
        if(minimum>5){
            // Cancellation returns complete elements before closed, retaining
            // sub-element bytes in the backing of an empty typed view.
            if(length===0)state.read.value.backing=backing;
            state.trace=phase==='eof'?['closed','read','cancel']:['read','closed','cancel'];
            state.beforeCancel={readSettled:phase==='eof',closedSettled:phase==='eof'};
            if(unalignedStandardEOF(kind)){
                state.trace=['closed-error','read-error','cancel-error'];
                state.read=error('Error',diagnosticMessage);state.closed=error('Error',diagnosticMessage);
                state.cancel=error('Error',diagnosticMessage);state.eofRead=null;
                state.errorIdentity={readClosed:true,readCancel:true};
            }
            return state;
        }
        if(phase==='eof'){
            const closed=length===5;state.beforeCancel.closedSettled=closed;
            state.trace=closed?['closed','read','cancel']:['read','closed','cancel'];
            const rest=data.slice(length),restBacking=Array(16).fill(0);restBacking.splice(0,rest.length,...rest);
            state.eofRead={value:readValue('Uint8Array',0,rest.length,16,rest,restBacking,closed)};
        }
    }
    return state;
}
const validationRows=['capacity','capacity-first-count','zero','negative','signed-max','signed-min','range-high','range-low',
    'range-fraction','string-capacity','object-capacity','primitive-capacity','bigint','symbol','object-bigint','object-symbol',
    'zero-null','zero-nan','zero-infinity','zero-fraction','conversion-error','primitive-error','primitive-noncallable',
    'released-capacity','released-zero','released-object-capacity','released-conversion-error','release-during-conversion',
    'empty-capacity','empty-zero','detached-capacity','detach-during-conversion','invalid-view','invalid-view-conversion-error',
    'wrong-receiver','proxy-receiver','valid-default','valid-minimum','valid-fraction','valid-string'];
function expectedValidationState(kind,challenge){
    const [,route,method]=kind.split('-'),payload=Array.from({length:16},(_,i)=>i+1);
    const scenarios=[...validationRows,...(method==='read'?['options-getter-error','options-null','options-primitive','getter-detaches-then-throws']:[])];
    const rows=scenarios.map(scenario=>{
        const wrong=['wrong-receiver','proxy-receiver'].includes(scenario),invalid=scenario.startsWith('invalid-view');
        const gets=method==='read'&&!wrong&&!invalid&&!['options-null','options-primitive'].includes(scenario);
        const valueOf=['object-capacity','object-bigint','object-symbol','conversion-error','released-object-capacity','released-conversion-error',
            'release-during-conversion','detach-during-conversion'].includes(scenario)||scenario==='invalid-view-conversion-error'&&method==='least';
        const primitive=['primitive-capacity','primitive-error'].includes(scenario);
        const events=[...(gets?['get-min']:[]),...(valueOf?['valueOf']:[]),...(primitive?['primitive:number']:[]),...(wrong?[]:['returned'])];
        const success=['valid-minimum','valid-fraction','valid-string','options-null'].includes(scenario)||scenario==='valid-default'&&method==='read';
        const detached=success||['detached-capacity','detach-during-conversion','getter-detaches-then-throws'].includes(scenario);
        const row={scenario,events,construction:{instanceOfReader:true,constructorMatches:true,
            directPrototype:!['subclass','reflect'].includes(route),prototypeParent:['subclass','reflect'].includes(route)},
            afterCall:{bufferBytes:detached?0:32,viewBytes:invalid?null:detached||scenario.startsWith('empty-')?0:16}};
        if(success){
            const backing=[...Array(8).fill(165),...payload,...Array(8).fill(165)];
            row.read={value:readValue('Float64Array',8,16,32,payload,backing)};
        }else{
            const callback=['conversion-error','released-conversion-error','options-getter-error','getter-detaches-then-throws'].includes(scenario)||
                scenario==='invalid-view-conversion-error'&&method==='least';
            let message;
            if(callback)message='validation/'+challenge+'/'+scenario;
            else if(wrong)message=illegalInvocation;
            else if(invalid)message=`Failed to execute '${method==='read'?'read':'readAtLeast'}' on 'ReadableStreamBYOBReader': parameter ${method==='read'?1:2} is not of type 'ArrayBufferView'.`;
            else if(scenario.startsWith('released-')||scenario==='release-during-conversion')message='This ReadableStream reader has been released.';
            else if(scenario.startsWith('empty-')||['detached-capacity','detach-during-conversion'].includes(scenario))message='You must call read() on a "byob" reader with a positive-sized TypedArray object.';
            else if(scenario==='options-primitive')message="Failed to execute 'read' on 'ReadableStreamBYOBReader': parameter 2 is not of type 'ReadableStreamBYOBReaderReadOptions'.";
            else if(scenario.startsWith('range-'))message='Value out of range. Must be between -2147483648 and 2147483647 (inclusive).';
            else if(scenario==='bigint'||scenario==='object-bigint')message='Cannot convert a BigInt value to a number';
            else if(scenario==='symbol'||scenario==='object-symbol')message='Cannot convert a Symbol value to a number';
            else if(scenario==='primitive-error')message='Cannot convert object to primitive value';
            else if(scenario==='primitive-noncallable')message="'3' returned for property 'Symbol(Symbol.toPrimitive)' of object '#<Object>' is not a function";
            else if(scenario.startsWith('zero')||scenario==='valid-default')message='Requested invalid minimum number of bytes to read (0).';
            else message=`Minimum bytes to read (${({'capacity-first-count':'17',negative:'18446744073709551615','signed-max':'2147483647','signed-min':'18446744071562067968'})[scenario]??'24'}) exceeds size of buffer (16).`;
            const failure={type:'object',name:callback?'Error':'TypeError',message,primitive:null,isTypeError:!callback,isRangeError:false,sameSentinel:callback};
            if(wrong)row.synchronous=failure;else row.read={error:failure};
        }
        row.originalBacking=detached?null:Array(32).fill(165);
        if(!success)row.retry=readValue('Uint8Array',0,16,16,payload,payload);
        row.eof=readValue('Uint8Array',0,0,1,[],[0],true);
        row.cancelReturnedUndefined=true;row.lockedAfterRelease=false;
        return row;
    });
    return {kind,challenge,rows,unexpected:null};
}
function assertBodyState(state,kind,challenge){
    fields(state,['kind','challenge','shapes','total','invalidBytes','unexpectedViews','closed','unexpected',
        'construction','response','input','sha256','bodyUsed','lockedAfterRelease'],'complete native body observation');
    const [,route,method,phase]=kind.split('-'),length=phase==='empty'?0:phase==='small'?6:1048613,byob=method!=='default';
    assert.equal(state.kind,kind);assert.equal(state.challenge,challenge);assert.equal(state.unexpected,null);
    assert.deepEqual(state.construction,{instanceOfReader:true,constructorMatches:true,
        directPrototype:!['subclass','reflect'].includes(route),prototypeParent:['subclass','reflect'].includes(route)});
    assert.deepEqual(state.response,{status:200,contentLength:String(length)});
    assert.deepEqual(state.input,byob?{type:'Uint8Array',byteOffset:16,byteLength:length+32,bufferBytes:length+64,min:length}:null);
    assert.equal(state.total,length);assert.equal(state.invalidBytes,0);assert.equal(state.unexpectedViews,0);
    assert.equal(state.sha256,nativeBodyContent(length).sha256,'complete ordered body, including bytes beyond initial credit');
    assert.deepEqual(state.closed,{fulfilled:true});assert.equal(state.bodyUsed,true);assert.equal(state.lockedAfterRelease,false);
    if(byob){
        assert.deepEqual(state.shapes,[[false,16,length,length+64,0,true],[true,0,0,length+64,0,true]],
            'full-body minimum crosses the 64 KiB window, detaches each input and preserves offset and outside bytes');
        return;
    }
    assert.ok(Array.isArray(state.shapes)&&state.shapes.length>0&&state.shapes.length<=length+1);
    assert.deepEqual(state.shapes.at(-1),[true,null,null,null,null,null],'DefaultReader observes an empty EOF');
    let consumed=0;
    for(const row of state.shapes.slice(0,-1)){
        assert.ok(Array.isArray(row)&&row.length===6);
        const [done,offset,count,capacity,detached,outside]=row;
        assert.equal(done,false);assert.equal(offset,0);assert.ok(Number.isInteger(count)&&count>0&&count<=4096);
        assert.equal(capacity,4096,'native DefaultReader allocates the pinned 4096-byte backing store');
        assert.equal(detached,null);assert.equal(outside,true,'unused allocation bytes remain zero');consumed+=count;
    }
    assert.equal(consumed,length);
    // Transport segmentation is observable in the raw shapes, but is not fixed
    // by TCP/TLS. Small controlled bodies fit in one transport fragment.
    if(phase==='small')assert.deepEqual(state.shapes,[[false,0,6,4096,null,true],[true,null,null,null,null,null]]);
}
function wire(value,nonces){
    for(const [key,final] of [['headers',false],['wire',true]]){
        const wire=value[key];fields(wire,['status','rawHeader','requestKey','rawTail','frames','peerTerminated','transportError','completion','closeReplySent'],'actual downstream wire');
        assert.equal(wire.status,101);assert.match(wire.requestKey,/^[A-Za-z0-9+/]{22}==$/);
        if(!final){assert.ok(!nonces.has(wire.requestKey));nonces.add(wire.requestKey);}
        else assert.equal(wire.requestKey,value.headers.requestKey);
        const accept=createHash('sha1').update(wire.requestKey+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        const lines=wire.rawHeader.split('\r\n');assert.equal(lines.shift(),'HTTP/1.1 101 Switching Protocols');
        assert.deepEqual(lines.map(line=>{const at=line.indexOf(':');assert.ok(at>0);return [line.slice(0,at).toLowerCase(),line.slice(at+1).trim()];}).sort(),
            [['connection','Upgrade'],['upgrade','websocket'],['sec-websocket-accept',accept]].sort());
        if(!final){assert.deepEqual(wire.frames,[]);assert.equal(wire.rawTail,'');assert.equal(wire.peerTerminated,false);
            assert.equal(wire.transportError,null);assert.equal(wire.completion,null);assert.equal(wire.closeReplySent,false);continue;}
        const frames=[{opcode:1,payload:Buffer.from('started').toString('base64')},{opcode:1,payload:Buffer.from('done').toString('base64')},
            {opcode:8,payload:Buffer.from([3,232,100,111,110,101]).toString('base64')}];
        assert.deepEqual(wire.frames,frames);assert.ok(bytes(wire.rawTail).equals(Buffer.concat(frames.map(frame=>{
            const payload=bytes(frame.payload);return Buffer.concat([Buffer.from([128|frame.opcode,payload.length]),payload]);}))));
        assert.equal(wire.peerTerminated,true);assert.ok(['peer-ended','peer-reset'].includes(wire.completion));
        assert.equal(wire.transportError,wire.completion==='peer-reset'?'ECONNRESET':null);assert.equal(wire.closeReplySent,true);
    }
}
function upstream(value,nonces){
    const snapshot=value.upstreamBeforeStop;fields(snapshot,['records','tcpRecords','sni','tlsErrors'],'actual upstream snapshot');
    assert.deepEqual(value.upstreamAfterStop,snapshot,'fixture cleanup cannot supply completion');assert.equal(value.allSourcesEndedBeforeStop,true);
    const quiet=value.kind.startsWith('standard-')||value.kind.startsWith('validation-'),bodyCase=value.kind.startsWith('body-'),eof=bodyCase||value.kind.endsWith('-eof');
    assert.deepEqual(snapshot.tlsErrors,[]);assert.deepEqual(snapshot.sni,quiet?[]:[{role:'source',servername:'localhost'}]);
    assert.equal(snapshot.records.length,quiet?0:1);assert.equal(snapshot.tcpRecords.length,quiet?0:1);
    if(quiet)return;
    const tcp=snapshot.tcpRecords[0];fields(tcp,['role','localAddress','localPort','remoteAddress','remotePort','ended','closed','error'],'TCP evidence');
    assert.equal(tcp.role,'source');assert.equal(tcp.localPort,value.sourcePort);port(tcp.remotePort);
    assert.ok(['127.0.0.1','::1'].includes(tcp.localAddress));assert.equal(tcp.remoteAddress,tcp.localAddress);
    assert.equal(tcp.closed,true);assert.equal(typeof tcp.ended,'boolean');assert.equal(tcp.error,null);
    const row=snapshot.records[0];fields(row,['role','localAddress','localPort','remoteAddress','remotePort','servername','request','rawRequest','responseHeader','responseStatus','frames','sentFrames','receivedChunks','sentChunks','events','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','error'],'HTTP evidence');
    for(const key of ['role','localAddress','localPort','remoteAddress','remotePort'])assert.equal(row[key],tcp[key]);
    assert.equal(row.servername,'localhost');assert.equal(row.error,null);assert.equal(row.fixtureDestroyed,false);assert.equal(row.fixtureEnded,eof);
    assert.ok(row.peerEnded||row.peerReset);assert.equal(typeof row.peerEnded,'boolean');assert.equal(typeof row.peerReset,'boolean');
    assert.deepEqual(row.frames,[]);assert.deepEqual(row.sentFrames,[]);
    const request=Buffer.concat(row.receivedChunks.map(bytes));assert.ok(request.equals(bytes(row.rawRequest)));assert.equal(request.toString('latin1'),row.request+'\r\n\r\n');
    const lines=row.request.split('\r\n');assert.equal(lines.shift(),`GET /probe?challenge=${value.challenge} HTTP/1.1`);
    const headers=lines.map(line=>{const at=line.indexOf(':');assert.ok(at>0);return [line.slice(0,at).toLowerCase(),line.slice(at+1).trim()];});
    const keys=headers.filter(([name])=>name==='sec-websocket-key');assert.equal(keys.length,1);const key=keys[0][1];assert.match(key,/^[A-Za-z0-9+/]{22}==$/);
    assert.ok(!nonces.has(key));nonces.add(key);
    assert.deepEqual(headers.sort(),[['connection','Upgrade'],['host','localhost:'+value.sourcePort],['sec-websocket-key',key],
        ['sec-websocket-version','13'],['upgrade','websocket'],['x-probe',value.challenge]].sort());
    const phase=value.kind.split('-').at(-1),length=bodyCase?(phase==='empty'?0:phase==='small'?6:1048613):6;
    const content=bodyCase?nativeBodyContent(length).bytes:Buffer.from([0,...data]);
    const header=`HTTP/1.1 ${eof?'200 OK':'409 Conflict'}\r\nContent-Length: ${eof?length:100}\r\nX-Probe: ${value.challenge}\r\nConnection: close`;
    assert.equal(row.responseHeader,header);assert.equal(row.responseStatus,eof?200:409);
    assert.ok(Buffer.concat(row.sentChunks.map(bodyCase?bodyBytes:bytes)).equals(Buffer.concat([Buffer.from(header+'\r\n\r\n'),content])));
    assert.deepEqual(row.events,row.peerEnded?['request','response','peer-ended']:['request','response']);
}
export function assertFetchBYOBCase(value,mode){
    fields(value,['kind','challenge','target','sourcePort','allowedAuthorities','initial','headers','wire','observed','allSourcesEndedBeforeStop',
        'upstreamBeforeStop','shutdown','stderr','upstreamAfterStop','failure','cleanupFailure'],'complete BYOB case');
    assert.ok(fetchBYOBCases.includes(value.kind));assert.match(value.challenge,uuid);port(value.sourcePort);
    assert.equal(value.target,`https://localhost:${value.sourcePort}/probe?challenge=${value.challenge}`);
    assert.deepEqual(value.allowedAuthorities,['localhost:'+value.sourcePort]);assert.equal(value.failure,null);assert.equal(value.cleanupFailure,null);
    assert.equal(typeof value.stderr,'string');assert.doesNotMatch(value.stderr,/AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON/);
    for(const name of ['initial','observed']){fields(value[name],['status','text'],'same VM observation');assert.equal(value[name].status,200);}
    const initial=JSON.parse(value.initial.text),observed=JSON.parse(value.observed.text);
    fields(initial,['realm','state'],'initial document');fields(observed,['realm','state'],'observed document');assert.match(initial.realm,uuid);
    assert.equal(initial.state,null);assert.equal(observed.realm,initial.realm);
    if(value.kind.startsWith('standard-close-'))assertStandardCloseState(observed.state,value.kind,value.challenge,value.stderr);
    else if(value.kind.startsWith('body-'))assertBodyState(observed.state,value.kind,value.challenge);
    else if(value.kind.startsWith('validation-'))assert.deepEqual(observed.state,expectedValidationState(value.kind,value.challenge),'independent parameter conversion, identity, release and byte-preservation observations');
    else assert.deepEqual(observed.state,expectedState(value.kind,value.challenge,linkedEOFDiagnostic(value)),'literal view/minimum/EOF/cancellation result');
    const nonces=new Set();wire(value,nonces);upstream(value,nonces);
    fields(value.shutdown,['code','signal','forced'],'normal service shutdown');assert.equal(value.shutdown.forced,false);
    if(mode==='reference')assert.ok(value.shutdown.code===0&&value.shutdown.signal===null||value.shutdown.code===null&&value.shutdown.signal==='SIGTERM');
    else assert.deepEqual(value.shutdown,{code:0,signal:null,forced:false});
    return {kind:value.kind};
}
export function validateFetchBYOBReport(report,expected){
    const shard=expected.shard??null,cases=shard===null?fetchBYOBCases:fetchBYOBShardCases[shard];
    assert.ok(cases,'known fixed BYOB shard');
    fields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','certificateInputs','cases',...(shard===null?[]:['shard'])],'BYOB report');
    if(shard!==null)assert.equal(report.shard,shard);
    assert.equal(report.schema,'capsid-fetch-byob-v1');assert.equal(report.complete,true);assert.equal(report.runId,expected.runId);
    assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock,expected.lock);assert.deepEqual(report.inputs,expected.inputs);
    assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(fetchBYOBInputPaths).sort());for(const hash of Object.values(report.inputs))assert.match(hash,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,worker:report.mode==='reference'?null:expected.worker});
    assertFetchTransitionCertificates(report.certificateInputs);assert.deepEqual(report.cases.map(value=>value.kind),cases);
    assert.equal(new Set(report.cases.map(value=>value.challenge)).size,cases.length);
    assert.equal(new Set(report.cases.map(value=>JSON.parse(value.initial.text).realm)).size,cases.length);
    uniqueEOFDiagnostics(report.cases);
    return report.cases.map(value=>assertFetchBYOBCase(value,report.mode));
}
export function compareFetchBYOBReports(reference,candidates,expected){
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(value=>value.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateFetchBYOBReport(reference,expected);for(const candidate of candidates)assert.deepEqual(validateFetchBYOBReport(candidate,expected),baseline);
    return {complete:true,casesPerBackend:fetchBYOBCases.length,backends:candidates.length};
}

function validateShards(reports,mode,expected){
    assert.deepEqual(reports.map(report=>report.shard),fetchBYOBShardNames,'every fixed shard executed exactly once');
    for(const report of reports){assert.equal(report.mode,mode);validateFetchBYOBReport(report,{...expected,shard:report.shard});}
    const cases=reports.flatMap(report=>report.cases);assert.deepEqual(cases.map(value=>value.kind),fetchBYOBCases);
    assert.equal(new Set(cases.map(value=>value.challenge)).size,fetchBYOBCases.length,'fresh challenges across all shards');
    assert.equal(new Set(cases.map(value=>JSON.parse(value.initial.text).realm)).size,fetchBYOBCases.length,'fresh VMs across all shards');
    uniqueEOFDiagnostics(cases);
    return cases.map(value=>({kind:value.kind}));
}
export function compareFetchBYOBShardReports(reference,candidates,expected){
    const baseline=validateShards(reference,'reference',expected);
    assert.deepEqual(candidates.map(group=>group[0]?.mode).sort(),referenceBackendModes(expected.lock).sort());
    for(const group of candidates)assert.deepEqual(validateShards(group,group[0].mode,expected),baseline);
    return {complete:true,casesPerBackend:fetchBYOBCases.length,backends:candidates.length,shardsPerBackend:fetchBYOBShardNames.length};
}
