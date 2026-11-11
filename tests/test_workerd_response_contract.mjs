import assert from 'node:assert/strict';
import {test} from 'node:test';
import fs from 'node:fs';
import net from 'node:net';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {assertResponseObservation} from '../tools/workerd-response-contract.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';

const sample=()=>JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-response-report.json',import.meta.url))).observation;
test('real locked-reference Response observations compare without modifying raw evidence',()=>{
    const observed=sample(),before=structuredClone(observed);
    const normalized=assertResponseObservation(observed);
    assert.deepEqual(observed,before);
    assert.equal(normalized.aborted.length,18);
    assert.equal(normalized.controls.length,2);
});

test('only peer EOF/reset metadata is normalized after complete wire validation',()=>{
    const original=sample(),changed=structuredClone(original);
    Object.assign(changed.aborted[0].wire,{completion:'peer-reset',transportError:'ECONNRESET'});
    const before=structuredClone(changed);
    assert.deepEqual(assertResponseObservation(changed),assertResponseObservation(original));
    assert.deepEqual(changed,before);
});

for(const [name,mutate] of [
    ['missing body conversion',r=>{delete r.constructors['body-empty'];}],
    ['old status error class',r=>{r.constructors['wrong-status'].error='TypeError';}],
    ['wrong constructor diagnostic',r=>{r.constructors.missing.message='other';}],
    ['empty fixed body rejected',r=>{r.constructors['body-bytes-empty']={error:'TypeError'};}],
    ['spoofed nonempty body admitted',r=>{r.constructors['body-buffer-spoof-empty']=r.constructors['body-buffer-empty'];}],
    ['empty stream admitted',r=>{r.constructors['body-stream-empty']=r.constructors.unaccepted;}],
    ['zero-length view uses its entire buffer',r=>{r.constructors['body-view-empty-offset']={error:'TypeError'};}],
    ['detached DataView rejected',r=>{r.constructors['body-detached-dataview']={error:'TypeError'};}],
    ['empty body keeps a stream',r=>{r.constructors['body-empty'].body=false;}],
    ['implicit content type on an empty body',r=>{r.constructors['body-blob-empty-type'].contentType='text/x-fixture';}],
    ['caller content type discarded',r=>{r.constructors['body-empty-explicit-type'].contentType=null;}],
    ['body already consumed',r=>{r.constructors['body-empty'].bodyUsed=true;}],
    ['clone accepted',r=>{r.constructors.clone={accepted:true};}],
    ['missing simultaneous quota control',r=>{r.controls.pop();}],
    ['serial quota control',r=>{r.controls[1].simultaneous=false;}],
    ['one live quota slot',r=>{r.controls[1].connections.pop();}],
    ['missing failed coupling invocation',r=>{r.aborted.pop();}],
    ['duplicate failed coupling invocation',r=>{r.aborted[1]=structuredClone(r.aborted[0]);}],
    ['HTTP 500 before coupling',r=>{r.aborted[0].wire.status=500;}],
    ['101 with a wrong nonce',r=>{r.aborted[0].wire.requestKey='A'.repeat(22)+'==';}],
    ['missing custom header',r=>{r.aborted[0].wire.rawHeader=r.aborted[0].wire.rawHeader.replace(/\r\nx-response-fixture: yes/i,'');}],
    ['duplicated handshake header',r=>{r.aborted[0].wire.rawHeader+='\r\nUpgrade: websocket';}],
    ['unknown selected protocol',r=>{r.aborted[0].wire.rawHeader=r.aborted[0].wire.rawHeader.replace(/Sec-WebSocket-Protocol: chat/i,'Sec-WebSocket-Protocol: other');}],
    ['HTTP body framing after 101',r=>{r.aborted[0].wire.rawHeader+='\r\nContent-Length: 0';}],
    ['unclosed peer',r=>{r.aborted[0].wire.peerTerminated=false;}],
    ['client disposal treated as peer EOF',r=>{r.aborted[0].wire.completion='client-disposed';}],
    ['unknown peer error',r=>{r.aborted[0].wire.transportError='ETIMEDOUT';}],
    ['wire Close replaces coupling abort',r=>{r.aborted[0].wire.frames.push({opcode:8,payload:'A+g='});}],
    ['HTTP error appended after 101',r=>{r.aborted[0].wire.rawTail=Buffer.from('HTTP/1.1 500 Error\r\n\r\n').toString('base64');}],
    ['late 101 after accept',r=>{r.delayed.beforeWire.status=null;}],
    ['callback before peer accept',r=>{r.delayed.before.events.push({type:'message',data:'early'});}],
    ['peer already accepted before barrier',r=>{r.delayed.before.accepted=true;}],
    ['missing delayed message',r=>{r.delayed.after.events.shift();}],
    ['missing pre-accept Close observation',r=>{delete r.delayedClose;}],
    ['early Close drops preceding DATA',r=>{r.delayedClose.after.events.shift();}],
    ['clean pre-accept Close becomes abort',r=>{r.delayedClose.wire.frames=[];r.delayedClose.wire.rawTail='';}],
    ['Close dispatches before peer accept',r=>{r.delayedClose.before.events.push({type:'close',code:1000,clean:true});}],
    ['corrupted echoed bytes',r=>{r.controls[1].connections[0].frames[0].payload='eA==';}],
    ['unparsed trailing wire bytes',r=>{r.controls[1].connections[0].rawTail+='AA==';}],
    ['incomplete cross-request HTTP failure',r=>{r.crossContext.bodyComplete=false;}],
    ['cross-request endpoint upgrades',r=>{r.crossContext.status=101;}],
    ['missing outbound-origin response',r=>{r.outbound.pop();}],
    ['unstopped fixture upstream',r=>{r.outbound[0].upstreamStopped=false;}],
    ['unhealthy worker after coupling failure',r=>{r.healthy.status=500;}],
]) test(`Response contract rejects ${name}`,()=>{
    const observed=sample();mutate(observed);
    assert.throws(()=>assertResponseObservation(observed));
});

async function withPeer(action,run) {
    const sockets=new Set();let current;
    const server=net.createServer(socket=>{
        current=socket;sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});
        let input='';socket.on('data',chunk=>{
            input+=chunk.toString();if(!input.includes('\r\n\r\n'))return;
            socket.removeAllListeners('data');action(socket,input);
        });
    });
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    try {await run(server.address().port,()=>current);}
    finally {for(const socket of sockets)socket.destroy();await new Promise(resolve=>server.close(resolve));}
}
function switching(request) {
    const key=/sec-websocket-key: ([^\r\n]+)/i.exec(request)[1];
    const accept=createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    return 'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n';
}

test('real observer does not turn its own HTTP-error disposal into peer closure',async()=>{
    await withPeer(socket=>socket.write('HTTP/1.1 500 Error\r\nContent-Length: 1\r\n\r\nx'),async port=>{
        const client=responseSession(port,'/'),result=await client.complete;
        assert.equal(result.status,500);assert.equal(result.completion,'http-status');assert.equal(result.peerTerminated,false);
        await delay(10);
        assert.equal(result.peerTerminated,false);assert.equal(client.snapshot().peerTerminated,false);
    });
});
test('real observer preserves the failed observation when a 101 peer never closes',async()=>{
    await withPeer((socket,request)=>socket.write(switching(request)),async port=>{
        const client=responseSession(port,'/',{timeoutMs:150});await client.headers;
        let error;try {await client.complete}catch(caught){error=caught}
        assert.equal(error.message,'Response peer terminal deadline');
        assert.equal(error.observation.status,101);assert.equal(error.observation.peerTerminated,false);
        await delay(10);
        assert.equal(error.observation.peerTerminated,false);assert.equal(client.snapshot().peerTerminated,false);
    });
});
test('real observer requires complete 101 headers before peer EOF can succeed',async()=>{
    await withPeer(socket=>socket.end('HTTP/1.1 101 Switching'),async port=>{
        const client=responseSession(port,'/');
        await assert.rejects(client.complete,/before complete Response headers/);
    });
});
test('real observer records peer EOF with no Close frame',async()=>{
    await withPeer((socket,request)=>socket.end(switching(request)),async port=>{
        const result=await responseSession(port,'/').complete;
        assert.equal(result.status,101);assert.equal(result.peerTerminated,true);
        assert.equal(result.completion,'peer-ended');assert.deepEqual(result.frames,[]);
    });
});
test('real observer records reset after an actual complete handshake',async()=>{
    await withPeer((socket,request)=>socket.write(switching(request)),async(port,socket)=>{
        const client=responseSession(port,'/');await client.headers;socket().resetAndDestroy();
        const result=await client.complete;
        assert.equal(result.status,101);assert.equal(result.peerTerminated,true);
        assert.equal(result.completion,'peer-reset');assert.equal(result.transportError,'ECONNRESET');
    });
});
test('real observer rejects invalid frame encoding after otherwise valid 101 headers',async()=>{
    await withPeer((socket,request)=>socket.write(Buffer.concat([Buffer.from(switching(request)),Buffer.from([0x81,0x81,0,0,0,0,1])])),async port=>{
        const client=responseSession(port,'/');
        await assert.rejects(client.complete,/unexpected frame encoding/);
        assert.equal(client.snapshot().peerTerminated,false);
    });
});
