import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {root,readLock,verifyClient,verifyBinary,fileDigest} from '../tools/workerd-reference.mjs';
import {fetchUpgradeLifetimePeer} from './helpers/ws_fetch_upgrade_lifetime_peer.mjs';
import {startCapsid,stopCapsid} from './helpers/ws_host_backend.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';
import {fetchUpgradeLifetimeCases,fetchUpgradeLifetimeInputPaths,validateFetchUpgradeLifetimeReport} from '../tools/workerd-fetch-upgrade-lifetime-contract.mjs';

const kinds = fetchUpgradeLifetimeCases, inputPaths = fetchUpgradeLifetimeInputPaths;
const options = new Map(), allowed = new Set(['--host','--worker','--mode','--report','--reference-from-env']);
for (let index=2;index<process.argv.length;index+=2) {const key=process.argv[index],value=process.argv[index+1]; assert.ok(allowed.has(key)&&value&&!options.has(key));options.set(key,value);}
const reference=options.get('--reference-from-env');assert.ok(reference===undefined||reference==='1');
const mode=reference?'reference':options.get('--mode');assert.ok(['reference','single-worker','static-pool','managed'].includes(mode));
assert.ok(reference||mode!=='reference','product cannot select the reference role');
const binary=reference?process.env.CAPSID_WORKERD_REF:options.get('--host'),worker=options.get('--worker');assert.ok(binary&&(reference||worker));
assert.ok(!reference||!options.has('--host')&&!options.has('--worker')&&!options.has('--mode'));
const lock=readLock();verifyClient(lock);if(reference)verifyBinary(binary,lock);
const inputs=Object.fromEntries(Object.entries(inputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))]));
const binaries={host:fileDigest(binary),worker:reference?null:fileDigest(worker)};
const runId=process.env.CAPSID_WS_RUN_ID??randomUUID();
const destination=options.get('--report')??path.join(process.env.CAPSID_WS_REPORT_DIR??os.tmpdir(),
    (process.env.CAPSID_WS_REPORT_DIR?'fetch-upgrade-lifetime-':'capsid-fetch-upgrade-lifetime-'+runId+'-')+mode+'.json');
assert.ok(!fs.existsSync(destination));fs.mkdirSync(path.dirname(destination),{recursive:true});
const report={schema:'capsid-fetch-upgrade-lifetime-v1',complete:false,mode,runId,referenceLock:lock,
    client:{node:process.version,undici:process.versions.undici},inputs,binaries,cases:[]};
const save=()=>fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');save();
async function observe(predicate,milliseconds=3000) {const deadline=performance.now()+milliseconds;while(!predicate()&&performance.now()<deadline)await delay(5);return predicate();}
async function stopReference(backend) {
    let timer,forced=false;if(backend.child.exitCode===null&&backend.child.signalCode===null)backend.child.kill('SIGTERM');
    try {await Promise.race([backend.closed,new Promise(resolve=>{timer=setTimeout(()=>{forced=true;backend.child.kill('SIGKILL');resolve();},2000);})]);await backend.closed;}
    finally {clearTimeout(timer);}return {code:backend.child.exitCode,signal:backend.child.signalCode,forced};
}
async function startReference(directory) {
    const listener=net.createServer();await new Promise(resolve=>listener.listen(0,'127.0.0.1',resolve));const port=listener.address().port;await new Promise(resolve=>listener.close(resolve));
    fs.copyFileSync(path.join(root,inputPaths.fixture),path.join(directory,'main.mjs'));const config=path.join(directory,'config.capnp');
    fs.writeFileSync(config,`using Workerd = import "/workerd/workerd.capnp"; const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child=spawn(binary,['serve',config],{stdio:['ignore','pipe','pipe']});let stderr='';child.stdout.resume();child.stderr.on('data',value=>{stderr+=value;});
    const backend={child,port,closed:new Promise(resolve=>child.once('close',resolve)),stderr:()=>stderr};
    try {const deadline=performance.now()+10000;for(;;)try{const response=await fetch(`http://127.0.0.1:${port}/state`,{signal:AbortSignal.timeout(500)});assert.equal(response.status,200);await response.text();return backend;}
        catch(error){assert.ok(performance.now()<deadline&&child.exitCode===null&&child.signalCode===null,stderr+error);await delay(20);}}
    catch(error){await stopReference(backend);throw error;}
}
async function runAwaitCase(kind) {
    const value={kind,challenge:randomUUID()},directory=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-upgrade-await-'));report.cases.push(value);save();
    const upstream=[];let backend,peer,state;
    try {
        const last=kind.endsWith('-last'),deferred=kind.startsWith('await-deferred-');
        if(!last)upstream.push(await fetchUpgradeLifetimePeer('socket',value.challenge,'a'));
        const pending=await fetchUpgradeLifetimePeer('pending',value.challenge,'b');upstream.push(pending);
        value.targets=upstream.map((item,index)=>({host:'localhost',port:item.port,role:!last&&index===0?'a':'b'}));
        const allow=value.targets.map(target=>target.host+':'+target.port);
        backend=reference?await startReference(directory):await startCapsid(binary,worker,path.join(root,inputPaths.fixture),directory,mode,{permissions:{modules:[],fetch:{allow}},fetchTargets:allow});
        const prefix=reference?'':'/@capsid/orders';state=async()=>{const response=await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`,{signal:AbortSignal.timeout(4000)});return {status:response.status,text:await response.text()};};
        const snapshot=()=>upstream.flatMap(item=>item.snapshot());value.initial=await state();
        const query=new URLSearchParams({kind,challenge:value.challenge,secondTarget:`http://localhost:${pending.port}/hold?challenge=${value.challenge}&role=b`});
        if(!last)query.set('target',`http://localhost:${upstream[0].port}/hold?challenge=${value.challenge}&role=a`);
        peer=responseSession(backend.port,prefix+'/probe?'+query,{observeCloseReply:true,timeoutMs:15000});value.header=await peer.headers;assert.equal(value.header.status,101);peer.send('go');
        if(!last)assert.equal(await peer.text(),'socket-ready');assert.equal(await peer.text(),'waiting');
        assert.ok(await observe(()=>pending.snapshot().length===1&&pending.snapshot()[0].events.includes('response-held')),'actual pending asynchronous fetch');
        value.beforeAction=await state();value.beforeUpstream=snapshot();
        if(kind==='await-close-parent') {peer.close();value.wire=await peer.complete;}
        else if(kind.startsWith('await-abrupt')||deferred) {peer.dispose();try{await peer.complete;}catch(error){value.wire=error.observation;}}
        await delay(100);value.afterAction=await state();value.actionUpstream=snapshot();
        await delay(100);value.checkpoint=await state();value.checkpointUpstream=snapshot();
        value.pendingTerminal=pending.snapshot().every(item=>item.peerEnded||item.peerReset);
        if(!value.pendingTerminal) {pending.respondHttp();let document;const deadline=performance.now()+2000;
            for(;;){document=await state();if(JSON.parse(document.text).state.response||performance.now()>=deadline)break;await delay(10);}value.afterRelease=document;}
        else value.afterRelease=await state();
        if(!value.pendingTerminal)assert.ok(await observe(()=>pending.snapshot().every(item=>item.peerEnded)),'released body actual EOF');
        value.releaseUpstream=snapshot();
        if(kind==='await-open-parent') {peer.send('finish');assert.equal(await peer.text(),'ack/finish');value.wire=await peer.complete;}
        if(!last) {upstream[0].sendAfterParent();assert.ok(await observe(()=>snapshot().every(item=>item.peerEnded)),'child and pending request terminal EOF');
            const deadline=performance.now()+3000;for(;;){const document=await state();if(JSON.parse(document.text).state.socket.closed){value.observed=document;break;}assert.ok(performance.now()<deadline);await delay(5);}}
        else value.observed=await state();
        await delay(100);value.finalState=await state();value.upstream=snapshot();
    }catch(error){value.failure=error.stack;if(state)try{value.observed=await state();}catch(failure){value.stateFailure=failure.stack;}value.upstream=upstream.flatMap(item=>item.snapshot());}
    finally{peer?.dispose();if(backend){try{value.shutdown=reference?await stopReference(backend):await stopCapsid(backend.child);}catch(error){value.cleanupFailure=error.stack;}value.stderr=backend.stderr();}for(const item of upstream)await item.stop();save();}
}
async function runFetchedDisconnectCase(kind) {
    const value={kind,challenge:randomUUID()},directory=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-fetch-close-kind-'));report.cases.push(value);let backend,peer,up,stopped=false;
    try{
        up=await fetchUpgradeLifetimePeer('socket',value.challenge,'a');const allow=['localhost:'+up.port];value.target={host:'localhost',port:up.port};
        backend=reference?await startReference(directory):await startCapsid(binary,worker,path.join(root,inputPaths.fixture),directory,mode,{permissions:{modules:[],fetch:{allow}},fetchTargets:allow});
        const prefix=reference?'':'/@capsid/orders';const state=async()=>{const response=await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`,{signal:AbortSignal.timeout(4000)});return {status:response.status,text:await response.text()};};
        value.initial=await state();const query=new URLSearchParams({kind,challenge:value.challenge,target:`http://localhost:${up.port}/hold?challenge=${value.challenge}&role=a`});
        peer=responseSession(backend.port,prefix+'/probe?'+query,{observeCloseReply:true,timeoutMs:15000});value.header=await peer.headers;peer.send('go');assert.equal(await peer.text(),'socket-ready');value.before=await state();value.upstreamBefore=up.snapshot();
        value.upstreamAction={kind:'fixture-initiated-transport-disposal'};await up.stop();stopped=true;
        assert.equal(await peer.text(),'closed');value.wire=await peer.complete;value.observed=await state();await delay(100);value.finalState=await state();value.upstream=up.snapshot();
    }catch(error){value.failure=error.stack;}
    finally{peer?.dispose();if(backend){value.shutdown=reference?await stopReference(backend):await stopCapsid(backend.child);value.stderr=backend.stderr();}if(up&&!stopped)await up.stop();save();}
}
for(const kind of kinds) {
    if(kind==='fetched-disconnect-close') {await runFetchedDisconnectCase(kind);continue;}
    if(kind.startsWith('await-')) {await runAwaitCase(kind);continue;}
    const value={kind,challenge:randomUUID()},directory=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-upgrade-lifetime-'));report.cases.push(value);save();
    const upstream=[],peers={};let backend,state;
    try {
        const sameOrigin=kind.startsWith('same-origin-'),completeDescendant=sameOrigin&&kind.endsWith('-complete');
        const type=kind==='close-pending'||kind==='two-owners'?'pending':kind==='close-unaccepted'||kind==='accepted-independent'||sameOrigin?'socket':'body';
        const count=kind==='two-owners'||sameOrigin?2:1;
        for(let index=0;index<count;index++)upstream.push(await fetchUpgradeLifetimePeer(sameOrigin&&index?'pending':type,value.challenge,index?'b':'a'));
        value.targets=upstream.map(peer=>({host:'localhost',port:peer.port}));const allow=value.targets.map(target=>target.host+':'+target.port);
        backend=reference?await startReference(directory):await startCapsid(binary,worker,path.join(root,inputPaths.fixture),directory,mode,{permissions:{modules:[],fetch:{allow}},fetchTargets:allow});
        const prefix=reference?'':'/@capsid/orders';state=async()=>{const response=await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`,{signal:AbortSignal.timeout(4000)});return {status:response.status,text:await response.text()};};
        value.initial=await state();value.headers={};value.wires={};value.checkpoint=null;value.checkpointUpstream=null;
        const snapshot=()=>upstream.flatMap(peer=>peer.snapshot());
        async function open(role,index) {
            const query=new URLSearchParams({kind,challenge:value.challenge,role,target:`http://localhost:${upstream[index].port}/hold?challenge=${value.challenge}&role=${role}`});
            if(sameOrigin)query.set('secondTarget',`http://localhost:${upstream[1].port}/hold?challenge=${value.challenge}&role=b`);
            const peer=peers[role]=responseSession(backend.port,prefix+'/probe?'+query,{observeCloseReply:true,timeoutMs:15000});value.headers[role]=await peer.headers;assert.equal(value.headers[role].status,101);
            peer.send('go');assert.equal(await peer.text(),'started');return peer;
        }
        const peer=await open('a',0);
        if(type==='pending')assert.ok(await observe(()=>snapshot().some(entry=>entry.events.includes('response-held'))),'actual pending request');
        else assert.equal(await peer.text(),type==='socket'?kind==='close-unaccepted'?'headers':'socket-ready':kind==='close-unread'?'headers':'reading');
        if(kind==='two-owners') {await open('b',1);assert.ok(await observe(()=>snapshot().length===2&&snapshot().every(entry=>entry.events.includes('response-held'))),'two actual pending owners');}
        if(sameOrigin)assert.ok(await observe(()=>upstream[1].snapshot().some(entry=>entry.events.includes('response-held'))),'actual same-origin pending descendant');
        value.beforeAction=await state();value.beforeUpstream=snapshot();
        if(kind==='cancel-reader') {peer.send('cancel');assert.equal(await peer.text(),'cancelled');assert.ok(await observe(()=>snapshot().every(entry=>entry.peerEnded||entry.peerReset)),'reader cancellation terminal');}
        if(kind==='abort-body') {peer.send('abort');assert.equal(await peer.text(),'ack/abort');assert.equal(await peer.text(),'failed');assert.ok(await observe(()=>snapshot().every(entry=>entry.peerEnded||entry.peerReset)),'body abort terminal');}
        peer.send('finish');assert.equal(await peer.text(),'ack/finish');value.wires.a=await peer.complete;
        value.closedParent=await state();value.closedUpstream=snapshot();
        value.terminalAfterParent=await observe(()=>upstream[0].snapshot().every(entry=>entry.peerEnded||entry.peerReset),kind==='accepted-independent'||sameOrigin?100:1500);
        if(kind==='two-owners') {
            const other=peers.b;other.send('checkpoint');assert.equal(await other.text(),'ack/checkpoint');value.checkpoint=await state();value.checkpointUpstream=snapshot();
            upstream[1].accept();assert.equal(await other.text(),'socket-ready');other.send('finish');assert.equal(await other.text(),'ack/finish');value.wires.b=await other.complete;
            upstream[1].sendAfterParent();
            assert.ok(await observe(()=>upstream[1].snapshot().every(entry=>entry.peerEnded)),'independent accepted second socket EOF');
        } else if(kind==='accepted-independent'||sameOrigin) {
            if(sameOrigin) {
                if(completeDescendant) {
                    const before=JSON.parse(value.closedParent.text).state.owners.b.timerTicks,deadline=performance.now()+3000;
                    for(;;) {const document=await state();if(JSON.parse(document.text).state.owners.b.timerTicks>before){value.checkpoint=document;break;}
                        assert.ok(performance.now()<deadline,'same-origin native timer continues after parent close');await delay(5);}
                } else value.checkpoint=await state();
                value.checkpointUpstream=snapshot();
                if(completeDescendant) {
                    upstream[1].respondHttp();const deadline=performance.now()+3000;
                    for(;;) {const document=await state();if(JSON.parse(document.text).state.owners.b.first){value.afterRelease=document;break;}
                        assert.ok(performance.now()<deadline,'pending fetch continuation after its original parent closed');await delay(5);}
                    assert.ok(await observe(()=>upstream[1].snapshot().every(entry=>entry.peerEnded)),'actual released HTTP response EOF');value.afterReleaseUpstream=snapshot();
                }
            }
            upstream[0].sendAfterParent();assert.ok(await observe(()=>snapshot().every(entry=>entry.peerEnded)),'independent accepted socket and its pending descendant EOF');
        }
        if(kind==='accepted-independent'||kind==='two-owners'||sameOrigin) {
            const role=kind==='two-owners'?'b':'a',deadline=performance.now()+3000;
            for(;;) {const document=await state();if(JSON.parse(document.text).state.owners[role].socket.closed){value.observed=document;break;}
                assert.ok(performance.now()<deadline,'accepted child close callback after parent completion');await delay(5);}
        } else value.observed=await state();
        await delay(100);value.finalState=await state();value.upstream=snapshot();
    } catch(error) {value.failure=error.stack;if(error.observation)value.failureObservation=error.observation;if(state)try{value.observed=await state();}catch(failure){value.stateFailure=failure.stack;}value.upstream=upstream.flatMap(peer=>peer.snapshot());}
    finally {for(const peer of Object.values(peers))peer.dispose();if(backend){try{value.shutdown=reference?await stopReference(backend):await stopCapsid(backend.child);}catch(error){value.cleanupFailure=error.stack;}value.stderr=backend.stderr();}
        for(const peer of upstream)await peer.stop();save();}
}
assert.deepEqual(inputs,Object.fromEntries(Object.entries(inputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])));
assert.deepEqual(binaries,{host:fileDigest(binary),worker:reference?null:fileDigest(worker)});
report.complete=report.cases.every(value=>!value.failure&&!value.cleanupFailure&&!value.shutdown.forced);save();
console.log(JSON.stringify({collectionComplete:report.complete,mode,cases:report.cases.length}));
validateFetchUpgradeLifetimeReport(report,{runId:report.runId,lock,inputs,host:binaries.host,worker:binaries.worker});
console.log(JSON.stringify({complete:true,mode,cases:report.cases.length}));
