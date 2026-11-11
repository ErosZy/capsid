import assert from 'node:assert/strict';import fs from 'node:fs';import path from 'node:path';import os from 'node:os';import net from 'node:net';
import {spawn,execFileSync} from 'node:child_process';import {randomUUID,createHash,X509Certificate} from 'node:crypto';
import {fileURLToPath,pathToFileURL} from 'node:url';import {setTimeout as delay} from 'node:timers/promises';
import {root as R,readLock,verifyBinary,verifyClient,fileDigest as sha} from '../tools/workerd-reference.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';
import {fetchProxySession} from './helpers/ws_fetch_proxy_client.mjs';
import {startCapsid,stopCapsid} from './helpers/ws_host_backend.mjs';
import {boundaryPeer} from './helpers/ws_fetch_transition_peer.mjs';
import {fetchTransitionCases,fetchTransitionInputPaths,validateFetchTransitionReport} from '../tools/workerd-fetch-transition-contract.mjs';
const options=new Map();for(let i=2;i<process.argv.length;i+=2){assert.ok(['--mode','--host','--worker','--report','--openssl','--reference-from-env'].includes(process.argv[i])&&process.argv[i+1]&&!options.has(process.argv[i]));options.set(process.argv[i],process.argv[i+1]);}
const selector=options.get('--reference-from-env');assert.ok(selector===undefined||selector==='1');
const reference=selector==='1',mode=reference?'reference':options.get('--mode');
assert.ok(['reference','single-worker','static-pool','managed'].includes(mode));
assert.ok(reference||mode!=='reference','product cannot select the reference role');
assert.ok(!reference||!options.has('--host')&&!options.has('--worker')&&!options.has('--mode'));
const host=reference?process.env.CAPSID_WORKERD_REF:options.get('--host'),worker=options.get('--worker');
assert.ok(host&&(reference||worker));const lock=readLock();verifyClient(lock);if(reference)verifyBinary(host,lock);
const runId=process.env.CAPSID_WS_RUN_ID??randomUUID();
const destination=options.get('--report')??path.join(process.env.CAPSID_WS_REPORT_DIR??os.tmpdir(),
    (process.env.CAPSID_WS_REPORT_DIR?'fetch-transition-':'capsid-fetch-transition-'+runId+'-')+mode+'.json');
assert.ok(!fs.existsSync(destination));fs.mkdirSync(path.dirname(destination),{recursive:true});
const openssl=options.get('--openssl');assert.ok(openssl&&path.isAbsolute(openssl)&&fs.statSync(openssl).isFile(),'explicit certificate tool executable');
const opensslSHA256=sha(openssl);
const fixture=path.join(R,fetchTransitionInputPaths.fixture);
const inputPaths=Object.fromEntries(Object.entries(fetchTransitionInputPaths).map(([key,file])=>[key,path.join(R,file)]));
const inputs=Object.fromEntries(Object.entries(inputPaths).map(([key,file])=>[key,sha(file)])),binaries={host:sha(host),worker:reference?null:sha(worker)};
const certDir=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-fetch-transition-certs-')),ca=path.join(certDir,'ca.pem'),caKey=path.join(certDir,'ca.key'),key=path.join(certDir,'server.key'),cert=path.join(certDir,'server.pem'),csr=path.join(certDir,'server.csr'),untrustedKey=path.join(certDir,'untrusted.key'),untrustedCert=path.join(certDir,'untrusted.pem');
const generate=args=>execFileSync(openssl,args,{stdio:['ignore','pipe','pipe']});
generate(['req','-x509','-newkey','rsa:2048','-nodes','-keyout',caKey,'-out',ca,'-days','1','-subj','/CN=Capsid fetch boundary CA']);
generate(['req','-newkey','rsa:2048','-nodes','-keyout',key,'-out',csr,'-subj','/CN=localhost']);
const ext=path.join(certDir,'extensions');fs.writeFileSync(ext,'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n');
generate(['x509','-req','-in',csr,'-CA',ca,'-CAkey',caKey,'-CAcreateserial','-out',cert,'-days','1','-extfile',ext]);
generate(['req','-x509','-newkey','rsa:2048','-nodes','-keyout',untrustedKey,'-out',untrustedCert,'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost']);
const ipCert=path.join(certDir,'ip-server.pem'),ipExt=path.join(certDir,'ip-extensions');fs.writeFileSync(ipExt,'subjectAltName=IP:127.0.0.1\nextendedKeyUsage=serverAuth\n');
generate(['x509','-req','-in',csr,'-CA',ca,'-CAkey',caKey,'-CAcreateserial','-out',ipCert,'-days','1','-extfile',ipExt]);
const ipv6Cert=path.join(certDir,'ipv6-server.pem'),ipv6Ext=path.join(certDir,'ipv6-extensions');fs.writeFileSync(ipv6Ext,'subjectAltName=IP:::1\nextendedKeyUsage=serverAuth\n');
generate(['x509','-req','-in',csr,'-CA',ca,'-CAkey',caKey,'-CAcreateserial','-out',ipv6Cert,'-days','1','-extfile',ipv6Ext]);
const certificates={ipv6Cert:fs.readFileSync(ipv6Cert),ipCert:fs.readFileSync(ipCert),key:fs.readFileSync(key),cert:fs.readFileSync(cert),untrustedKey:fs.readFileSync(untrustedKey),untrustedCert:fs.readFileSync(untrustedCert)};
const report={schema:'capsid-fetch-transition-v1',complete:false,mode,runId,referenceLock:lock,
    client:{node:process.version,undici:process.versions.undici},inputs,binaries,certificateInputs:{
        opensslVersion:execFileSync(openssl,['version'],{encoding:'utf8'}).trim(),opensslSHA256,caSHA256:sha(ca),
        ipv6ServerSHA256:sha(ipv6Cert),ipServerSHA256:sha(ipCert),serverSHA256:sha(cert),untrustedSHA256:sha(untrustedCert),
        serverSAN:new X509Certificate(certificates.cert).subjectAltName,
        ipv6ServerSAN:new X509Certificate(certificates.ipv6Cert).subjectAltName,ipServerSAN:new X509Certificate(certificates.ipCert).subjectAltName,
        untrustedSAN:new X509Certificate(certificates.untrustedCert).subjectAltName,
        caPEM:fs.readFileSync(ca,'utf8'),serverPEM:certificates.cert.toString(),
        ipv6ServerPEM:certificates.ipv6Cert.toString(),ipServerPEM:certificates.ipCert.toString(),untrustedPEM:certificates.untrustedCert.toString()},cases:[]};
const save=()=>fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');save();
async function poll(read,accept,milliseconds=3000){const end=performance.now()+milliseconds;for(;;){const value=await read();if(accept(value))return {matched:true,value};if(performance.now()>=end)return {matched:false,value};await delay(10);}}
async function stopReference(backend){let timer,forced=false;if(backend.child.exitCode===null&&backend.child.signalCode===null)backend.child.kill('SIGTERM');
    try{await Promise.race([backend.closed,new Promise(resolve=>{timer=setTimeout(()=>{forced=true;backend.child.kill('SIGKILL');resolve();},2000);})]);await backend.closed;}finally{clearTimeout(timer);}return {code:backend.child.exitCode,signal:backend.child.signalCode,forced};}
async function startReference(directory,kind){
    const reserve=net.createServer();await new Promise(resolve=>reserve.listen(0,'127.0.0.1',resolve));const port=reserve.address().port;await new Promise(resolve=>reserve.close(resolve));
    fs.copyFileSync(fixture,path.join(directory,'main.mjs'));fs.copyFileSync(ca,path.join(directory,'trust.pem'));const config=path.join(directory,'main.capnp');
    fs.writeFileSync(config,`using Workerd = import "/workerd/workerd.capnp"; const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=${JSON.stringify(kind==='redirect-https-denied'?['127.0.0.1/32','::1/128']:['local'])},tlsOptions=(trustBrowserCas=false,trustedCertificates=[embed "trust.pem"])))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child=spawn(host,['serve',config],{stdio:['ignore','pipe','pipe']});child.stdout.resume();let stderr='';child.stderr.on('data',data=>stderr+=data);const backend={child,port,stderr:()=>stderr,closed:new Promise(resolve=>child.once('close',resolve))};
    try{const ready=await poll(async()=>{assert.ok(child.exitCode===null&&child.signalCode===null,stderr);try{const response=await fetch(`http://127.0.0.1:${port}/state`,{signal:AbortSignal.timeout(500)});return response.status===200&&await response.text();}catch{return false;}},Boolean,10000);assert.ok(ready.matched,stderr);return backend;}
    catch(error){await stopReference(backend);throw error;}
}
const kinds=fetchTransitionCases;
for(const kind of kinds){
    const value={kind,challenge:randomUUID(),checkpoint:null,checkpointUpstream:null,followedBeforeRelease:null,binary:null,foreign:null,failure:null,cleanupFailure:null},directory=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-fetch-transition-'));report.cases.push(value);save();let upstream,backend,peer;
    try{
        upstream=await boundaryPeer(kind,value.challenge,certificates);value.target=upstream.target;value.sourcePort=upstream.sourcePort;value.targetPort=upstream.targetPort??null;value.allowedAuthorities=upstream.allow;
        backend=reference?await startReference(directory,kind):await startCapsid(host,worker,fixture,directory,mode,{permissions:{modules:[],fetch:{allow:upstream.allow}},fetchTargets:upstream.allow,env:{SSL_CERT_FILE:ca},requestTimeoutMs:30000});
        const prefix=reference?'':'/@capsid/orders';const state=async()=>{const response=await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`,{signal:AbortSignal.timeout(4000)});return {status:response.status,text:await response.text()};};value.initial=await state();
        const query=new URLSearchParams({kind,challenge:value.challenge,target:upstream.target});
        peer=kind==='tls-ipv6-proxy'?fetchProxySession(backend.port,prefix+'/probe?'+query,{observeCloseReply:true,timeoutMs:30000}):responseSession(backend.port,prefix+'/probe?'+query,{observeCloseReply:true,timeoutMs:30000});
        value.headers=await peer.headers;
        if(kind==='tls-ipv6-proxy'){
            if(value.headers.status===101){const bytes=Buffer.alloc(180000);for(let i=0;i<bytes.length;i++)bytes[i]=(i*13+value.challenge.charCodeAt(i%value.challenge.length))%251;peer.sendBinary(bytes);const received=await peer.binary();assert.ok(bytes.equals(received),'complete actual TLS proxy payload');value.binary={bytes:received.length,sha256:createHash('sha256').update(received).digest('hex')};peer.close();}
        }else{
            assert.equal(value.headers.status,101);peer.send('go');assert.equal(await peer.text(),'started');
            if(kind.endsWith('-after-close')){value.checkpoint=await state();value.checkpointUpstream=upstream.snapshot();peer.close();}
            if(['tls-body-reader-cancel','tls-body-abort','tls-body-abort-before-read','tls-body-context-end'].includes(kind)){
                assert.equal(await peer.text(),kind==='tls-body-abort-before-read'?'headers-ready':'body-first');
                value.checkpoint=await state();value.checkpointUpstream=upstream.snapshot();
                if(kind==='tls-body-context-end')peer.close();else peer.send(kind==='tls-body-reader-cancel'?'cancel':'abort');
            }
            if(['tls-upgrade-abort-pending','tls-handshake-abort','redirect-held-abort'].includes(kind)){
                const ready=await poll(()=>upstream.snapshot(),s=>kind==='tls-handshake-abort'?s.records.some(row=>row.receivedChunks.length>0):s.records.some(row=>row.request!==null&&(kind!=='redirect-held-abort'||row.responseStatus===302)));
                assert.ok(ready.matched,'actual requested phase reached before abort');value.checkpoint=await state();value.checkpointUpstream=ready.value;peer.send('abort');
            }

        }
        value.wire=await peer.complete;if(kind==='http-created-promise-after-close'){const response=await fetch(`http://127.0.0.1:${backend.port}${prefix}/resolve`,{signal:AbortSignal.timeout(4000)});value.foreign={status:response.status,text:await response.text()};}await delay(kind==='tls-body-context-end'||kind.endsWith('-after-close')?750:0);value.observed=await state();const ended=await poll(()=>upstream.snapshot(),value=>value.tcpRecords.every(row=>row.closed),2000);value.allSourcesEndedBeforeStop=ended.matched;
    }catch(error){value.failure=error.stack;if(error.observation)value.failureObservation=error.observation;}
    finally{
        value.upstreamBeforeStop=upstream?.snapshot();peer?.dispose();
        if(backend){try{value.shutdown=reference?await stopReference(backend):await stopCapsid(backend.child);}catch(error){value.cleanupFailure=error.stack;}value.stderr=backend.stderr();}
        value.upstreamAfterStop=upstream?.snapshot();if(upstream)await upstream.stop();save();
    }
}
assert.deepEqual(inputs,Object.fromEntries(Object.entries(inputPaths).map(([key,file])=>[key,sha(file)])));assert.deepEqual(binaries,{host:sha(host),worker:reference?null:sha(worker)});
assert.equal(sha(openssl),opensslSHA256,'certificate executable unchanged during run');
report.complete=report.cases.length===fetchTransitionCases.length&&report.cases.every(value=>value.observed?.status===200&&value.shutdown&&!value.shutdown.forced&&!value.failure&&!value.cleanupFailure);save();
assert.ok(report.complete,'complete capture and normal cleanup');
validateFetchTransitionReport(report,{runId,lock,inputs,host:binaries.host,worker:binaries.worker});
console.log(JSON.stringify({complete:true,mode,cases:report.cases.length}));
