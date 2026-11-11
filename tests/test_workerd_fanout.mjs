import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid,stopCapsid} from './helpers/ws_host_backend.mjs';
import {loadNodeWS} from './helpers/ws_interop_node_sdk.mjs';
import {readLock,verifyClient,verifyBinary,fileDigest} from '../tools/workerd-reference.mjs';
import {fanoutInputPaths,fanoutCases,fanoutOverlapCases,validateFanoutReport} from '../tools/workerd-fanout-contract.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const args=new Map();
for(let index=2;index<process.argv.length;index+=2) {
    assert.ok(process.argv[index]?.startsWith('--')&&process.argv[index+1],'complete argument pair');
    args.set(process.argv[index],process.argv[index+1]);
}
if(args.has('--node-package'))process.env.CAPSID_WS_INTEROP_NODE_PACKAGE=args.get('--node-package');
const reference=args.get('--workerd')??(args.has('--reference-from-env')?process.env.CAPSID_WORKERD_REF:undefined);
const mode=reference||args.has('--reference-from-env')?'reference':args.get('--mode')??'single-worker';
const runId=process.env.CAPSID_WS_RUN_ID??randomUUID();
const destination=args.get('--report')??(process.env.CAPSID_WS_REPORT_DIR?path.join(process.env.CAPSID_WS_REPORT_DIR,'fanout-'+mode+'.json'):undefined);
const write=value=>{if(destination)fs.writeFileSync(destination,JSON.stringify(value,null,2)+'\n');};
write({complete:false,status:'running',mode,runId});
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-fanout-'));
const backends=[],peers=[];
const trace={discovery:[],selected:[],extraCloses:[],baseline:[],probes:[],overlap:[],echoes:[],memberCloses:[],afterClose:[],finalCloses:[]};
let report;
async function unusedPorts() {
    const servers=[];
    try {
        for(let index=0;index<2;index++) {
            const server=net.createServer();servers.push(server);
            await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
        }
        return servers.map(server=>server.address().port);
    }finally{for(const server of servers)if(server.listening)await new Promise(resolve=>server.close(resolve));}
}
async function healthy(port) {
    const response=await fetch(`http://127.0.0.1:${port}/@capsid/orders/healthy`,{signal:AbortSignal.timeout(2000)});
    return {status:response.status,text:await response.text()};
}
async function startReference(fixture,lock) {
    assert.ok(reference,'CAPSID_WORKERD_REF is required');verifyBinary(reference,lock);
    const ports=await unusedPorts();assert.notEqual(...ports);
    fs.copyFileSync(fixture,path.join(dir,'main.mjs'));
    const config=path.join(dir,'reference.capnp');
    fs.writeFileSync(config,`using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="first",worker = .mainWorker),(name="second",worker = .mainWorker)],
sockets=[(name="first",address="127.0.0.1:${ports[0]}",http=(),service="first"),(name="second",address="127.0.0.1:${ports[1]}",http=(),service="second")]);
const mainWorker :Workerd.Worker = (compatibilityDate=${JSON.stringify(lock.compatibility_date)},
compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")]);\n`);
    const child=spawn(reference,['serve',config],{stdio:['ignore','pipe','pipe']});
    let stderr='';child.stderr.setEncoding('utf8');child.stderr.on('data',chunk=>stderr+=chunk);child.stdout.resume();
    const backend={child,ports,stderr:()=>stderr};backends.push(backend);
    const deadline=performance.now()+10000;
    for(const port of ports)for(;;) {
        assert.ok(performance.now()<deadline&&child.exitCode===null&&child.signalCode===null,'reference startup: '+stderr);
        try{if((await healthy(port)).status===200)break;}catch{}
        await delay(10);
    }
}
function connect(WebSocket,endpoint) {
    const peer={ws:new WebSocket(endpoint,{perMessageDeflate:false}),endpoint,messages:[],errors:[]};
    peers.push(peer);
    peer.opened=new Promise((resolve,reject)=>{
        const timer=setTimeout(()=>reject(new Error('fanout Upgrade deadline')),5000);
        peer.ws.once('open',()=>{
            clearTimeout(timer);
            try{assert.equal(peer.ws.protocol,'');assert.equal(peer.ws.extensions,'');resolve();}catch(error){reject(error);}
        });
        peer.ws.once('error',error=>{clearTimeout(timer);reject(error);});
    });
    peer.closed=new Promise(resolve=>peer.ws.once('close',(code,reason)=>{
        const result={code,reason:reason.toString(),clean:peer.ws._closeFrameSent&&peer.ws._closeFrameReceived};
        peer.close=result;
        if(peer.pending){clearTimeout(peer.pending.timer);peer.pending.reject(new Error('fanout closed before response'));peer.pending=null;}
        resolve(result);
    }));
    peer.ws.on('error',error=>{
        peer.errors.push({name:error.name,message:error.message});
        if(peer.pending){clearTimeout(peer.pending.timer);peer.pending.reject(error);peer.pending=null;}
    });
    peer.ws.on('message',(data,binary)=>{
        const value={text:data.toString(),binary};
        if(peer.pending){const pending=peer.pending;peer.pending=null;clearTimeout(pending.timer);pending.resolve(value);}
        else peer.messages.push(value);
    });
    return peer;
}
async function request(peer,op,challenge,target,operation) {
    assert.equal(peer.messages.length,0,'no unexpected message before a fanout command');
    assert.equal(peer.ws.readyState,1,'fanout connection remains open');assert.ok(!peer.pending);
    const result=new Promise((resolve,reject)=>{
        const timer=setTimeout(()=>{peer.pending=null;reject(new Error('fanout '+op+' reply deadline'));},5000);
        peer.pending={resolve,reject,timer};
    });
    peer.ws.send('@capsid-fanout:'+JSON.stringify({op,challenge,...(target?{target,operation}:{})}));
    const message=await result;assert.equal(message.binary,false);
    const response=JSON.parse(message.text);assert.equal(response.op,op);assert.equal(response.challenge,challenge);
    return response;
}
async function close(peer,reason) {
    assert.equal(peer.ws.readyState,1,'close an actual live connection');peer.ws.close(1000,reason);
    let timer;
    const timeout=new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('fanout Close deadline')),4000);});
    const value=await Promise.race([peer.closed,timeout]).finally(()=>clearTimeout(timer));
    assert.deepEqual(value,{code:1000,reason,clean:true});
    return {connection:peer.identity.connection,...value};
}
async function settle(peer,ids,stage,index) {
    const values=[],deadline=performance.now()+3000;
    for(let attempt=0;attempt<128;attempt++) {
        const value=await request(peer,'stats',`${runId}:${stage}:${index}:${attempt}`);values.push(value);
        if(JSON.stringify(value.members.toSorted())===JSON.stringify(ids.toSorted()))return values;
        assert.ok(performance.now()<deadline,'closed fanout member leaves its room');await delay(10);
    }
    assert.fail('fanout room cleanup observation limit');
}
try {
    const lock=readLock();verifyClient(lock);
    if(mode==='reference')assert.ok(reference,'CAPSID_WORKERD_REF is required for fanout acceptance');
    const clientLock=JSON.parse(fs.readFileSync(path.join(root,'tests/fixtures/websocket-interop-lock.json'),'utf8'));
    const {WebSocket,identity:client}=loadNodeWS();
    const fixture=path.resolve(args.get('--fixture')??path.join(root,fanoutInputPaths.fixture));
    const inputs=Object.fromEntries(Object.entries(fanoutInputPaths).map(([name,file])=>[name,fileDigest(name==='fixture'?fixture:path.join(root,file))]));
    const binaries={host:fileDigest(reference??args.get('--host')),worker:reference?null:fileDigest(args.get('--worker'))};
    if(mode==='reference')await startReference(fixture,lock);
    else for(let index=0;index<(mode==='single-worker'?2:1);index++) {
        const subdir=path.join(dir,'host-'+index);fs.mkdirSync(subdir);
        const backend=await startCapsid(args.get('--host'),args.get('--worker'),fixture,subdir,mode,
            {workers:mode==='single-worker'?1:2});backends.push({...backend,ports:[backend.port]});
    }
    const endpoints=backends.flatMap(b=>b.ports).map(port=>`ws://127.0.0.1:${port}/@capsid/orders/fanout`);
    const groups=new Map();
    for(let index=0;index<64;index++) {
        const peer=connect(WebSocket,endpoints[index%endpoints.length]);await peer.opened;
        peer.identity=await request(peer,'identity',runId+':identity:'+index);
        trace.discovery.push({endpoint:peer.endpoint,identity:peer.identity});
        if(!groups.has(peer.identity.instance))groups.set(peer.identity.instance,[]);
        groups.get(peer.identity.instance).push(peer);
        assert.ok(groups.size<=2,'configured two Workers produce at most two instance groups');
        if(groups.size===2&&[...groups.values()].every(group=>group.length>=2))break;
    }
    assert.equal(groups.size,2,'two distinct application Worker instances were exercised');
    assert.ok([...groups.values()].every(group=>group.length>=2),'two actual connections in each Worker group');
    const selected=[...groups.values()].flatMap(group=>group.slice(0,2));
    trace.selected=selected.map(p=>p.identity.connection);
    for(const peer of peers.filter(peer=>!selected.includes(peer)))trace.extraCloses.push(await close(peer,'fanout-extra'));
    for(const [index,peer] of selected.entries())trace.baseline.push(await settle(peer,selected.filter(p=>p.identity.instance===peer.identity.instance).map(p=>p.identity.connection),'baseline',index));
    for(const name of fanoutCases) {
        const [kind,operation,groupText,withinText]=name.split('/');
        const same=kind==='same',group=Number(groupText),within=Number(withinText),slot=group*2+within;
        const sender=selected[slot],target=selected[same?group*2+1-within:(1-group)*2+within];
        const response=await request(sender,'probe',`${runId}:${name}:\0界😀`,target.identity.connection,operation);
        trace.probes.push({name,sender:sender.identity.connection,target:target.identity.connection,response});
        for(const peer of peers)assert.equal(peer.messages.length,0,'fanout does not deliver into another request/Worker');
    }
    for(const name of fanoutOverlapCases) {
        const [,operation,groupText]=name.split('/'),group=Number(groupText);
        const sender=selected[group*2],target=selected[group*2+1];
        const pending=request(target,'hold',`${runId}:${name}:hold`);pending.catch(()=>{});
        const entry={name,sender:sender.identity.connection,target:target.identity.connection,heldBefore:[]};
        trace.overlap.push(entry);
        const deadline=performance.now()+3000;
        for(let index=0;index<128;index++) {
            const value=await request(sender,'held',`${runId}:${name}:held:${index}`);entry.heldBefore.push(value);
            if(value.held.includes(target.identity.connection))break;
            assert.ok(performance.now()<deadline,'target handler enters its pending phase');await delay(10);
        }
        assert.deepEqual(entry.heldBefore.at(-1).held,[target.identity.connection]);
        entry.probe=await request(sender,'probe',`${runId}:${name}:\0界😀`,target.identity.connection,operation);
        assert.equal(entry.probe.attempts.length,1);
        assert.equal(entry.probe.attempts[0].outcome,'blocked','foreign operation is rejected before the other handler stages output');
        entry.release=await request(sender,'release',`${runId}:${name}:release`,target.identity.connection);
        entry.holdReply=await pending;
        for(const peer of peers)assert.equal(peer.messages.length,0,'overlapping handlers do not deliver foreign output');
    }
    for(const [index,peer] of selected.entries())trace.echoes.push(await request(peer,'echo',`${runId}:echo:${index}`));
    for(const peer of [selected[0],selected[2]])trace.memberCloses.push(await close(peer,'fanout-member'));
    for(const [index,peer] of [selected[1],selected[3]].entries())trace.afterClose.push(await settle(peer,[peer.identity.connection],'after-close',index));
    for(const peer of [selected[1],selected[3]])trace.finalCloses.push(await close(peer,'fanout-done'));
    const checks=[];for(const backend of backends)for(const port of backend.ports)checks.push(await healthy(port));
    const shutdowns=[];for(const backend of backends)shutdowns.push(await stopCapsid(backend.child));
    for(const peer of peers)assert.deepEqual(peer.errors,[],'no independent-client errors');
    report={schema:'capsid-workerd-fanout-v1',complete:true,runId,mode,referenceLock:lock,clientLock,inputs,binaries,client,
        topology:{workers:2,hostProcesses:backends.length,referenceServices:mode==='reference'?2:0},endpoints,...trace,
        unexpectedMessages:peers.map(peer=>({connection:peer.identity.connection,messages:peer.messages})),
        healthy:checks,shutdowns,backendStderr:backends.map(b=>b.stderr())};
    if(destination)fs.writeFileSync(destination+'.raw.json',JSON.stringify(report,null,2)+'\n');
    validateFanoutReport(report,{runId,lock,clientLock,inputs,host:reference?undefined:binaries.host,worker:reference?undefined:binaries.worker});
    write(report);console.log(`fanout ${mode}: two actual Worker groups, ${trace.probes.length} ownership probes, ${peers.length} actual connections PASS`);
}catch(error) {
    write({complete:false,status:'failed',runId,mode,error:error.message});
    if(destination)fs.writeFileSync(destination+'.trace.json',JSON.stringify({complete:false,runId,mode,...trace,backendStderr:backends.map(b=>b.stderr()),error:error.message},null,2)+'\n');
    console.error(error,...backends.map(b=>b.stderr()));process.exitCode=1;
}finally {
    for(const peer of peers){if(peer.pending)clearTimeout(peer.pending.timer);if(peer.ws.readyState!==3)peer.ws.terminate();}
    for(const backend of backends)await stopCapsid(backend.child);
    fs.rmSync(dir,{recursive:true,force:true});
}
