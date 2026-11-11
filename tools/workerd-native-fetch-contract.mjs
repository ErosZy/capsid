import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {gunzipSync} from 'node:zlib';

const uuid=/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
export function exactFields(value,names,label){
    assert.ok(value&&typeof value==='object'&&!Array.isArray(value),label);
    assert.deepEqual(Object.keys(value).sort(),[...names].sort(),label);
}
function bytes(value){
    if(typeof value==='string'){
        const data=Buffer.from(value,'base64');assert.equal(data.toString('base64'),value);return data;
    }
    exactFields(value,['encoding','rawBytes','sha256','payload'],'complete lossless transcript');
    assert.equal(value.encoding,'gzip-base64');
    assert.ok(Number.isInteger(value.rawBytes)&&value.rawBytes>0&&value.rawBytes<=1048613);
    const raw=gunzipSync(bytes(value.payload),{maxOutputLength:1048613});
    assert.equal(raw.length,value.rawBytes);assert.equal(createHash('sha256').update(raw).digest('hex'),value.sha256);return raw;
}
function port(value){assert.ok(Number.isInteger(value)&&value>0&&value<65536);}
function nonce(value,seen){assert.match(value,/^[A-Za-z0-9+/]{22}==$/);assert.ok(!seen.has(value),'fresh actual handshake nonce');seen.add(value);}
function wire(value,nonces){
    let key;
    for(const [field,final] of [['headers',false],['wire',true]]){
        const observed=value[field];
        exactFields(observed,['status','rawHeader','requestKey','rawTail','frames','peerTerminated','transportError','completion','closeReplySent'],'actual downstream wire');
        assert.equal(observed.status,101);
        if(!final){key=observed.requestKey;nonce(key,nonces);}else assert.equal(observed.requestKey,key);
        const accept=createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        const lines=observed.rawHeader.split('\r\n');assert.equal(lines.shift(),'HTTP/1.1 101 Switching Protocols');
        assert.deepEqual(lines.map(line=>{const at=line.indexOf(':');assert.ok(at>0);return [line.slice(0,at).toLowerCase(),line.slice(at+1).trim()];}).sort(),
            [['connection','Upgrade'],['upgrade','websocket'],['sec-websocket-accept',accept]].sort());
        if(!final){
            assert.deepEqual(observed.frames,[]);assert.equal(observed.rawTail,'');assert.equal(observed.peerTerminated,false);
            assert.equal(observed.transportError,null);assert.equal(observed.completion,null);assert.equal(observed.closeReplySent,false);continue;
        }
        const frames=[{opcode:1,payload:'c3RhcnRlZA=='},{opcode:1,payload:'ZG9uZQ=='},{opcode:8,payload:'A+hkb25l'}];
        assert.deepEqual(observed.frames,frames);
        assert.ok(bytes(observed.rawTail).equals(Buffer.concat(frames.map(frame=>{
            const payload=bytes(frame.payload);return Buffer.concat([Buffer.from([128|frame.opcode,payload.length]),payload]);}))));
        assert.equal(observed.peerTerminated,true);assert.ok(['peer-ended','peer-reset'].includes(observed.completion));
        assert.equal(observed.transportError,observed.completion==='peer-reset'?'ECONNRESET':null);assert.equal(observed.closeReplySent,true);
    }
}
export function assertNativeFetchEnvelope(value,{mode,requests,body,nonces=new Set()}){
    exactFields(value,['kind','challenge','target','sourcePort','allowedAuthorities','initial','headers','wire','observed',
        'allSourcesEndedBeforeStop','upstreamBeforeStop','shutdown','stderr','upstreamAfterStop','failure','cleanupFailure'],'complete native fetch case');
    assert.match(value.challenge,uuid);port(value.sourcePort);
    assert.equal(value.target,`https://localhost:${value.sourcePort}/probe?challenge=${value.challenge}`);
    assert.deepEqual(value.allowedAuthorities,['localhost:'+value.sourcePort]);assert.equal(value.failure,null);assert.equal(value.cleanupFailure,null);
    assert.equal(typeof value.stderr,'string');
    assert.doesNotMatch(value.stderr,/AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|invalid IPC frame|worker_crash|TOKEN LEAK|POISON/);
    for(const name of ['initial','observed']){exactFields(value[name],['status','text'],'same VM observation');assert.equal(value[name].status,200);}
    const initial=JSON.parse(value.initial.text),observed=JSON.parse(value.observed.text);
    exactFields(initial,['realm','state'],'initial document');exactFields(observed,['realm','state'],'observed document');
    assert.match(initial.realm,uuid);assert.equal(initial.state,null);assert.equal(observed.realm,initial.realm);
    wire(value,nonces);
    const snapshot=value.upstreamBeforeStop;exactFields(snapshot,['records','tcpRecords','sni','tlsErrors'],'actual upstream snapshot');
    assert.deepEqual(value.upstreamAfterStop,snapshot,'teardown cannot supply completion');assert.equal(value.allSourcesEndedBeforeStop,true);
    assert.deepEqual(snapshot.tlsErrors,[]);assert.deepEqual(snapshot.sni,Array.from({length:requests},()=>({role:'source',servername:'localhost'})));
    assert.equal(snapshot.records.length,requests);assert.equal(snapshot.tcpRecords.length,requests);
    // Separate accept events and fresh handshake nonces identify connections.
    // A closed TCP connection may release its tuple for a later connection.
    for(let index=0;index<requests;index++){
        const tcp=snapshot.tcpRecords[index],row=snapshot.records[index];
        exactFields(tcp,['role','localAddress','localPort','remoteAddress','remotePort','ended','closed','error'],'real TCP connection');
        assert.equal(tcp.role,'source');assert.ok(['127.0.0.1','::1'].includes(tcp.localAddress));assert.equal(tcp.remoteAddress,tcp.localAddress);
        assert.equal(tcp.localPort,value.sourcePort);port(tcp.remotePort);assert.equal(tcp.closed,true);assert.equal(typeof tcp.ended,'boolean');assert.equal(tcp.error,null);
        exactFields(row,['role','localAddress','localPort','remoteAddress','remotePort','servername','request','rawRequest','responseHeader','responseStatus',
            'frames','sentFrames','receivedChunks','sentChunks','events','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','error'],'complete HTTP source record');
        for(const key of ['role','localAddress','localPort','remoteAddress','remotePort'])assert.equal(row[key],tcp[key]);
        assert.equal(row.servername,'localhost');assert.equal(row.error,null);assert.equal(row.fixtureDestroyed,false);assert.equal(row.fixtureEnded,true);
        assert.ok(row.peerEnded||row.peerReset);assert.equal(typeof row.peerEnded,'boolean');assert.equal(typeof row.peerReset,'boolean');
        assert.deepEqual(row.frames,[]);assert.deepEqual(row.sentFrames,[]);
        const request=Buffer.concat(row.receivedChunks.map(bytes));assert.ok(request.equals(bytes(row.rawRequest)));
        assert.equal(request.toString('latin1'),row.request+'\r\n\r\n');
        const lines=row.request.split('\r\n');assert.equal(lines.shift(),`GET /probe?challenge=${value.challenge} HTTP/1.1`);
        const headers=lines.map(line=>{const at=line.indexOf(':');assert.ok(at>0);return [line.slice(0,at).toLowerCase(),line.slice(at+1).trim()];});
        const keys=headers.filter(([name])=>name==='sec-websocket-key');assert.equal(keys.length,1);nonce(keys[0][1],nonces);
        assert.deepEqual(headers.sort(),[['connection','Upgrade'],['host','localhost:'+value.sourcePort],['sec-websocket-key',keys[0][1]],
            ['sec-websocket-version','13'],['upgrade','websocket'],['x-probe',value.challenge]].sort());
        const header=`HTTP/1.1 200 OK\r\nContent-Length: ${body.length}\r\nX-Probe: ${value.challenge}\r\nConnection: close`;
        assert.equal(row.responseHeader,header);assert.equal(row.responseStatus,200);
        assert.ok(Buffer.concat(row.sentChunks.map(bytes)).equals(Buffer.concat([Buffer.from(header+'\r\n\r\n'),body])));
        assert.deepEqual(row.events,row.peerEnded?['request','response','peer-ended']:['request','response']);
    }
    exactFields(value.shutdown,['code','signal','forced'],'normal service shutdown');assert.equal(value.shutdown.forced,false);
    if(mode==='reference')assert.ok(value.shutdown.code===0&&value.shutdown.signal===null||value.shutdown.code===null&&value.shutdown.signal==='SIGTERM');
    else assert.deepEqual(value.shutdown,{code:0,signal:null,forced:false});
    return {realm:initial.realm,state:observed.state};
}
