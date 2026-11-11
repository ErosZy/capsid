import assert from 'node:assert/strict';import fs from 'node:fs';import path from 'node:path';import os from 'node:os';import http from 'node:http';
import {randomUUID,createHash} from 'node:crypto';import {fileURLToPath,pathToFileURL} from 'node:url';import {setTimeout as delay} from 'node:timers/promises';
const E=path.dirname(fileURLToPath(import.meta.url)),R=path.resolve(E,'..');
const {startCapsid,stopCapsid}=await import(pathToFileURL(path.join(R,'tests/helpers/ws_host_backend.mjs')).href);
const {responseSession}=await import(pathToFileURL(path.join(R,'tests/helpers/ws_response_client.mjs')).href);
const {readLock,verifyClient}=await import(pathToFileURL(path.join(R,'tools/workerd-reference.mjs')).href);
assert.equal(process.platform,'linux');verifyClient(readLock());
const options=new Map(),allowed=new Set(['--host','--worker','--mode','--report']);
for(let index=2;index<process.argv.length;index+=2){const key=process.argv[index],value=process.argv[index+1];assert.ok(allowed.has(key)&&value&&!options.has(key),'complete unique supported option');options.set(key,value);}
assert.equal(options.get('--mode')??'managed','managed');
const host=options.get('--host'),worker=options.get('--worker');assert.ok(host&&worker);
const sha=file=>createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const destination=options.get('--report')??path.join(process.env.CAPSID_WS_REPORT_DIR??os.tmpdir(),'managed-local-owner-'+randomUUID()+'.json');
fs.mkdirSync(path.dirname(destination),{recursive:true});
const report={schema:'capsid-managed-local-owner-v1',complete:false,platform:process.platform,scope:'Actual managed generation retention, timer-only incoming EOF, explicit local close and unchanged drain deadline; all four cases are mandatory.',inputs:{runner:sha(fileURLToPath(import.meta.url)),backend:sha(path.join(R,'tests/helpers/ws_host_backend.mjs')),client:sha(path.join(R,'tests/helpers/ws_response_client.mjs'))},binaries:{host:sha(host),worker:sha(worker)},cases:[]};
assert.ok(!fs.existsSync(destination));const save=()=>fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');save();
async function until(read,condition,label,timeout=5000) {
    const deadline=performance.now()+timeout;let value;
    for(;;){value=await read();if(condition(value))return value;assert.ok(performance.now()<deadline,label+': '+JSON.stringify(value));await delay(10);}
}
for(const kind of ['steady','deploy-deferred','deploy-acknowledged','retire-deadline']) {
    const value={kind,challenge:randomUUID(),timerDelayMs:kind==='retire-deadline'?20000:2500},directory=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-retained-generation-'));report.cases.push(value);let backend,client,observer;
    const notes=[],observerSockets=new Set();
    try {
        observer=http.createServer((request,response)=>{
            notes.push({method:request.method,url:request.url,headers:request.headers,remoteAddress:request.socket.remoteAddress});response.end('ack');
        });
        observer.on('connection',socket=>{observerSockets.add(socket);socket.once('close',()=>observerSockets.delete(socket));});
        await new Promise(resolve=>observer.listen(0,'127.0.0.1',resolve));
        const observerPort=observer.address().port,noteURL='http://127.0.0.1:'+observerPort+'/resumed?'+new URLSearchParams({version:'v1',challenge:value.challenge});
        const bundle=path.join(directory,'source.mjs');
        const source=version=>`const version=${JSON.stringify(version)};let state=null;
export default {fetch(request){const url=new URL(request.url);
 if(url.pathname.endsWith('/state'))return Response.json({version,state});
 if(!url.pathname.endsWith('/ws'))return new Response(version);
 const pair=new WebSocketPair(),server=pair[1];server.accept();
 const value=state={started:false,closed:0,resumed:false,localBefore:null,localAfter:null,failure:null};
 server.onclose=e=>{value.closed++;value.close={code:e.code,reason:e.reason,wasClean:e.wasClean,before:server.readyState};${kind==='deploy-acknowledged'?"server.close(1000,'local');value.close.after=server.readyState;":''}};
 server.onmessage=async()=>{value.started=true;server.send('started');await new Promise(resolve=>setTimeout(resolve,${value.timerDelayMs}));
 value.resumed=true;try{const response=await fetch(${JSON.stringify(noteURL)});value.note=await response.text();}catch(error){value.failure={name:error.name,message:error.message};}
 value.localBefore=server.readyState;server.close(1000,'local');value.localAfter=server.readyState;};
 return new Response(null,{status:101,webSocket:pair[0]});}};`;
        fs.writeFileSync(bundle,source('v1'));
        const allow=['127.0.0.1:'+observerPort];
        backend=await startCapsid(host,worker,bundle,directory,'managed',{permissions:{modules:[],fetch:{allow}},fetchTargets:allow,requestTimeoutMs:10000,hostCapacity:{workersTotal:1,activationSurgeWorkers:1}});
        const v2=path.join(directory,'apps/orders/v2');fs.mkdirSync(v2,{recursive:true});fs.writeFileSync(path.join(v2,'bundle.mjs'),source('v2'));fs.copyFileSync(path.join(directory,'apps/orders/v1/capsid.json'),path.join(v2,'capsid.json'));
        const request=async target=>{const response=await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders${target}`,{signal:AbortSignal.timeout(4000)});return {status:response.status,text:await response.text()};};
        const state=async()=>{const response=await request('/state');assert.equal(response.status,200);return JSON.parse(response.text);};
        const children=()=>{
            const result=new Set();for(const task of fs.readdirSync(`/proc/${backend.child.pid}/task`))try{for(const pid of fs.readFileSync(`/proc/${backend.child.pid}/task/${task}/children`,'utf8').trim().split(/\s+/))if(pid)result.add(Number(pid));}catch(error){if(error.code!=='ENOENT')throw error;}
            return [...result];
        };
        const metrics=async()=>{
            const response=await backend.admin('/metrics');assert.equal(response.status,200);const result={};
            for(const [field,name] of [['channels','channel_reservations'],['active','channel_active'],['dials','channel_pending_dials'],['http','channel_http_inflight']]) {
                const lines=response.text.split('\n').filter(line=>line.startsWith('capsid_'+name+'{scope="service",'));assert.equal(lines.length,1);result[field]=Number(lines[0].split(' ').at(-1));
            }return result;
        };
        value.before=await state();value.originalWorkers=await until(children,list=>list.length===1,'one original worker');
        client=responseSession(backend.port,'/@capsid/orders/ws',{observeCloseReply:true,timeoutMs:15000});value.header=await client.headers;assert.equal(value.header.status,101);
        const started=performance.now();client.send('go');assert.equal(await client.text(),'started');client.dispose();try{await client.complete;}catch(error){value.wire=error.observation;}
        value.afterEOF=await until(state,document=>document.state?.closed===1,'actual abnormal Close callback');
        assert.equal(value.afterEOF.state.resumed,false,'timer has not yet fired');
        value.noTransportResources=await until(metrics,result=>!result.channels&&!result.active&&!result.dials&&!result.http,'all transport and HTTP reservations released');
        assert.equal(notes.length,0);
        if(kind!=='steady') {
            const began=performance.now();const submitted=kind==='retire-deadline'?await backend.admin('/v1/apps/orders/retire',undefined,'POST'):await backend.admin('/v1/deploy',{app:'orders',version:'v2'});assert.equal(submitted.status,202,submitted.text);
            value.operation=await until(async()=>{const result=await backend.admin('/v1/operations/'+submitted.json.operationId);assert.equal(result.status,200);return result.json;},result=>['active','failed'].includes(result.state),'deploy operation',15000);
            assert.equal(value.operation.state,'active',JSON.stringify(value.operation));
            value.newResponse=await request('/version');if(kind==='retire-deadline')assert.equal(value.newResponse.status,404);else assert.deepEqual(value.newResponse,{status:200,text:'v2'});
            value.workersAfterActivation=children();value.activationElapsedMs=performance.now()-started;
            assert.ok(value.activationElapsedMs<1800,'activation or retirement is acknowledged before the held native timer');
            if(kind==='retire-deadline'){await until(children,list=>!value.originalWorkers.some(pid=>list.includes(pid)),'retained context obeys original drain deadline',14000);value.drainElapsedMs=performance.now()-began;}
        }
        await delay(Math.max(0,started+3500-performance.now()));
        value.notes=structuredClone(notes);value.workersAfterTimer=children();
        value.oldWorkerStillPresent=value.originalWorkers.some(pid=>value.workersAfterTimer.includes(pid));
        if(kind==='steady')value.finalState=await state();else value.finalVersion=await request('/version');
        value.noFinalTransportResources=await metrics();
        value.compatible=kind==='retire-deadline'?notes.length===0&&!value.oldWorkerStillPresent&&value.drainElapsedMs>=9000&&value.drainElapsedMs<14000:kind==='steady'?notes.length===1&&value.finalState.state.localAfter===3:kind==='deploy-acknowledged'?notes.length===0&&!value.oldWorkerStillPresent:notes.length===1&&!value.oldWorkerStillPresent;
    }catch(error){value.failure=error.stack;}
    finally {
        client?.dispose();if(backend){value.shutdown=await stopCapsid(backend.child);value.stderr=backend.stderr();}
        for(const socket of observerSockets)socket.destroy();if(observer)await new Promise(resolve=>observer.close(resolve));save();
    }
}
report.complete=report.cases.every(value=>!value.failure&&value.shutdown.code===0&&!value.shutdown.forced);save();
console.log(JSON.stringify({complete:report.complete,cases:report.cases.map(({kind,compatible,notes,oldWorkerStillPresent,activationElapsedMs,failure})=>({kind,compatible,notes:notes?.length,oldWorkerStillPresent,activationElapsedMs,failure}))}));
assert.equal(report.complete,true,'all four managed cases collected and shut down normally');
assert.deepEqual(report.cases.map(value=>value.kind),['steady','deploy-deferred','deploy-acknowledged','retire-deadline']);
for(const value of report.cases){
    assert.equal(value.compatible,true,value.kind+' retains/cancels exactly its own local I/O context');
    assert.deepEqual(value.noTransportResources,{channels:0,active:0,dials:0,http:0});
    assert.deepEqual(value.noFinalTransportResources,value.noTransportResources);
    assert.equal(value.wire.completion,'client-disposed');assert.equal(value.wire.peerTerminated,false);
    assert.equal(value.afterEOF.state.close.code,1006);assert.equal(value.afterEOF.state.closed,1);assert.equal(value.afterEOF.state.resumed,false);
    assert.equal(value.notes.length,['steady','deploy-deferred'].includes(value.kind)?1:0);
    for(const note of value.notes){const url=new URL(note.url,'http://fixture');assert.equal(url.pathname,'/resumed');assert.equal(url.searchParams.get('version'),'v1');assert.equal(url.searchParams.get('challenge'),value.challenge);}
    if(['deploy-deferred','retire-deadline'].includes(value.kind))assert.ok(value.originalWorkers.every(pid=>value.workersAfterActivation.includes(pid)),'old generation stays alive after route change');
    if(value.kind==='steady'){assert.equal(value.finalState.state.localBefore,2);assert.equal(value.finalState.state.localAfter,3);assert.equal(value.finalState.state.failure,null);}
    assert.doesNotMatch(value.stderr,/WORKER POISONED|terminal continuation leak|ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:/);
}
assert.deepEqual({host:sha(host),worker:sha(worker)},report.binaries,'tested binaries are unchanged');
