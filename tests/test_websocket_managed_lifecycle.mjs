import strictAssert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid,stopCapsid} from './helpers/ws_host_backend.mjs';

const args=process.argv.slice(2);
function option(name,fallback) {const i=args.indexOf(name);return i<0?fallback:args[i+1]}
const host=option('--host'),worker=option('--worker'),scenario=option('--case','deploy');
// This footprint regression tests whether the last factory allocation remains
// live, not glibc's adaptive arena retention. Keep large allocations mapped so
// freeing them is observable in RSS. The ordinary churn scenarios retain the
// default allocator; exact pool/factory lifetime is also tested in C++.
const allocatorEnvironment=scenario==='reclamation-idle'
    ? {MALLOC_MMAP_THRESHOLD_:'65536',MALLOC_TRIM_THRESHOLD_:'65536'} : {};
let assertions=0;
const assert=new Proxy(strictAssert,{get(target,key) {const value=target[key];return typeof value==='function'?(...args)=>{assertions++;return value(...args)}:value}});
assert.ok(host && worker);assert.ok(['deploy','retire-deadline','replacement','replacement-recursion','deploy-persist-failure','reclamation-idle'].includes(scenario));
assert.equal(process.version,JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-ws/reference-lock.json',import.meta.url))).client.node_version);
// Keep the original RSS oracle as the default. Explicit instrumented profiles
// records those same predicates and finishes resource/exit/LSan assertions;
// production RSS acceptance remains a separate mandatory release gate.
const memoryAcceptance=option('--memory-acceptance','production-rss');
assert.ok(['production-rss','asan-resources','tsan-resources'].includes(memoryAcceptance),'known memory acceptance profile');
const footprintChecks=[];
function assertFootprint(passed,message) {
    footprintChecks.push({passed:Boolean(passed),message});
    if(memoryAcceptance==='production-rss')assert.ok(passed,message);
}
let sanitizerRuntimeMarkers;
if(memoryAcceptance!=='production-rss') {
    assert.equal(process.platform,'linux','explicit instrumented resource acceptance is Linux-only');
    assert.ok(['replacement-recursion','reclamation-idle'].includes(scenario),'resource profile applies only to the two allocator footprint cases');
    assert.ok(option('--report'),'resource acceptance requires a durable report');
    if(memoryAcceptance==='asan-resources') {
    assert.deepEqual((process.env.ASAN_OPTIONS??'').split(':').sort(),
        ['detect_leaks=1','abort_on_error=1','strict_string_checks=1'].sort(),
        'resource acceptance requires complete LSan, fatal ASan and the default quarantine without suppressions');
    assert.equal(process.env.LSAN_OPTIONS??'','','resource acceptance keeps the default complete LSan options');
    } else {
        assert.deepEqual((process.env.TSAN_OPTIONS??'').split(':').sort(),
            ['halt_on_error=1','exitcode=66'].sort(),
            'TSan resource acceptance requires fatal race reports and unchanged runtime options');
    }
    const runtimeMarker=memoryAcceptance==='asan-resources'?'__asan_init':'__tsan_init';
    sanitizerRuntimeMarkers=Object.fromEntries([['host',host],['worker',worker]].map(([role,file])=>
        [role,fs.readFileSync(file).includes(Buffer.from(runtimeMarker))]));
    assert.ok(sanitizerRuntimeMarkers.host && sanitizerRuntimeMarkers.worker,memoryAcceptance==='asan-resources'?'resource profile requires ASan runtime markers in both actual executables':'resource profile requires TSan runtime markers in both actual executables');
}
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-managed-ws-'));
const sockets=new Set(),traces=[],observations=[];
let backend,peer,report,shutdown;
function track(socket) {sockets.add(socket);socket.on('error',()=>{});socket.on('close',()=>sockets.delete(socket));return socket}
async function until(read,condition,label,timeout=6000) {
    const deadline=Date.now()+timeout;let value;
    do {value=await read();if(condition(value))return value;await delay(20)} while(Date.now()<deadline);
    assert.fail(`${label}: ${JSON.stringify(value)}\n${backend?.stderr()}`);
}
async function within(promise,label,timeout=6000) {
    let timer;
    try {return await Promise.race([promise,new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error(label)),timeout)})])}
    finally {clearTimeout(timer)}
}
function frame(opcode,payload,masked=false) {
    payload=Buffer.from(payload);assert.ok(payload.length<65536);
    const header=Buffer.alloc(payload.length<126?2:4);header[0]=0x80|opcode;
    header[1]=(masked?128:0)|(payload.length<126?payload.length:126);
    if(payload.length>=126)header.writeUInt16BE(payload.length,2);
    if(!masked)return Buffer.concat([header,payload]);
    const mask=crypto.randomBytes(4),bytes=Buffer.from(payload);for(let i=0;i<bytes.length;i++)bytes[i]^=mask[i%4];
    return Buffer.concat([header,mask,bytes]);
}
function readFrames(buffer,masked,onFrame) {
    while(buffer.length>=2) {
        assert.equal(Boolean(buffer[1]&128),masked,'peer framing uses the correct mask role');
        let length=buffer[1]&127,offset=2;
        assert.notEqual(length,127,'fixture bounds frames to 64KiB');
        if(length===126) {if(buffer.length<4)break;length=buffer.readUInt16BE(2);offset=4}
        const mask=masked?buffer.subarray(offset,offset+4):null;if(masked)offset+=4;
        if(buffer.length<offset+length)break;
        const payload=Buffer.from(buffer.subarray(offset,offset+length));
        if(masked)for(let i=0;i<payload.length;i++)payload[i]^=mask[i%4];
        onFrame(buffer[0]&15,payload);buffer=buffer.subarray(offset+length);
    }
    return buffer;
}
async function makePeer() {
    const current={accepted:0,connections:new Set()};
    current.server=net.createServer(socket=>{
        track(socket);const entry={socket,route:'',upgraded:false,frames:[]};current.connections.add(entry);current.accepted++;
        socket.on('close',()=>current.connections.delete(entry));
        let input=Buffer.alloc(0),head=false;
        entry.upgrade=()=>{
            assert.ok(!entry.upgraded && entry.key);entry.upgraded=true;
            const accept=crypto.createHash('sha1').update(entry.key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
            socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
        };
        socket.on('data',chunk=>{
            input=Buffer.concat([input,chunk]);
            if(!head) {
                const end=input.indexOf('\r\n\r\n');if(end<0)return;
                const text=input.subarray(0,end).toString();entry.route=/^GET ([^ ]+)/.exec(text)?.[1];entry.key=/Sec-WebSocket-Key: ([^\r\n]+)/i.exec(text)?.[1];
                assert.ok(entry.route && entry.key);traces.push({type:'upstream-handshake',route:entry.route});
                head=true;input=input.subarray(end+4);if(entry.route!=='/held')entry.upgrade();
            }
            if(!entry.upgraded)return;
            input=readFrames(input,true,(opcode,payload)=>{
                if(opcode===1) {entry.frames.push(payload.toString());socket.write(frame(1,payload))}
                else if(opcode===8 && !entry.closing) {entry.closing=true;socket.write(frame(8,payload))}
                else if(opcode===8) {}
                else if(opcode===9)socket.write(frame(10,payload));
                else assert.fail(`unexpected upstream opcode ${opcode}`);
            });
        });
    });
    await new Promise((resolve,reject)=>{current.server.once('error',reject);current.server.listen(0,'127.0.0.1',resolve)});
    current.port=current.server.address().port;current.target=`ws://127.0.0.1:${current.port}/echo`;current.held=`ws://127.0.0.1:${current.port}/held`;
    current.closeWhere=predicate=>{for(const entry of current.connections)if(entry.upgraded && !entry.closing && predicate(entry)) {entry.closing=true;entry.socket.write(frame(8,Buffer.from([3,232])))}};
    return current;
}
function source(version) {
    return `const version=${JSON.stringify(version)},held=${JSON.stringify(peer.held)};
const connections=[],events=[];
export default {async fetch(request) {
  const url=new URL(request.url);
  if(url.pathname.endsWith('/ws')) {
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.onmessage=async e=>{
      if(e.data==='start-pending') {
        const pending=new WebSocket(held);connections.push(pending);
        pending.onerror=()=>{};server.send(version+':pending');return;
      }
      if(e.data==='deferred') {
        const pending=new WebSocket(held);connections.push(pending);
        await new Promise((resolve,reject)=>{pending.onopen=resolve;pending.onerror=reject});pending.close(1000);
      }
      server.send(version+':'+e.data);
    };
    return new Response(null,{status:101,webSocket:pair[0]});
  }
  if(url.pathname.endsWith('/out')) {
    const result=[];events.push(result);const socket=new WebSocket(url.searchParams.get('target'));connections.push(socket);
    let settle;
    const opened=new Promise(resolve=>settle=resolve);
    socket.onopen=()=>{
      result.push({type:'open'});
      try {socket.send(version+':open');settle(true)}
      catch(error) {result.push({type:'callback-error',message:String(error)});settle(false);throw error}
    };
    socket.onmessage=e=>result.push({type:'message',data:e.data});
    socket.onerror=()=>{result.push({type:'error'});settle(false)};
    socket.onclose=e=>result.push({type:'close',code:e.code});
    return Response.json({version,opened:await opened,events:result});
  }
  if(url.pathname.endsWith('/state'))return Response.json(events);
  if(url.pathname.endsWith('/slow-http')) {
    await new Promise(resolve=>setTimeout(resolve,40));return new Response(version+':slow');
  }
  return new Response(version);
}};`;
}
function paddedSource(version) {
    // review 12.2 shape: ~512 KiB per generation so a retained pool
    // is visible in the Host's memory footprint
    const filler='\n// retained-generation probe padding '+version+'\nexport const pad="'
        +'x'.repeat(512*1024)+'";\n';
    return source(version)+filler;
}
function threadCount() {
    return fs.readdirSync('/proc/'+backend.child.pid+'/task').length;
}
async function failedDeploy(version) {
    const submitted=await backend.admin('/v1/deploy',{app:'orders',version});
    assert.equal(submitted.status,202,submitted.text);
    const operation=await until(async()=>{
        const response=await backend.admin('/v1/operations/'+submitted.json.operationId);
        assert.equal(response.status,200);return response.json;
    },value=>['active','failed'].includes(value.state),'failed deploy settles',20000);
    assert.equal(operation.state,'failed','persist-blocked deploy must fail: '+JSON.stringify(operation));
    traces.push({type:'failed-deploy',version});
    return operation;
}
function hostStatus() {
    const status=fs.readFileSync('/proc/'+backend.child.pid+'/status','utf8');
    const rss=Number(/^VmRSS:\s+(\d+) kB$/m.exec(status)?.[1]);
    const vm=Number(/^VmSize:\s+(\d+) kB$/m.exec(status)?.[1]);
    assert.ok(rss>0 && vm>0,'host status parses');
    return {rss,vm};
}
function request(target='/version') {
    return new Promise((resolve,reject)=>{
        const req=http.get({host:'127.0.0.1',port:backend.port,path:'/@capsid/orders'+target,agent:backend.agent},response=>{
            traces.push({type:'http-head',target,status:response.statusCode,headers:response.headers});
            const chunks=[];response.on('data',chunk=>chunks.push(chunk));response.on('end',()=>resolve({status:response.statusCode,text:Buffer.concat(chunks).toString()}));
            response.on('error',reject);
        });
        req.setTimeout(5000,()=>req.destroy(new Error('HTTP deadline: '+target)));req.on('error',reject);
    });
}
function upgrade() {
    return new Promise((resolve,reject)=>{
        const socket=track(net.connect(backend.port,'127.0.0.1')),messages=[];
        const closed=new Promise(resolve=>socket.once('close',resolve));let input=Buffer.alloc(0),upgraded=false,closing=false;
        const key=crypto.randomBytes(16).toString('base64');
        const timer=setTimeout(()=>{socket.destroy();reject(new Error('upgrade deadline'))},5000);
        socket.once('connect',()=>socket.write(`GET /@capsid/orders/ws HTTP/1.1\r\nHost: public.example\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${key}\r\n\r\n`));
        socket.once('error',reject);
        socket.on('data',chunk=>{
            input=Buffer.concat([input,chunk]);
            if(!upgraded) {
                const end=input.indexOf('\r\n\r\n');if(end<0)return;
                clearTimeout(timer);const head=input.subarray(0,end).toString(),status=Number(/^HTTP\/1.1 (\d+)/.exec(head)?.[1]);input=input.subarray(end+4);upgraded=true;
                if(status===101)assert.equal(/Sec-WebSocket-Accept: ([^\r\n]+)/i.exec(head)?.[1],crypto.createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'));
                resolve({socket,closed,status,messages,send:text=>socket.write(frame(1,Buffer.from(text),true)),
                    next:()=>until(()=>messages.shift(),value=>value!==undefined,'inbound message'),
                    close:async()=>{if(!socket.destroyed && !closing) {closing=true;socket.write(frame(8,Buffer.from([3,232]),true))}await within(closed,'inbound close deadline')}});
                if(status!==101){socket.destroy();return}
            }
            input=readFrames(input,false,(opcode,payload)=>{
                if(opcode===1)messages.push(payload.toString());
                else if(opcode===8 && !closing) {closing=true;socket.write(frame(8,payload,true))}
                else if(opcode===8) {}
                else if(opcode===9)socket.write(frame(10,payload,true));
                else assert.fail(`unexpected inbound opcode ${opcode}`);
            });
        });
    });
}
async function operate(target,body,method) {
    const submitted=await backend.admin(target,body,method);assert.equal(submitted.status,202,submitted.text);
    const operation=await until(async()=>{
        const response=await backend.admin('/v1/operations/'+submitted.json.operationId);assert.equal(response.status,200);return response.json;
    },value=>['active','failed'].includes(value.state),'admin operation settles',15000);
    assert.notEqual(operation.state,'failed',JSON.stringify(operation));traces.push({type:'operation',target,state:operation.state});return operation;
}
function versionFiles(version,padded=false) {
    const dest=path.join(dir,'apps','orders',version);fs.mkdirSync(dest,{recursive:true,mode:0o700});
    fs.writeFileSync(path.join(dest,'bundle.mjs'),padded?paddedSource(version):source(version));
    fs.copyFileSync(path.join(dir,'apps','orders','v1','capsid.json'),path.join(dest,'capsid.json'));
}
async function appStatus() {const response=await backend.admin('/v1/apps/orders');assert.equal(response.status,200);return response.json}
async function metrics() {
    const response=await backend.admin('/metrics');assert.equal(response.status,200);const values={};
    for(const [key,name] of [['channels','channel_reservations'],['active','channel_active'],['dials','channel_pending_dials'],['queue','channel_queue_bytes'],['http','channel_http_inflight']]) {
        const line=response.text.split('\n').find(line=>line.startsWith('capsid_'+name+'{scope="service",'));
        assert.ok(line,`live ${key} service metric`);values[key]=Number(line.slice(line.lastIndexOf(' ')+1));
    }
    const pidLine=response.text.split('\n').find(line=>line.startsWith('capsid_worker_rss_bytes{'));
    values.pid=Number(/pid="(\d+)"/.exec(pidLine)?.[1]);
    values.generationDrains=response.text.split('\n')
        .filter(line=>line.startsWith('capsid_worker_events_total{event="generation_drained",app="orders",'))
        .reduce((sum,line)=>sum+Number(line.slice(line.lastIndexOf(' ')+1)),0);
    return values;
}
async function idle() {return until(metrics,value=>!value.channels && !value.active && !value.dials && !value.queue && !value.http,'all service resources return')}
function children() {
    const pids=new Set();
    for(const task of fs.readdirSync(`/proc/${backend.child.pid}/task`)) {
        try {for(const value of fs.readFileSync(`/proc/${backend.child.pid}/task/${task}/children`,'utf8').trim().split(/\s+/))if(value)pids.add(Number(value))}catch(error){if(error.code!=='ENOENT')throw error}
    }
    return [...pids].filter(pid=>{
        // This fixture Host only spawns its own Workers. Include zombies
        // and the short fork/exec window: neither a missing /proc/exe link
        // nor process death proves that the product reaper completed.
        try {const stat=fs.readFileSync(`/proc/${pid}/stat`,'utf8');return Number(stat.slice(stat.lastIndexOf(')')+2).split(' ')[1])===backend.child.pid}catch(error){if(['ENOENT','ESRCH'].includes(error.code))return false;throw error}
    });
}
function killOwnedWorker(pid) {
    assert.ok(pid>1 && children().includes(pid),'only this fixture host worker may be killed');
    const text=fs.readFileSync(`/proc/${pid}/stat`,'utf8'),stat=text.slice(text.lastIndexOf(')')+2).split(' ');
    assert.equal(Number(stat[1]),backend.child.pid,'worker parent belongs to this host');process.kill(pid,'SIGKILL');
}
async function checkVersion(expected) {const response=await request();assert.equal(response.status,200);assert.equal(response.text,expected)}
async function halfClosedRequest() {
    const socket=track(net.connect(backend.port,'127.0.0.1'));const chunks=[];
    socket.on('data',chunk=>chunks.push(chunk));
    const closed=new Promise(resolve=>socket.once('close',resolve));
    socket.once('connect',()=>socket.end('GET /@capsid/orders/slow-http HTTP/1.1\r\nHost: public.example\r\nConnection: close\r\n\r\n'));
    await within(closed,'half-closed HTTP response deadline');
    const wire=Buffer.concat(chunks).toString();assert.match(wire,/^HTTP\/1\.1 200 /);assert.ok(wire.endsWith('v1:slow'),'FIN on client send side preserves response');
}
async function openOut(target=peer.target) {
    const response=await request('/out?'+new URLSearchParams({target}));assert.equal(response.status,200);
    const result=JSON.parse(response.text);assert.ok(!result.events.some(event=>event.type==='callback-error'),JSON.stringify(result));return result;
}
try {
    peer=await makePeer();const bundle=path.join(dir,'source.mjs');fs.writeFileSync(bundle,scenario==='replacement-recursion'?paddedSource('v1'):source('v1'));
    const allow=[`127.0.0.1:${peer.port}`];
    // The churn scenarios (16 rounds / repeated failed persists) run
    // minutes long under loaded CI: a 1s worker request deadline turns
    // scheduler noise into legitimate worker replacements and deploy
    // failures. They do not assert timeout behavior, so they get a
    // relaxed request deadline; the deadline-sensitive scenarios keep
    // the tight 1s.
    const longRunning=scenario==='replacement-recursion'||scenario==='deploy-persist-failure';
    backend=await startCapsid(host,worker,bundle,dir,'managed',{permissions:{modules:[],fetch:{allow}},fetchTargets:allow,requestTimeoutMs:longRunning?5000:1000,
        env:allocatorEnvironment,
        hostCapacity:{workersTotal:1,activationSurgeWorkers:1},channelCapacity:{channelsTotal:scenario==='deploy'?4:2,pendingDialsTotal:2},
        channels:{dialTimeoutMs:30000},
        recovery:{restartBackoff:{initial:'20ms',maximum:'100ms',jitter:'0%',stableReset:'60s'}}});
    backend.agent=new http.Agent({keepAlive:true,maxSockets:1});versionFiles('v2');await checkVersion('v1');await halfClosedRequest();
    const initial=await appStatus();assert.equal(initial.active,true);assert.equal(initial.version,'v1');
    const samples={baseline:null};
    if(scenario==='replacement-recursion') {await delay(500);samples.baseline=hostStatus();observations.push({baseline:samples.baseline});}
    if(scenario==='deploy') {
        const old=await upgrade();assert.equal(old.status,101);assert.equal((await openOut()).opened,true);
        await delay(1100);old.send('past-http-deadline');assert.equal(await old.next(),'v1:past-http-deadline');
        old.send('deferred');
        const held=await until(()=>[...peer.connections].filter(x=>x.route==='/held' && !x.upgraded),value=>value.length===1,'old callback waits on real pending handshake');
        assert.equal((await operate('/v1/deploy',{app:'orders',version:'v2'})).state,'active');await checkVersion('v2');
        const active=await appStatus();assert.equal(active.version,'v2');assert.notEqual(active.generation,initial.generation);
        const fresh=await upgrade();assert.equal(fresh.status,101);fresh.send('new-owner');assert.equal(await fresh.next(),'v2:new-owner');
        const full=await until(metrics,value=>value.channels===4 && value.active===3 && value.dials===1 && !value.http,'both generations share quota');
        const before=peer.accepted;assert.equal((await openOut()).opened,false);assert.equal(peer.accepted,before,'new generation cannot bypass old reservations');
        held[0].upgrade();assert.equal(await old.next(),'v1:deferred','old async callback cannot migrate to new generation');
        await old.close();peer.closeWhere(entry=>entry.frames.includes('v1:open'));
        await until(metrics,value=>value.channels===1 && value.active===1 && !value.dials && !value.queue && !value.http,'old generation returns capacity');
        await until(children,value=>value.length===1,'old worker reaped');
        assert.equal((await operate('/v1/deploy',{app:'orders',version:'v1'})).state,'active');await checkVersion('v1');
        assert.equal((await appStatus()).generation,initial.generation,'same immutable artifact has the original identity');
        fresh.send('still-v2');assert.equal(await fresh.next(),'v2:still-v2');
        assert.equal((await operate('/v1/apps/orders/retire',undefined,'POST')).state,'active');
        assert.equal((await request()).status,404);assert.equal((await upgrade()).status,404);assert.equal((await appStatus()).active,false);
        fresh.send('retired-owner');assert.equal(await fresh.next(),'v2:retired-owner');await fresh.close();
        observations.push({initial,active,full,empty:await idle()});await until(children,value=>value.length===0,'retired workers reaped');
        assert.equal((await operate('/v1/deploy',{app:'orders',version:'v2'})).state,'active');await checkVersion('v2');
        const revived=await upgrade();assert.equal(revived.status,101);revived.send('revived');assert.equal(await revived.next(),'v2:revived');await revived.close();await idle();
    } else if(scenario==='retire-deadline') {
        const quiet=await upgrade();assert.equal(quiet.status,101);
        quiet.send('start-pending');assert.equal(await quiet.next(),'v1:pending');
        await until(()=>peer.accepted,value=>value===1,'real pending dial reaches handshake');
        const full=await until(metrics,value=>value.channels===2 && value.active===1 && value.dials===1 && !value.http,'quiet WS and pending constructor pin generation');
        const began=Date.now();assert.equal((await operate('/v1/apps/orders/retire',undefined,'POST')).state,'active');
        assert.equal((await request()).status,404);assert.equal((await upgrade()).status,404);
        const empty=await until(metrics,value=>!value.channels && !value.active && !value.dials && !value.queue && !value.http,'quiet drain deadline returns all resources',14000);
        const elapsedMs=Date.now()-began;assert.ok(elapsedMs>=9000 && elapsedMs<14000,'actual default 10s generation drain bound');
        await within(quiet.closed,'quiet inbound closes');await until(()=>peer.connections.size,value=>value===0,'pending transport canceled');
        await until(children,value=>value.length===0,'deadline reaps worker');await delay(300);assert.equal(children().length,0,'retired worker has no replacement');
        observations.push({full,empty,elapsedMs});
        assert.equal((await operate('/v1/deploy',{app:'orders',version:'v2'})).state,'active');await checkVersion('v2');
        const reused=await upgrade();assert.equal(reused.status,101);assert.equal((await openOut()).opened,true);await reused.close();peer.closeWhere(()=>true);await idle();
    } else if(scenario==='reclamation-idle') {
        await idle(); await delay(250);
        const before=hostStatus(),baseThreads=threadCount();
        versionFiles('large');
        fs.appendFileSync(path.join(dir,'apps','orders','large','bundle.mjs'),
            '\nexport const retainedFactoryPadding="'+'x'.repeat(2*1024*1024)+'";\n');
        assert.equal((await operate('/v1/deploy',{app:'orders',version:'large'})).state,'active');
        await checkVersion('large'); await until(children,value=>value.length===1,'large generation replaces baseline');
        const loaded=hostStatus();
        assert.ok(loaded.rss-before.rss>1024,'fixture must allocate observable retained factory input');
        assert.equal((await operate('/v1/deploy',{app:'orders',version:'v2'})).state,'active');
        await checkVersion('v2'); await idle(); await until(children,value=>value.length===1,'last retired worker reaped');
        // Only /proc observations follow: no later deploy/retire sweeps the last pool.
        const samples=[]; const deadline=Date.now()+5000;let after;
        do {after=hostStatus();samples.push(after);if(after.rss<=before.rss+1024)break;await delay(25)}while(Date.now()<deadline);
        observations.push({before,loaded,after,baseThreads,threads:threadCount(),allocatorEnvironment,samples});
        console.log('reclamation footprint:',JSON.stringify(observations.at(-1)));
        assertFootprint(after.rss<=before.rss+1024,'last retired large factory returns memory without another admin operation');
        assert.ok(threadCount()<=baseThreads,'retired pool threads are joined');
    } else if(scenario==='replacement-recursion') {
        // P1 regression (review 12.2): sixteen replacements of a
        // ~512 KiB generation must not accumulate retired pools.
        const rounds=16;
        const completedBefore=(await metrics()).generationDrains;
        for(let i=1;i<=rounds;i+=1) {
            const target=i%2===0?'v2':'v1';
            versionFiles(target,true);
            assert.equal((await operate('/v1/deploy',{app:'orders',version:target})).state,'active');
            await checkVersion(target);
            await until(children,value=>value.length===1,'round '+i+': old worker reaped');
            const completion=await until(metrics,value=>value.generationDrains===completedBefore+i,
                'round '+i+': old generation capacity released');
            observations.push({round:i,generationDrains:completion.generationDrains,children:children()});
        }
        await delay(500);
        const after=hostStatus();
        observations.push({final:after});
        assertFootprint(after.rss-samples.baseline.rss<6144,'16 replacements grow RSS by '
            +(after.rss-samples.baseline.rss)+' KiB; retired pools are reclaimed');
        assertFootprint(after.vm-samples.baseline.vm<65536,'16 replacements grow VmSize by '
            +(after.vm-samples.baseline.vm)+' KiB');
        assert.equal((await operate('/v1/apps/orders/retire',undefined,'POST')).state,'active');
        await idle();await until(children,value=>value.length===0,'retired workers reaped');
        const drained=hostStatus();
        observations.push({drained});
        assertFootprint(drained.rss-samples.baseline.rss<6144,'full retire returns the footprint');
    } else if(scenario==='deploy-persist-failure') {
        // P1 regression (review §2): a persist failure must abort the
        // never-published pool through plain refcount (no self-hold),
        // undo the old pool's parking reference, and leave the ledger
        // reusable — three consecutive failures may not accumulate
        // workers or threads.
        const stateApp=path.join(dir,'state','apps','orders');
        const baseThreads=threadCount();
        assert.equal(children().length,1,'baseline is one worker');
        // fix-review §6: the metrics identity must follow the generation
        // that is actually serving across failed activations
        const basePid=children()[0];
        const baseMetrics=await metrics();
        assert.equal(baseMetrics.pid,basePid,'metrics initially track the serving worker');
        // Root bypasses directory write permissions. Fail the same persist
        // transaction at rename in that environment, without changing its
        // worker teardown, thread, serving identity or ledger assertions.
        const persistFault=option('--persist-fault',process.getuid()===0?'rename':'permissions');
        assert.ok(['rename','permissions'].includes(persistFault));
        const activePath=path.join(stateApp,'active.json');
        const activeBackup=path.join(stateApp,'active.json.last-good');
        const activeBefore=fs.readFileSync(activePath);
        const activeDocument=JSON.parse(activeBefore);
        if(persistFault==='rename') {
            fs.renameSync(activePath,activeBackup);
            fs.mkdirSync(activePath,{mode:0o700});
        } else fs.chmodSync(stateApp,0o500);
        try {
            for(let round=1;round<=3;round+=1) {
                versionFiles('v2');
                const existingTemps=new Set(fs.readdirSync(stateApp).filter(name=>name.startsWith('active.json.tmp.')));
                const failed=await failedDeploy('v2');
                if(persistFault==='rename') {
                    // Admin operation IDs wrap the deployment transaction ID.
                    // Identify its actual exclusive temp by the new directory
                    // entry, rather than assuming those two IDs are equal.
                    const addedTemps=fs.readdirSync(stateApp).filter(name=>name.startsWith('active.json.tmp.') && !existingTemps.has(name));
                    assert.equal(addedTemps.length,1,'one newly persisted document per failed activation');
                    const temporary=path.join(stateApp,addedTemps[0]);
                    assert.ok(fs.lstatSync(temporary).isFile(),'persist reached the actual active-state temporary file');
                    const bytes=fs.readFileSync(temporary,'utf8'),document=JSON.parse(bytes);
                    assert.deepEqual(Object.keys(document),['schema','app','state','version','generation']);
                    assert.equal(document.schema,activeDocument.schema);
                    assert.equal(document.app,'orders');assert.equal(document.state,'active');assert.equal(document.version,'v2');
                    assert.match(document.generation,/^sha256:[0-9a-f]{64}$/);
                    assert.notEqual(document.generation,activeDocument.generation);
                    assert.equal(bytes,JSON.stringify(document),'failed rename leaves the complete canonical active-state document');
                    assert.ok(fs.lstatSync(path.join(stateApp,'generations',document.generation,'COMPLETE')).isFile(),
                        'persist failure occurs after generation materialization');
                    assert.ok(fs.lstatSync(activePath).isDirectory(),'the conflicting destination remains a directory');
                    assert.deepEqual(fs.readFileSync(activeBackup),activeBefore,'the previous committed pointer stays intact');
                    observations.push({round,persistFault,operationId:failed.operationId,generation:document.generation});
                } else assert.deepEqual(fs.readFileSync(activePath),activeBefore,'permission failure preserves the committed pointer');
                const reapBegan=Date.now();
                await until(children,value=>value.length===1,'failure '+round+': the never-published pool is reaped',20000);
                const reapMs=Date.now()-reapBegan;
                // the abort destructor blocks in bounded teardown
                // (drain deadline 10s + shutdown grace 2s): a reap
                // needing the full window is a defect, not latency
                assert.ok(reapMs<13000,'failure '+round+' reap needed '+reapMs+'ms; bounded teardown exceeded');
                // fix-review §6: metrics must keep tracking the
                // still-serving worker across the failed activation
                const afterFailure=await metrics();
                assert.equal(afterFailure.pid,basePid,
                    'failure '+round+': metrics track the still-serving worker');
                await checkVersion('v1');
                observations.push({round,workers:children().length,threads:threadCount(),reapMs,
                    metricsPid:afterFailure.pid});
            }
        } finally {
            if(persistFault==='rename') {fs.rmdirSync(activePath);fs.renameSync(activeBackup,activePath);}
            else fs.chmodSync(stateApp,0o700);
        }
        assert.deepEqual(fs.readFileSync(activePath),activeBefore,'restore the exact last committed pointer');
        const settled=threadCount();
        assert.ok(settled<=baseThreads+1,'threads return to baseline: '+settled+' vs '+baseThreads);
        assert.equal((await operate('/v1/deploy',{app:'orders',version:'v2'})).state,'active','ledger capacity is reusable after rollbacks');
        await checkVersion('v2');
        await until(children,value=>value.length===1,'successful redeploy has exactly one worker');
        observations.push({baseThreads,settled,persistFault});
    } else {
        const live=await upgrade();assert.equal(live.status,101);
        live.send('start-pending');assert.equal(await live.next(),'v1:pending');
        await until(()=>[...peer.connections].filter(entry=>entry.route==='/held').length,value=>value===1,'crash has a real pending handshake');
        const full=await until(metrics,value=>value.channels===2 && value.active===1 && value.dials===1 && !value.http,'crash starts with pinned socket and dial');
        const pid=full.pid;killOwnedWorker(pid);await within(live.closed,'crashed worker inbound closes');await until(()=>peer.connections.size,value=>value===0,'crashed worker pending transport closes');await idle();
        const nextPid=await until(children,value=>value.length===1 && value[0]!==pid,'managed replacement becomes a live child');
        await until(request,value=>value.status===200 && value.text==='v1','replacement reaches HTTP READY');
        assert.equal((await appStatus()).generation,initial.generation,'replacement retains committed artifact identity');
        const fresh=await upgrade();assert.equal(fresh.status,101);fresh.send('replacement');assert.equal(await fresh.next(),'v1:replacement');assert.equal((await openOut()).opened,true);
        const reused=await until(metrics,value=>value.channels===2 && value.active===2 && !value.dials && !value.http,'replacement reuses released service slots');
        const before=peer.accepted;assert.equal((await openOut()).opened,false);assert.equal(peer.accepted,before,'replacement respects service quota');
        await fresh.close();peer.closeWhere(()=>true);observations.push({pid,nextPid:nextPid[0],full,reused,empty:await idle()});
        assert.ok(backend.stderr().includes('worker_replaced'),'managed recovery emits replacement event');
    }
    const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
    report={complete:true,scenario,assertions,observations,traces,allocatorEnvironment,node:process.version,client:'independent raw Node TCP/HTTP plus Managed Admin',
        hostSHA256:hash(host),workerSHA256:hash(worker),sourceSHA256:hash(new URL(import.meta.url)),helperSHA256:hash(new URL('./helpers/ws_host_backend.mjs',import.meta.url))};
} catch(error) {
    console.error('managed traces:',JSON.stringify(traces));
    console.error('original scenario failure:',error?.stack??error);
    if(backend){try{console.error('failure metrics:',JSON.stringify(await metrics()));}catch(metricsError){console.error('failure metrics unavailable:',metricsError?.stack??metricsError);}}
    console.error('managed host stderr:',backend?.stderr());throw error;
}
finally {
    if(backend){backend.agent?.destroy();shutdown=await stopCapsid(backend.child)}
    for(const socket of sockets)socket.destroy();if(peer)await new Promise(resolve=>peer.server.close(resolve));
    fs.rmSync(dir,{recursive:true,force:true});
}
assert.equal(shutdown.forced,false,backend.stderr());assert.equal(shutdown.code,0,backend.stderr());
assert.doesNotMatch(backend.stderr(),/ERROR: (?:AddressSanitizer|LeakSanitizer)|(?:WARNING|FATAL): ThreadSanitizer|runtime error:/);
if(memoryAcceptance!=='production-rss')assert.equal(footprintChecks.length,scenario==='replacement-recursion'?3:1,
    'the original allocator predicates actually executed');
report.assertions=assertions;report.shutdown=shutdown;
report.memoryAcceptance={profile:memoryAcceptance,footprintChecks,
    fullLeakDetectionRequired:memoryAcceptance==='asan-resources',sanitizerRuntimeMarkers,
    asanOptions:memoryAcceptance==='asan-resources'?process.env.ASAN_OPTIONS:undefined,
    tsanOptions:memoryAcceptance==='tsan-resources'?process.env.TSAN_OPTIONS:undefined,
    sanitizerProfile:memoryAcceptance==='production-rss'?undefined:memoryAcceptance.split('-')[0],
    separateCompleteLsanAcceptanceRequired:memoryAcceptance==='tsan-resources',
    productionRssAcceptanceRequiredSeparately:memoryAcceptance!=='production-rss'};
if(option('--report'))fs.writeFileSync(option('--report'),JSON.stringify(report,null,2));
console.log(JSON.stringify(report));
