import strictAssert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid,stopCapsid,withCleanup} from './helpers/ws_host_backend.mjs';

const args=process.argv.slice(2);
function option(name,fallback) {const i=args.indexOf(name);return i<0?fallback:args[i+1]}
const host=option('--host'),worker=option('--worker'),mode=option('--mode','single-worker');
let assertions=0;
const assert=new Proxy(strictAssert,{get(target,key) {const value=target[key];return typeof value==='function'?(...args)=>{assertions++;return value(...args)}:value}});
assert.ok(host && worker,'host and worker paths are required');
const lock=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-ws/reference-lock.json',import.meta.url)));
assert.equal(process.version,lock.client.node_version,'independent client version is pinned');
const root=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-budget-'));
const sockets=new Set(),cases=[],shutdowns=[];
const peers=[];
const bundle=path.join(root,'bundle.mjs');
fs.writeFileSync(bundle,`
const connections=[];
export default {fetch(request) {
  const url=new URL(request.url);
  if(url.pathname.endsWith('/dial')) {
    const events=[];let output;
    const body=new ReadableStream({start(controller){
      output=controller;controller.enqueue(new TextEncoder().encode('constructed\\n'));
    }});
    const record=event=>{
      events.push(event);
      if(output && (event.type==='open' || event.type==='close')) {
        output.enqueue(new TextEncoder().encode(JSON.stringify(events)));output.close();output=null;
      }
    };
    const ws=new WebSocket(url.searchParams.get('target'));connections.push(ws);
    ws.onopen=()=>record({type:'open'});
    ws.onerror=()=>record({type:'error'});
    ws.onclose=e=>record({type:'close',code:e.code});
    // The HTTP body owns an unfinished constructor. Keep it live until an
    // actual open or close callback supplies the observation to the client.
    return new Response(body);
  }
  if(url.pathname.endsWith('/in') || url.pathname.endsWith('/burst')) {
    const pair=new WebSocketPair();const server=pair[1];server.accept();
    server.onmessage=e=>server.send(url.pathname.endsWith('/burst')?new Uint8Array(8192):e.data);
    return new Response(null,{status:101,webSocket:pair[0]});
  }
  return new Response('healthy');
}};
`);
function track(socket) {sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});return socket}
async function until(read,condition,label) {
    const deadline=Date.now()+5000;let value;
    do {value=await read();if(condition(value))return value;await delay(20)} while(Date.now()<deadline);
    assert.fail(`${label}: ${JSON.stringify(value)}`);
}
async function within(promise,label) {
    let timer;
    try {return await Promise.race([promise,new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error(label)),5000)})])}
    finally {clearTimeout(timer)}
}
async function makePeer() {
    const current={accepted:0,stall:false,connections:new Set()};
    current.server=net.createServer(socket=>{
        track(socket);current.connections.add(socket);current.accepted++;
        socket.on('close',()=>current.connections.delete(socket));
        let input=Buffer.alloc(0),upgraded=false;
        socket.on('data',chunk=>{
            input=Buffer.concat([input,chunk]);
            if(upgraded) return;
            const end=input.indexOf('\r\n\r\n');if(end<0)return;
            const key=/Sec-WebSocket-Key: ([^\r\n]+)/i.exec(input.subarray(0,end).toString())?.[1];
            assert.ok(key,'actual upstream receives valid handshake');
            upgraded=true;
            if(!current.stall) {
                const accept=crypto.createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
                socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
            }
        });
    });
    await new Promise((resolve,reject)=>{current.server.once('error',reject);current.server.listen(0,'127.0.0.1',resolve)});
    current.port=current.server.address().port;
    current.target=`ws://127.0.0.1:${current.port}/hold`;
    current.destroy=()=>{for(const socket of current.connections)socket.destroy()};
    peers.push(current);return current;
}
function request(backend,target='/hello') {
    return new Promise((resolve,reject)=>{
        const request=http.get({host:'127.0.0.1',port:backend.port,path:'/@capsid/orders'+target,agent:backend.agent},response=>{
            const parts=[];response.on('data',part=>parts.push(part));response.on('end',()=>resolve({status:response.statusCode,text:Buffer.concat(parts).toString()}));
        });
        request.setTimeout(5000,()=>request.destroy(new Error('HTTP deadline')));request.once('error',reject);
    });
}
async function beginDial(backend,peer) {
    let ready,failed;
    const constructed=new Promise((resolve,reject)=>{ready=resolve;failed=reject});
    const complete=new Promise((resolve,reject)=>{
        // A held response must not occupy the one keep-alive diagnostics
        // connection, including when static-pool dispatches to another worker.
        const current=http.get({host:'127.0.0.1',port:backend.port,
            path:'/@capsid/orders/dial?'+new URLSearchParams({target:peer.target}),agent:false},response=>{
            let body='';
            response.setEncoding('utf8');
            response.on('data',chunk=>{
                body+=chunk;
                if(body.includes('\n')) {
                    try {assert.equal(response.statusCode,200);assert.equal(body.split('\n',1)[0],'constructed');ready()}
                    catch(error){failed(error);reject(error)}
                }
            });
            response.on('end',()=>{
                try {
                    assert.equal(response.statusCode,200);assert.ok(body.startsWith('constructed\n'));
                    resolve(JSON.parse(body.slice('constructed\n'.length)));
                } catch(error){failed(error);reject(error)}
            });
            response.on('error',error=>{failed(error);reject(error)});
        });
        current.setTimeout(5000,()=>current.destroy(new Error('outbound HTTP body deadline')));
        current.on('error',error=>{failed(error);reject(error)});
    });
    // A peer can fail before the caller reaches await complete. Keep its
    // rejection observed while still returning the original rejecting promise.
    complete.catch(()=>{});
    await constructed;
    return {complete};
}
async function dial(backend,peer) {
    return (await beginDial(backend,peer)).complete;
}
function upgrade(backend,route='/in') {
    return new Promise((resolve,reject)=>{
        const socket=track(net.connect(backend.port,'127.0.0.1'));
        const closed=new Promise(resolve=>socket.once('close',resolve));
        let input=Buffer.alloc(0);const timer=setTimeout(()=>{socket.destroy();reject(new Error('upgrade deadline'))},5000);
        socket.once('error',reject);
        socket.once('connect',()=>socket.write(`GET /@capsid/orders${route} HTTP/1.1\r\nHost: public.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\n\r\n`));
        socket.on('data',chunk=>{
            input=Buffer.concat([input,chunk]);const end=input.indexOf('\r\n\r\n');if(end<0)return;
            clearTimeout(timer);socket.removeAllListeners('data');
            const status=Number(/^HTTP\/1.1 (\d+)/.exec(input.toString())?.[1]);
            resolve({socket,closed,status,close:async()=>{
                if(!socket.destroyed) {
                    // Masked RFC 6455 close (1000); exercises the normal return path.
                    const mask=crypto.randomBytes(4),payload=Buffer.from([3,232]);
                    payload[0]^=mask[0];payload[1]^=mask[1];socket.write(Buffer.concat([Buffer.from([0x88,0x82]),mask,payload]));
                }
                await within(closed,'normal inbound close deadline');
            }});
        });
    });
}
function triggerBurst(socket) {
    const mask=crypto.randomBytes(4);
    socket.write(Buffer.concat([Buffer.from([0x81,0x81]),mask,Buffer.from([0x67^mask[0]])]));
}
function promValue(text,name,scope,resource) {
    const lines=text.split('\n').filter(line=>line.startsWith(`capsid_${name}{`) && line.includes(`scope="${scope}"`) && (!resource || line.includes(`resource="${resource}"`)));
    assert.equal(lines.length,1,`one live ${scope}/${name}/${resource||''} series`);
    return Number(lines[0].slice(lines[0].lastIndexOf(' ')+1));
}
async function snapshot(backend) {
    if(mode!=='managed') {
        // A healthy request pumps first-party diagnostics after asynchronous cleanup.
        const health=await request(backend);assert.equal(health.status,200);assert.equal(health.text,'healthy');await delay(10);
        const line=backend.stderr().split('\n').findLast(line=>line.startsWith('capsid-host: channel-metrics '));
        assert.ok(line,'opt-in host budget diagnostics exist');return JSON.parse(line.slice('capsid-host: channel-metrics '.length));
    }
    const response=await backend.admin('/metrics');assert.equal(response.status,200);
    const result={};
    for(const scope of ['service','listener']) {
        const view={active:promValue(response.text,'channel_active',scope),httpInflight:promValue(response.text,'channel_http_inflight',scope)};
        for(const [field,resource,gauge] of [['channels','channels','channel_reservations'],['dials','dials','channel_pending_dials'],['queueBytes','queue_bytes','channel_queue_bytes']]) {
            view[field]={used:promValue(response.text,gauge,scope),limit:promValue(response.text,'channel_budget_limit',scope,resource),highWater:promValue(response.text,'channel_budget_high_water',scope,resource),rejected:promValue(response.text,'channel_budget_rejections_total',scope,resource)};
        }
        result[scope]=view;
    }
    return result;
}
async function idle(backend) {
    return until(()=>snapshot(backend),value=>['service','listener'].every(scope=>!value[scope].channels.used && !value[scope].dials.used && !value[scope].queueBytes.used && !value[scope].active && !value[scope].httpInflight),'all reservations returned');
}
async function run(name,options,action) {
    const dir=path.join(root,name);fs.mkdirSync(dir);const peer=await makePeer();let backend;
    await withCleanup(async () => {
        const allow=[`127.0.0.1:${peer.port}`];
        backend=await startCapsid(host,worker,bundle,dir,mode,{workers:mode==='static-pool'?3:1,
            permissions:{modules:[],fetch:{allow}},fetchTargets:allow,env:{CAPSID_HOST_IPC_METRICS:'1'},...options});
        backend.agent=new http.Agent({keepAlive:true,maxSockets:1});
        const detail=await action(backend,peer);cases.push({name,...detail});
    }, async () => {
        await withCleanup(async () => {
        if(backend) {
            backend.agent?.destroy();
            const shutdown=await stopCapsid(backend.child);shutdowns.push({name,...shutdown});
            assert.equal(shutdown.forced,false,`bounded shutdown: ${backend.stderr()}`);
            assert.equal(shutdown.code,0,`clean shutdown: ${backend.stderr()}`);
            assert.equal(shutdown.signal,null,`signal-free shutdown: ${backend.stderr()}`);
            assert.doesNotMatch(backend.stderr(),/ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:/);
        }
        }, async () => {peer.destroy();await new Promise(resolve=>peer.server.close(resolve));});
    });
}
try {
    await run('service-channels',{channelCapacity:{channelsTotal:1},listenerChannels:4},async(backend,peer)=>{
        const accepted=await upgrade(backend);assert.equal(accepted.status,101);
        for(let i=0;i<8;i++) {const denied=await upgrade(backend);assert.equal(denied.status,429);denied.socket.destroy()}
        const before=peer.accepted;const denied=await dial(backend,peer);
        assert.deepEqual(denied,[{type:'error'},{type:'close',code:1006}]);assert.equal(peer.accepted,before,'service refusal occurs before TCP');
        const full=await until(()=>snapshot(backend),value=>value.service.active===1 && value.service.channels.used===1 && !value.service.httpInflight,'101 owns channel after HTTP terminal');
        assert.equal(full.service.channels.limit,1);assert.equal(full.service.channels.highWater,1);assert.ok(full.service.channels.rejected>=9);
        await accepted.close();await idle(backend);
        assert.deepEqual(await dial(backend,peer),[{type:'open'}],'returned channel reused by outbound');
        assert.equal(peer.accepted,before+1);peer.destroy();return {full,empty:await idle(backend)};
    });
    await run('listener-channels',{channelCapacity:{channelsTotal:4},listenerChannels:1},async(backend,peer)=>{
        const accepted=await upgrade(backend);assert.equal(accepted.status,101);
        const denied=await upgrade(backend);assert.equal(denied.status,429);denied.socket.destroy();
        assert.deepEqual(await dial(backend,peer),[{type:'open'}],'outbound uses service scope');
        const full=await until(()=>snapshot(backend),value=>value.service.active===2 && value.listener.active===1 && !value.service.httpInflight,'listener active is distinct from standalone outbound');
        assert.equal(full.listener.channels.limit,1);assert.equal(full.listener.channels.rejected,1);assert.equal(full.service.channels.rejected,0);
        peer.destroy();await accepted.close();return {full,empty:await idle(backend)};
    });
    await run('pending-dials',{channelCapacity:{channelsTotal:4,pendingDialsTotal:1}},async(backend,peer)=>{
        peer.stall=true;const pending=await beginDial(backend,peer);
        await until(()=>peer.accepted,count=>count===1,'first real handshake is pending');
        const denied=await dial(backend,peer);
        assert.deepEqual(denied,[{type:'error'},{type:'close',code:1006}]);assert.equal(peer.accepted,1,'dial budget rejects before TCP');
        const full=await until(()=>snapshot(backend),value=>value.service.dials.used===1 && value.service.httpInflight===1,'unfinished HTTP body owns one pending constructor');
        assert.equal(full.service.dials.limit,1);assert.equal(full.service.dials.highWater,1);assert.equal(full.service.dials.rejected,1);
        peer.destroy();assert.deepEqual(await pending.complete,[{type:'error'},{type:'close',code:1006}],'held dial failure settles');await idle(backend);
        peer.stall=false;assert.deepEqual(await dial(backend,peer),[{type:'open'}],'returned dial slot reused');
        assert.equal(peer.accepted,2);peer.destroy();return {full,empty:await idle(backend)};
    });
    for(const scope of ['service','listener']) {
        await run(`${scope}-queue`,{channelCapacity:{channelQueueBytesTotal:scope==='service'?'1KiB':'64KiB'},listenerChannelQueueBytes:scope==='listener'?'1KiB':'16KiB'},async backend=>{
            const burst=await upgrade(backend,'/burst');assert.equal(burst.status,101);
            triggerBurst(burst.socket);
            await within(burst.closed,'oversized network buffer must terminate channel');
            const empty=await idle(backend);assert.ok(empty[scope].queueBytes.rejected>=1,'network queue refusal recorded');
            assert.ok(empty[scope].queueBytes.highWater<=1024,'network write high-water remains bounded');
            if(scope==='listener')assert.equal(empty.service.queueBytes.rejected,0,'local refusal never charges ancestor');
            return {empty};
        });
    }
} finally {
    for(const socket of sockets)socket.destroy();
    for(const peer of peers)if(peer.server.listening)await new Promise(resolve=>peer.server.close(resolve));
    fs.rmSync(root,{recursive:true,force:true});
}
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const report={complete:true,mode,workers:mode==='static-pool'?3:1,scenarioTypes:cases.length,assertions,cases,shutdowns,
    client:'independent raw Node TCP/HTTP',node:process.version,hostSHA256:hash(host),workerSHA256:hash(worker),
    sourceSHA256:hash(new URL(import.meta.url)),helperSHA256:hash(new URL('./helpers/ws_host_backend.mjs',import.meta.url))};
if(option('--report'))fs.writeFileSync(option('--report'),JSON.stringify(report,null,2));
console.log(JSON.stringify(report));
