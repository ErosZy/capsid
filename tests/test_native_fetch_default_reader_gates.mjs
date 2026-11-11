import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash,randomUUID} from 'node:crypto';
import {createGzip,constants} from 'node:zlib';
import {parseArgs,isDeepStrictEqual} from 'node:util';
import {pathToFileURL,fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {assertGatedDefaultReaderState as validate, assertGatedDefaultReaderReport} from '../tools/native-fetch-default-reader-gates-contract.mjs';

const {values:args}=parseArgs({options:Object.fromEntries(['source','host','worker','reference','fixture','report','transport'].map(name=>[name,{type:'string'}]))});
assert.ok(['ordinary','upgrade'].includes(args.transport),'explicit ordinary or Upgrade fallback transport');
args.source ??= path.resolve(fileURLToPath(new URL('..',import.meta.url)));
args.fixture ??= path.join(args.source,'tests/fixtures/native-fetch-default-reader-gates.mjs');
args.reference ??= process.env.CAPSID_WORKERD_REF;
args.report ??= path.resolve(process.env.CAPSID_WS_REPORT_DIR ?? process.cwd(),'native-default-reader-gates-'+args.transport+'.json');
for(const key of ['source','host','worker','reference','fixture','report']) assert.ok(args[key]&&path.isAbsolute(args[key]),key);
const {startCapsid,stopCapsid}=await import(pathToFileURL(path.join(args.source,'tests/helpers/ws_host_backend.mjs')));
const hash=file=>createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const windows=process.platform==='win32';
const lockFile=path.join(args.source,'tests/fixtures/workerd-ws',windows?'reference-lock-windows.json':'reference-lock.json');
const lock=JSON.parse(fs.readFileSync(lockFile));assert.equal(hash(args.reference),lock.binary_sha256);assert.equal(process.version,'v24.14.0');
assert.ok(!fs.existsSync(args.report));fs.mkdirSync(path.dirname(args.report),{recursive:true});
const artifact=path.join(path.dirname(args.report),path.basename(args.report)+'.runs');fs.mkdirSync(artifact);
const measured=[args.host,args.worker,args.reference,args.fixture,lockFile,fileURLToPath(import.meta.url),
    path.join(args.source,'tests/helpers/ws_host_backend.mjs'),fileURLToPath(new URL('../tools/native-fetch-default-reader-gates-contract.mjs',import.meta.url))];
const inputs=Object.fromEntries(measured.map(file=>[file,hash(file)]));
const plan=[7,4089,...Array(66).fill(4096),17],total=plan.reduce((a,b)=>a+b,0);
const report={schema:'capsid-native-default-reader-gates-v1',complete:false,status:'running',functionalQualification:true,performanceQualification:false,transport:args.transport,
    client:{node:process.version,undici:process.versions.undici,sha256:hash(process.execPath)},inputs,referenceLock:lock,
    partPlan:plan,totalBodyBytes:total,phaseDeadlineMilliseconds:2000,originalRequestDeadlineMilliseconds:30000,
    programIdenticalAcrossNativeAndPrimary:true,sourceEndWithheldUntilLastReadAcknowledgment:true,cases:[]};
const save=()=>fs.writeFileSync(args.report,JSON.stringify(report,null,2)+'\n');save();
async function poll(read,accept,milliseconds){const until=performance.now()+milliseconds;for(;;){const value=await read();if(accept(value))return value;assert.ok(performance.now()<until,'poll deadline: '+JSON.stringify(value));await delay(5);}}
async function port(){const server=net.createServer();await new Promise(r=>server.listen(0,'127.0.0.1',r));const value=server.address().port;await new Promise(r=>server.close(r));return value;}
async function stopReference(backend){let timer,forced=false;if(backend.child.exitCode===null&&backend.child.signalCode===null)backend.child.kill('SIGTERM');await Promise.race([backend.closed,new Promise(resolve=>{timer=setTimeout(()=>{forced=true;backend.child.kill('SIGKILL');resolve();},2000);})]);clearTimeout(timer);await backend.closed;return{code:backend.child.exitCode,signal:backend.child.signalCode,forced};}
async function reference(directory){
    const listening=await port(),config=path.join(directory,'reference.capnp');
    fs.writeFileSync(config,`using Workerd = import "/workerd/workerd.capnp"; const config :Workerd.Config =
        (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},
        modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],
        sockets=[(name="http",address="127.0.0.1:${listening}",http=(),service="main")]);`);
    const child=spawn(args.reference,['serve',config],{stdio:['ignore','pipe','pipe']});let stderr='';child.stdout.resume();child.stderr.on('data',data=>stderr+=data);
    const backend={child,port:listening,stderr:()=>stderr,closed:new Promise(resolve=>child.once('close',resolve))};
    try{await poll(async()=>{assert.ok(child.exitCode===null&&child.signalCode===null,stderr);try{const r=await fetch(`http://127.0.0.1:${listening}/health`,{signal:AbortSignal.timeout(500)});return r.status===200&&await r.text()==='healthy';}catch{return false;}},Boolean,10000);return backend;}
    catch(error){await stopReference(backend);throw error;}
}
async function peer(token,bodyKind){
    let active=null;const sockets=new Set();
    const respond=(socket,text,status=200)=>socket.end(`HTTP/1.1 ${status} ${status===200?'OK':'Error'}\r\nContent-Length: ${Buffer.byteLength(text)}\r\nConnection: close\r\n\r\n${text}`);
    function arm(state,index){clearTimeout(state.timer);state.timer=setTimeout(()=>{
        state.phaseTimedOut=index;state.events.push({kind:'phase-timeout',part:index});state.dataSocket.destroy();
    },2000);}
    async function emit(state,index){
        state.phase=index;state.sent=false;const size=plan[index],bytes=Buffer.alloc(size);
        for(let i=0;i<size;i++)bytes[i]=((state.produced+i)*29+17)%251;
        state.produced+=size;state.events.push({kind:'emit',part:index,bytes:size});
        if(state.compressor){
            state.compressor.write(bytes);await new Promise((resolve,reject)=>state.compressor.flush(constants.Z_SYNC_FLUSH,error=>error?reject(error):resolve()));
            const wire=Buffer.concat(state.compressed);state.compressed=[];state.dataSocket.write(wire);
        }else state.dataSocket.write(bytes);
        state.sent=true;arm(state,index);
    }
    const server=net.createServer(socket=>{
        socket.setNoDelay(true);sockets.add(socket);const state=active;
        if(state){state.openSockets++;state.connections++;}
        socket.on('error',()=>{});socket.on('close',()=>{sockets.delete(socket);if(state)state.openSockets--;});
        let received=Buffer.alloc(0),dispatched=false;
        socket.on('data',async bytes=>{
            if(dispatched)return;received=Buffer.concat([received,bytes]);
            if(!received.includes('\r\n\r\n'))return;dispatched=true;
            try{
                const match=/^GET ([^ ]+) HTTP\/1\.[01]\r\n/.exec(received.toString('ascii'));assert.ok(match,'one HTTP GET');
                const url=new URL(match[1],'http://origin');assert.ok(state&&url.searchParams.get('token')===token,'source token');
                if(url.pathname==='/data'){
                    assert.ok(!state.dataSocket,'one source');state.dataSocket=socket;state.events.push({kind:'head'});
                    socket.on('close',()=>{clearTimeout(state.timer);state.dataResponseClosed=true;state.events.push({kind:'response-close'});if(!state.eof)state.compressor?.destroy();});
                    // Close-delimited HTTP deliberately has no per-part chunk framing.
                    // A separate readiness acknowledgment proves the header was parsed
                    // before the first body packet is made available.
                    socket.write('HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n'+
                        (bodyKind==='gzip'?'Content-Encoding: gzip\r\n':'')+'\r\n');
                    if(bodyKind==='gzip'){
                        state.compressed=[];state.compressor=createGzip();
                        state.compressor.on('data',data=>state.compressed.push(Buffer.from(data)));
                        state.compressor.on('error',error=>state.compressorError=String(error));
                    }
                    arm(state,-1);
                }else if(url.pathname==='/ready'){
                    assert.ok(state.dataSocket&&!state.ready);state.ready=true;clearTimeout(state.timer);
                    state.events.push({kind:'headers-observed-before-first-byte'});await emit(state,0);respond(socket,'ready');
                }else if(url.pathname==='/ack'){
                    const index=Number(url.searchParams.get('part'));assert.equal(index,state.phase);assert.ok(state.sent);
                    clearTimeout(state.timer);state.acks.push(index);state.events.push({kind:'ack',part:index});
                    if(index+1===plan.length){
                        state.eof=true;state.events.push({kind:'eof'});
                        if(state.compressor){await new Promise((resolve,reject)=>{state.compressor.once('end',resolve);state.compressor.once('error',reject);state.compressor.end();});state.dataSocket.end(Buffer.concat(state.compressed));state.compressed=[];}
                        else state.dataSocket.end();
                    }else await emit(state,index+1);
                    respond(socket,'ack:'+index);
                }else respond(socket,'missing',404);
            }catch(error){if(state){state.peerError=error.stack;state.dataSocket?.destroy();}socket.destroy();}
        });
    });
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const origin=`http://127.0.0.1:${server.address().port}`;
    return{origin,authority:`127.0.0.1:${server.address().port}`,
        reset:()=>{active={phase:-1,produced:0,sent:false,eof:false,ready:false,acks:[],events:[],connections:0,openSockets:0};return active;},
        snapshot:state=>structuredClone({phase:state.phase,produced:state.produced,eof:state.eof,ready:state.ready,acks:state.acks,events:state.events,
            connections:state.connections,openSockets:state.openSockets,dataResponseClosed:state.dataResponseClosed??false,
            phaseTimedOut:state.phaseTimedOut??null,peerError:state.peerError??null,compressorError:state.compressorError??null}),
        stop:async()=>{clearTimeout(active?.timer);active?.dataSocket?.destroy();active?.compressor?.destroy();for(const socket of sockets)socket.destroy();await new Promise(resolve=>server.close(resolve));}};
}
for(const bodyKind of ['raw','gzip'])for(const mode of ['stream','cancel-after-first','buffer-all-control']){
    const token=randomUUID(),entry={bodyKind,mode,token,failure:null,roles:{}};report.cases.push(entry);save();let upstream;
    try{
        upstream=await peer(token,bodyKind);
        for(const role of ['primary','native']){
            const row=entry.roles[role]={failure:null},state=upstream.reset();const directory=path.join(artifact,`${bodyKind}-${mode}-${role}`);fs.mkdirSync(directory);
            const fixture=path.join(directory,'main.mjs');fs.copyFileSync(args.fixture,fixture);assert.equal(hash(fixture),inputs[args.fixture]);let backend;
            try{
                backend=role==='primary'?await reference(directory):await startCapsid(args.host,args.worker,fixture,directory,'single-worker',
                    {permissions:{modules:[],fetch:{allow:[upstream.authority]}},fetchTargets:[upstream.authority],requestTimeoutMs:30000});
                const prefix=role==='primary'?'':'/@capsid/orders';
                const query=new URLSearchParams({origin:upstream.origin,token,body:bodyKind,mode,transport:args.transport});
                row.probeQuery=query.toString();row.programSHA256=hash(fixture);
                const response=await fetch(`http://127.0.0.1:${backend.port}${prefix}/probe?${query}`,{signal:AbortSignal.timeout(30000)});
                row.httpStatus=response.status;row.rawResponse=await response.text();assert.equal(response.status,200,row.rawResponse);row.publicState=JSON.parse(row.rawResponse);
                await poll(()=>upstream.snapshot(state),value=>value.openSockets===0,2000);row.sourceBeforeBackendStop=upstream.snapshot(state);
                const healthy=await fetch(`http://127.0.0.1:${backend.port}${prefix}/health`,{signal:AbortSignal.timeout(4000)});row.healthyReuse={status:healthy.status,text:await healthy.text()};
                assert.deepEqual(row.healthyReuse,{status:200,text:'healthy'});validate(row.publicState,mode,row.sourceBeforeBackendStop,token);
            }catch(error){row.failure=error.stack;}
            finally{
                row.sourceBeforeCleanup=upstream.snapshot(state);
                if(backend){try{row.shutdown=role==='primary'?await stopReference(backend):await stopCapsid(backend.child);assert.equal(row.shutdown.forced,false);if(role==='native')assert.equal(row.shutdown.code,0);}catch(error){row.cleanupFailure=error.stack;}row.stderr=backend.stderr();}
                row.sourceAfterBackendStop=upstream.snapshot(state);save();
            }
            assert.equal(row.failure,null);assert.equal(row.cleanupFailure,undefined);assert.ok(!/ERROR: AddressSanitizer|LeakSanitizer:|runtime error:|AddressSanitizer:DEADLYSIGNAL/.test(row.stderr));
        }
        assert.equal(entry.roles.primary.probeQuery,entry.roles.native.probeQuery);assert.equal(entry.roles.primary.programSHA256,entry.roles.native.programSHA256);
        if(mode!=='buffer-all-control'){assert.ok(isDeepStrictEqual(entry.roles.primary.publicState,entry.roles.native.publicState),'full public state equality');entry.fullPublicStatesMatch=true;}
        else entry.actualWholeBodyWaitControlRejectedBothRuntimes=true;
    }catch(error){entry.failure=error.stack;}
    finally{if(upstream)await upstream.stop();save();}
}
assert.deepEqual(inputs,Object.fromEntries(measured.map(file=>[file,hash(file)])));
report.complete=report.cases.length===6&&report.cases.every(value=>!value.failure);report.status=report.complete?'complete':'complete-with-failures';
if(report.complete){try{report.contract=assertGatedDefaultReaderReport(report,{transport:args.transport,fixtureSHA256:inputs[args.fixture]});}catch(error){report.contractFailure=error.stack;report.complete=false;report.status='contract-failed';}}
save();
console.log(JSON.stringify({complete:report.complete,cases:report.cases.length,fullPublicMatches:report.cases.filter(c=>c.fullPublicStatesMatch).length,
    actualWholeBodyWaitControls:report.cases.filter(c=>c.actualWholeBodyWaitControlRejectedBothRuntimes).length,bodyBytes:total,readGates:plan.length}));
assert.ok(report.complete,'all gated DefaultReader cases must pass; see raw report');
