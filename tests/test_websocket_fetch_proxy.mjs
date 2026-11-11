import assert from 'node:assert/strict';import fs from 'node:fs';import path from 'node:path';import os from 'node:os';import net from 'node:net';
import {spawn} from 'node:child_process';import {randomUUID,createHash} from 'node:crypto';import {fileURLToPath,pathToFileURL} from 'node:url';import {setTimeout as delay} from 'node:timers/promises';
import {root as R,readLock,verifyClient,verifyBinary,fileDigest as sha} from '../tools/workerd-reference.mjs';
import {fetchProxyPeer} from './helpers/ws_fetch_proxy_peer.mjs';
import {fetchProxySession} from './helpers/ws_fetch_proxy_client.mjs';
import {startCapsid,stopCapsid} from './helpers/ws_host_backend.mjs';
import {fetchProxyCases,fetchProxyInputPaths,validateFetchProxyReport} from '../tools/workerd-fetch-proxy-contract.mjs';
const options=new Map();for(let i=2;i<process.argv.length;i+=2){assert.ok(['--mode','--host','--worker','--report','--reference-from-env'].includes(process.argv[i])&&process.argv[i+1]&&!options.has(process.argv[i]));options.set(process.argv[i],process.argv[i+1]);}
const selector=options.get('--reference-from-env');assert.ok(selector===undefined||selector==='1');
const reference=selector==='1',mode=reference?'reference':options.get('--mode');
assert.ok(['reference','single-worker','static-pool','managed'].includes(mode));
assert.ok(reference||mode!=='reference','product cannot select the reference role');
assert.ok(!reference||!options.has('--host')&&!options.has('--worker')&&!options.has('--mode'));
const host=reference?process.env.CAPSID_WORKERD_REF:options.get('--host'),worker=options.get('--worker');
assert.ok(host&&(reference||worker));const lock=readLock();verifyClient(lock);if(reference)verifyBinary(host,lock);
const runId=process.env.CAPSID_WS_RUN_ID??randomUUID();
const destination=options.get('--report')??path.join(process.env.CAPSID_WS_REPORT_DIR??os.tmpdir(),
    (process.env.CAPSID_WS_REPORT_DIR?'fetch-proxy-':'capsid-fetch-proxy-'+runId+'-')+mode+'.json');
assert.ok(!fs.existsSync(destination));fs.mkdirSync(path.dirname(destination),{recursive:true});
const fixture=path.join(R,fetchProxyInputPaths.fixture);
const inputPaths=Object.fromEntries(Object.entries(fetchProxyInputPaths).map(([key,file])=>[key,path.join(R,file)]));
const inputs=Object.fromEntries(Object.entries(inputPaths).map(([key,file])=>[key,sha(file)]));
const binaries={host:sha(host),worker:reference?null:sha(worker)};
const report={schema:'capsid-fetch-proxy-v1',complete:false,mode,runId,referenceLock:lock,
    client:{node:process.version,undici:process.versions.undici},inputs,binaries,cases:[]};
const save=()=>fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');save();
async function until(read,condition,label,timeout=5000){const deadline=performance.now()+timeout;for(;;){const value=await read();if(condition(value))return value;assert.ok(performance.now()<deadline,label+': '+JSON.stringify(value));await delay(10);}}
async function stopReference(backend){let timer,forced=false;if(backend.child.exitCode===null&&backend.child.signalCode===null)backend.child.kill('SIGTERM');try{await Promise.race([backend.closed,new Promise(resolve=>{timer=setTimeout(()=>{forced=true;backend.child.kill('SIGKILL');resolve();},2000);})]);await backend.closed;}finally{clearTimeout(timer);}return {code:backend.child.exitCode,signal:backend.child.signalCode,forced};}
async function startReference(directory){
    const listener=net.createServer();await new Promise(resolve=>listener.listen(0,'127.0.0.1',resolve));const port=listener.address().port;await new Promise(resolve=>listener.close(resolve));
    fs.copyFileSync(fixture,path.join(directory,'main.mjs'));const config=path.join(directory,'config.capnp');fs.writeFileSync(config,`using Workerd = import "/workerd/workerd.capnp"; const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child=spawn(host,['serve',config],{stdio:['ignore','pipe','pipe']});let stderr='';child.stdout.resume();child.stderr.on('data',data=>stderr+=data);const backend={child,port,stderr:()=>stderr,closed:new Promise(resolve=>child.once('close',resolve))};
    try{await until(async()=>{try{const response=await fetch(`http://127.0.0.1:${port}/state`,{signal:AbortSignal.timeout(500)});return {status:response.status,text:await response.text()};}catch(error){assert.ok(child.exitCode===null&&child.signalCode===null,stderr+error);return null;}},value=>value?.status===200,'native reference ready',10000);return backend;}catch(error){await stopReference(backend);throw error;}
}
const kinds=fetchProxyCases;
for(const kind of kinds){
    const value={kind,challenge:randomUUID(),target:null,initial:null,headers:null,echo:null,early:null,earlyBinary:null,
        expectedBinary:null,actualBinary:null,wire:null,secondHeaders:null,secondWire:null,parked:null,mark:null,
        checkpoint:null,checkpointUpstream:null,wake:null,postWake:null,foreign:null,pauseCheckpoint:null,
        upstreamAfterClientEnd:null,observed:null,upstreamBeforeStop:null,shutdown:null,stderr:null,
        upstreamAfterStop:null,failure:null,cleanupFailure:null},directory=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-native-proxy-'));report.cases.push(value);let backend,upstream,client,second;
    try{
        upstream=await fetchProxyPeer(kind,value.challenge);value.target={host:'localhost',port:upstream.port};const allow=['localhost:'+upstream.port];backend=reference?await startReference(directory):await startCapsid(host,worker,fixture,directory,mode,{permissions:{modules:[],fetch:{allow}},fetchTargets:allow});
        const prefix=reference?'':'/@capsid/orders',target=`http://localhost:${upstream.port}/proxy?challenge=${value.challenge}`,query=new URLSearchParams({kind,target,challenge:value.challenge});
        const normal=async suffix=>{const response=await fetch(`http://127.0.0.1:${backend.port}${prefix}${suffix}`,{signal:AbortSignal.timeout(4000)});return {status:response.status,text:await response.text()};};
        value.initial=await normal('/state');
        if(kind==='cross-origin'){value.parked=await normal('/probe?'+new URLSearchParams({kind:'park',target}));assert.deepEqual(value.parked,{status:200,text:'parked'});}
        client=fetchProxySession(backend.port,prefix+'/probe?'+query,{timeoutMs:15000,observeCloseReply:true});value.headers=await client.headers;
        if(value.headers.status===101&&!['accepted','cross-origin'].includes(kind)){
            if(kind==='foreign-methods')value.foreign=await normal('/foreign');
            if(kind.startsWith('source-')){
                value.mark=await normal('/mark');await delay(kind==='source-live-sibling'?1600:750);value.checkpoint=await normal('/state');value.checkpointUpstream=upstream.snapshot();
                if(kind==='source-parked-promise'){value.wake=await normal('/wake');await delay(50);value.postWake=await normal('/state');}
            }
            if(kind==='upstream-first')value.early=await client.text();
            if(kind==='upstream-first-binary'){const bytes=await client.binary(),expected=Buffer.alloc(180000);for(let i=0;i<expected.length;++i)expected[i]=(i*17+value.challenge.charCodeAt(i%value.challenge.length))%251;assert.deepEqual(bytes,expected);value.earlyBinary={bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')};}
            if(['large-binary','slow-upstream'].includes(kind)){
                const payloads=Array.from({length:kind==='slow-upstream'?12:1},(_,message)=>{const bytes=Buffer.alloc(180000);for(let index=0;index<bytes.length;++index)bytes[index]=(index*17+message*31+value.challenge.charCodeAt(index%value.challenge.length))%251;return bytes;});
                value.expectedBinary=payloads.map(bytes=>({bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')}));for(const bytes of payloads)client.sendBinary(bytes);
                if(kind==='slow-upstream'){await delay(250);value.pauseCheckpoint=upstream.snapshot();upstream.resume();}
                value.actualBinary=[];for(const expected of payloads){const bytes=await client.binary();assert.deepEqual(bytes,expected);value.actualBinary.push({bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')});}
            }else{client.send(value.challenge);value.echo=await client.text();}
            if(kind==='client-abrupt'){client.dispose();try{await client.complete;}catch(error){value.wire=error.observation;}value.upstreamAfterClientEnd=await until(()=>upstream.snapshot(),records=>records.length===1&&(records[0].peerEnded||records[0].peerReset),'upstream ended before service shutdown');}
            else if(kind==='upstream-abrupt'){upstream.abort();value.wire=await client.complete;}
            else if(kind==='server-close'){upstream.close();value.wire=await client.complete;}
            else{client.close();value.wire=await client.complete;}
            if(kind==='reuse'){
                second=fetchProxySession(backend.port,prefix+'/probe?'+query,{timeoutMs:5000,observeCloseReply:true});value.secondHeaders=await second.headers;value.secondWire=await second.complete;
            }
        }else value.wire=await client.complete;
        value.observed=await normal('/state');
        if(!['accepted','upstream-abrupt'].includes(kind))
            await until(()=>upstream.snapshot(),records=>records.every(item=>item.peerEnded||item.peerReset),
                'actual upstream EOF before local service shutdown');
    }catch(error){value.failure=error.stack;if(error.observation)value.failureObservation=error.observation;}
    finally{
        value.upstreamBeforeStop=upstream?.snapshot();client?.dispose();second?.dispose();
        if(backend){try{value.shutdown=reference?await stopReference(backend):await stopCapsid(backend.child);
            if(upstream)await until(()=>upstream.snapshot(),records=>records.every(item=>item.peerEnded||item.peerReset||item.fixtureDestroyed),
                'actual source EOF before fixture disposal');
        }catch(error){value.cleanupFailure=error.stack;}value.stderr=backend.stderr();}
        value.upstreamAfterStop=upstream?.snapshot();if(upstream)await upstream.stop();save();
    }
}
assert.deepEqual(inputs,Object.fromEntries(Object.entries(inputPaths).map(([key,file])=>[key,sha(file)])));assert.equal(sha(host),report.binaries.host);if(!reference)assert.equal(sha(worker),report.binaries.worker);
report.complete=report.cases.every(value=>!value.failure&&!value.cleanupFailure&&value.shutdown&&!value.shutdown.forced);save();
console.log(JSON.stringify({collectionComplete:report.complete,mode,cases:report.cases.length}));
validateFetchProxyReport(report,{runId,lock,inputs,...binaries});
console.log(JSON.stringify({complete:true,mode,cases:report.cases.length}));
