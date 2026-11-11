import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import net from 'node:net';
import tls from 'node:tls';
import {spawn,execFile,execFileSync} from 'node:child_process';
import {promisify} from 'node:util';
import {randomUUID} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import {startCapsid,stopCapsid} from './helpers/ws_host_backend.mjs';
import {readLock,verifyClient,verifyBinary,fileDigest} from '../tools/workerd-reference.mjs';
import {validateInteropReport,interopCases} from '../tools/workerd-interop-contract.mjs';
import {startNodePeer} from './helpers/ws_interop_node_peer.mjs';
import {loadNodeWS} from './helpers/ws_interop_node_sdk.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const args=new Map();for(let i=2;i<process.argv.length;i+=2)args.set(process.argv[i],process.argv[i+1]);
const reference=args.get('--workerd')??(args.has('--reference-from-env')?process.env.CAPSID_WORKERD_REF:undefined);
const mode=reference?'reference':args.get('--mode')??'single-worker';
if(args.has('--reference-from-env')&&!reference)throw new Error('CAPSID_WORKERD_REF is required for independent-client acceptance');
const python=args.get('--python')??process.env.CAPSID_WS_INTEROP_PYTHON??'python3';
if(args.has('--node-package'))process.env.CAPSID_WS_INTEROP_NODE_PACKAGE=args.get('--node-package');
const runId=process.env.CAPSID_WS_RUN_ID??randomUUID();
const destination=args.get('--report')??(process.env.CAPSID_WS_REPORT_DIR?path.join(process.env.CAPSID_WS_REPORT_DIR,'interop-'+mode+'.json'):undefined);
if(destination)fs.writeFileSync(destination,JSON.stringify({complete:false,status:'running',runId,mode})+'\n');
const lock=readLock();verifyClient(lock);if(reference)verifyBinary(reference,lock);
const inputPaths={runner:'tests/test_websocket_interop.mjs',pythonProgram:'tests/helpers/ws_interop_python.py',
    nodeProgram:'tests/helpers/ws_interop_node.mjs',fixture:'tests/fixtures/websocket-interop.mjs',
    clientLock:'tests/fixtures/websocket-interop-lock.json',contract:'tools/workerd-interop-contract.mjs',
    hostBackend:'tests/helpers/ws_host_backend.mjs',nodeSDK:'tests/helpers/ws_interop_node_sdk.mjs',nodePeer:'tests/helpers/ws_interop_node_peer.mjs'};
const inputs=Object.fromEntries(Object.entries(inputPaths).map(([name,p])=>[name,fileDigest(path.join(root,p))]));
const exec=promisify(execFile);
const pythonProgram=path.join(root,inputPaths.pythonProgram),nodeProgram=path.join(root,inputPaths.nodeProgram);
const clientLock=JSON.parse(fs.readFileSync(path.join(root,inputPaths.clientLock),'utf8'));
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-interop-'));
const listeners=[],sockets=new Set(),cases=[];
let backend,peer,peerStopped,nodePeer,nodeStopped;
async function waitFor(predicate,message) {
    const deadline=performance.now()+6000;
    while(!predicate()){assert.ok(performance.now()<deadline,message);await delay(10);}
}
async function runJson(binary,parameters,env={}) {
    const result=await exec(binary,parameters,{env:{...process.env,...env},timeout:12000,maxBuffer:4*1024*1024});
    return JSON.parse(result.stdout);
}
function spawnPeer(parameters) {
    const child=spawn(python,['-I',pythonProgram,'peer',...parameters],{stdio:['pipe','pipe','pipe']});
    const queue=[],waiters=[];let buffer='',stderr='';
    child.stderr.setEncoding('utf8');child.stderr.on('data',c=>stderr+=c);
    const fail=error=>{for(const pending of waiters.splice(0)){clearTimeout(pending.timer);pending.reject(error);}};
    child.stdout.setEncoding('utf8');child.stdout.on('data',chunk=>{
        buffer+=chunk;
        try {while(buffer.includes('\n')) {
            const end=buffer.indexOf('\n'),value=JSON.parse(buffer.slice(0,end));buffer=buffer.slice(end+1);
            if(waiters.length){const pending=waiters.shift();clearTimeout(pending.timer);pending.resolve(value);}else queue.push(value);
        }}catch(error){fail(error);}
    });
    child.once('error',fail);child.once('exit',(code,signal)=>{if(code!==0||signal)fail(new Error(`Python peer exit ${code}/${signal}: ${stderr}`));});
    return {child,stderr:()=>stderr,send:value=>child.stdin.write(JSON.stringify(value)+'\n'),
        next:()=>queue.length?Promise.resolve(queue.shift()):new Promise((resolve,reject)=>{
            const pending={resolve,reject};pending.timer=setTimeout(()=>{waiters.splice(waiters.indexOf(pending),1);reject(new Error('Python peer reply deadline: '+stderr));},10000);waiters.push(pending);
        })};
}
async function listen(server) {
    listeners.push(server);server.on('error',error=>console.error(error));
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
    return server.address().port;
}
function track(socket) {sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});return socket;}
async function proxy(port,tlsOptions) {
    let accepted=0;
    const connect=socket=>{
        track(socket);const upstream=track(net.connect(port,'127.0.0.1'));
        socket.pipe(upstream).pipe(socket);socket.on('close',()=>upstream.destroy());upstream.on('close',()=>socket.destroy());
    };
    const server=tlsOptions?tls.createServer(tlsOptions,connect):net.createServer(connect);
    server.on('connection',socket=>{accepted++;if(tlsOptions)track(socket);});if(tlsOptions)server.on('tlsClientError',()=>{});
    return {port:await listen(server),accepted:()=>accepted};
}
function certificates() {
    const ca=path.join(dir,'ca.pem'),key=path.join(dir,'key.pem'),cert=path.join(dir,'cert.pem');
    const command=parameters=>execFileSync(args.get('--openssl') ?? 'openssl',parameters,{stdio:'ignore'});
    command(['req','-x509','-newkey','rsa:2048','-nodes','-keyout',path.join(dir,'ca.key'),'-out',ca,'-days','1','-subj','/CN=Capsid interop CA']);
    command(['req','-newkey','rsa:2048','-nodes','-keyout',key,'-out',path.join(dir,'cert.csr'),'-subj','/CN=localhost']);
    const ext=path.join(dir,'extensions');fs.writeFileSync(ext,'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n');
    command(['x509','-req','-in',path.join(dir,'cert.csr'),'-CA',ca,'-CAkey',path.join(dir,'ca.key'),'-CAcreateserial','-out',cert,'-days','1','-extfile',ext]);
    const untrustedKey=path.join(dir,'untrusted.key'),untrustedCert=path.join(dir,'untrusted.pem');
    command(['req','-x509','-newkey','rsa:2048','-nodes','-keyout',untrustedKey,'-out',untrustedCert,'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost']);
    return {ca,key,cert,untrustedKey,untrustedCert};
}
async function startBackend(ca) {
    const bundle=path.join(root,inputPaths.fixture);
    if(!reference)return startCapsid(args.get('--host'),args.get('--worker'),bundle,dir,mode,
        {permissions:{modules:[],fetch:{allow:['localhost','127.0.0.1']}},fetchTargets:['localhost','127.0.0.1'],env:{SSL_CERT_FILE:ca},requestTimeoutMs:20000});
    const reserve=net.createServer();const port=await listen(reserve);await new Promise(resolve=>reserve.close(resolve));
    fs.copyFileSync(bundle,path.join(dir,'main.mjs'));fs.copyFileSync(ca,path.join(dir,'trust.pem'));
    const config=path.join(dir,'reference.capnp');fs.writeFileSync(config,`using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),
(name="internet",network=(allow=["local"],tlsOptions=(trustBrowserCas=false,trustedCertificates=[embed "trust.pem"])))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child=spawn(reference,['serve',config],{stdio:['ignore','pipe','pipe']});let stderr='';child.stderr.setEncoding('utf8');child.stderr.on('data',c=>stderr+=c);
    try {
        const deadline=performance.now()+10000;
        for(;;){assert.ok(performance.now()<deadline&&child.exitCode===null&&child.signalCode===null,stderr);try{const r=await fetch(`http://127.0.0.1:${port}/healthy`);await r.text();if(r.status===200)break;}catch{}await delay(10);}
        return {child,port,stderr:()=>stderr};
    }catch(error){await stopCapsid(child);throw error;}
}
try {
    const pythonIdentity=await runJson(python,['-I',pythonProgram,'identity']);
    const nodeIdentity=loadNodeWS().identity;
    const certs=certificates();
    nodePeer=await startNodePeer(certs);
    peer=spawnPeer(['--cert',certs.cert,'--key',certs.key,'--untrusted-cert',certs.untrustedCert,'--untrusted-key',certs.untrustedKey]);
    const ready=await peer.next();assert.equal(ready.type,'ready');assert.deepEqual(ready.identity,pythonIdentity);
    const peerTLS=await proxy(ready.ports.wss),peerUntrusted=await proxy(ready.ports.untrusted);
    const nodeTLS=await proxy(nodePeer.ports.wss),nodeUntrusted=await proxy(nodePeer.ports.untrusted);
    backend=await startBackend(certs.ca);
    const inboundTLS=await proxy(backend.port,{key:fs.readFileSync(certs.key),cert:fs.readFileSync(certs.cert)});
    const inboundUntrusted=await proxy(backend.port,{key:fs.readFileSync(certs.untrustedKey),cert:fs.readFileSync(certs.untrustedCert)});
    const endpoints={inbound:{ws:`ws://127.0.0.1:${backend.port}`,wss:`wss://localhost:${inboundTLS.port}`,
        untrusted:`wss://localhost:${inboundUntrusted.port}`,wrongHost:`wss://127.0.0.1:${inboundTLS.port}`},
        outbound:{python:{ws:`ws://127.0.0.1:${ready.ports.ws}`,wss:`wss://localhost:${peerTLS.port}`,
            untrusted:`wss://localhost:${peerUntrusted.port}`,wrongHost:`wss://127.0.0.1:${peerTLS.port}`},
            node:{ws:`ws://127.0.0.1:${nodePeer.ports.ws}`,wss:`wss://localhost:${nodeTLS.port}`,
            untrusted:`wss://localhost:${nodeUntrusted.port}`,wrongHost:`wss://127.0.0.1:${nodeTLS.port}`}}};
    for(const item of interopCases.filter(x=>x.direction==='inbound')) {
        const challenge=runId+':'+item.name;
        const uri=endpoints.inbound[item.transport]+'/\u0040capsid/orders/inbound?'+new URLSearchParams({case:item.name});
        const observedProxy=item.transport==='untrusted'?inboundUntrusted:item.transport==='ws'?null:inboundTLS;
        const before=observedProxy?.accepted();
        const config={uri,case:item.name,challenge,operation:item.operation,ca:certs.ca};
        const observed=item.client==='python'
            ?await runJson(python,['-I',pythonProgram,'client',...Object.entries(config).flatMap(([k,v])=>['--'+k,String(v)])])
            :await runJson(process.execPath,[nodeProgram,JSON.stringify(config)],{NODE_EXTRA_CA_CERTS:certs.ca});
        cases.push({name:item.name,observed,tcpAccepted:observedProxy?observedProxy.accepted()-before:null});
    }
    for(const item of interopCases.filter(x=>x.direction==='outbound')) {
        const target=endpoints.outbound[item.peer][item.transport]+'/echo?'+new URLSearchParams({case:item.name,challenge:runId+':'+item.name});
        const proxies=item.peer==='node'?{wss:nodeTLS,untrusted:nodeUntrusted}:{wss:peerTLS,untrusted:peerUntrusted};
        const observedProxy=item.transport==='untrusted'?proxies.untrusted:item.transport==='ws'?null:proxies.wss;
        const before=observedProxy?.accepted();
        const response=await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/outbound?`+new URLSearchParams({target}),{signal:AbortSignal.timeout(10000)});
        const text=await response.text();assert.equal(response.status,200,`${text}\n${backend.stderr()}`);
        cases.push({name:item.name,observed:JSON.parse(text),tcpAccepted:observedProxy?observedProxy.accepted()-before:null});
    }
    await waitFor(()=>sockets.size===0,'all actual TLS/proxy transports end before teardown');
    peer.send({op:'snapshot'});const peerSnapshot=await peer.next();assert.equal(peerSnapshot.type,'snapshot');assert.equal(peerSnapshot.active,0);
    peer.send({op:'stop'});peerStopped=await peer.next();assert.equal(peerStopped.type,'stopped');
    await waitFor(()=>peer.child.exitCode!==null||peer.child.signalCode!==null,'Python peer normal exit');
    assert.equal(peer.child.exitCode,0,peer.stderr());assert.equal(peer.child.signalCode,null);
    nodeStopped=await nodePeer.stop();
    const health=await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/healthy`);const healthy={status:health.status,text:await health.text()};
    const shutdown=await stopCapsid(backend.child);
    const report={schema:'capsid-websocket-interop-v1',complete:true,runId,mode,referenceLock:lock,clientLock,inputs,
        binaries:{host:fileDigest(reference??args.get('--host')),worker:reference?null:fileDigest(args.get('--worker'))},
        clients:{node:nodeIdentity,python:pythonIdentity},endpoints,cases,
        peers:{python:{...peerStopped,exit:peer.child.exitCode,signal:peer.child.signalCode,stderr:peer.stderr()},node:nodeStopped},backendStderr:backend.stderr(),healthy,shutdown};
    if(destination)fs.writeFileSync(destination+'.raw.json',JSON.stringify(report,null,2)+'\n');
    validateInteropReport(report,{runId,lock,clientLock,inputs,host:reference?undefined:fileDigest(args.get('--host')),worker:reference?undefined:fileDigest(args.get('--worker'))});
    if(destination)fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');
    console.log(`WebSocket independent clients: ${mode} ${cases.length} cases PASS; Node/Python peers ${nodeStopped.records.length}/${peerStopped.records.length} real sessions`);
} catch(error) {
    if(destination)fs.writeFileSync(destination,JSON.stringify({complete:false,status:'failed',runId,mode,error:error.message})+'\n');
    console.error('interop failure',error,'backend:',backend?.stderr(),'python:',peer?.stderr());process.exitCode=1;
}
finally {
    if(backend)await stopCapsid(backend.child);
    if(peer&&peer.child.exitCode===null&&peer.child.signalCode===null){peer.child.kill('SIGKILL');await new Promise(resolve=>peer.child.once('exit',resolve));}
    if(nodePeer&&!nodeStopped)await nodePeer.stop(true);
    for(const socket of sockets)socket.destroy();
    for(const server of listeners)if(server.listening)await new Promise(resolve=>server.close(resolve));
    fs.rmSync(dir,{recursive:true,force:true});
}
