import strictAssert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import tls from 'node:tls';
import {execFileSync,spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid,stopCapsid} from './helpers/ws_host_backend.mjs';
import {readLock,verifyClient,verifyBinary,assertOutboundSuccess} from '../tools/workerd-reference.mjs';
import {abruptOperations,assertOutboundDisconnect,assertOutboundHttpOwnership,outboundDeviations} from '../tools/workerd-outbound-contract.mjs';

const arguments_=process.argv.slice(2);
function option(name,fallback) { const i=arguments_.indexOf(name); return i<0 ? fallback : arguments_[i+1]; }
let assertions=0;
const assert=new Proxy(strictAssert,{get(target,key){const value=target[key];return typeof value==='function' ? (...args)=>{assertions++;return value(...args)} : value}});
const host=option('--host'),worker=option('--worker');
const fromEnv=arguments_.includes('--reference-from-env');
const referenceArg=option('--workerd');
const reference=referenceArg||(fromEnv?process.env.CAPSID_WORKERD_REF:undefined)||undefined;
if(fromEnv&&!reference){
    console.log('SKIP: CAPSID_WORKERD_REF is not set (locked workerd reference binary unavailable)');
    process.exit(77);
}
const mode=reference?'workerd':option('--mode','single-worker');
const reportPath=option('--report') || (process.env.CAPSID_WS_REPORT_DIR
    ? path.join(process.env.CAPSID_WS_REPORT_DIR, `outbound-${reference ? 'reference' : mode}.json`) : undefined);
assert.ok(reference || host && worker,'host/worker or locked workerd paths are required');
const lock=readLock();verifyClient(lock);
if(reference) verifyBinary(reference,lock);
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-outbound-'));
const sockets=new Set(), servers=[], masks=new Set(), nonces=new Set(), traces=[],cases=[],ownedPeers=new Map();
const deviations=outboundDeviations;
let frameCount=0,accepted=0,sni='';
const bundle=path.join(dir,'bundle.mjs');
fs.writeFileSync(bundle,`
let independent=[];
const ownedJobs=new Map();
function inspectOwned(job) {
  const state={events:job.events.slice(),returnedState:job.returnedState,
    sendRejected:job.sendRejected,acceptRejected:job.acceptRejected};
  for(const key of ['readyState','protocol','extensions']) {
    try {state[key]=job.ws[key]}catch(error){state[key]={error:error.name,message:error.message}}
  }
  return state;
}
async function startOwned(url) {
  const id=url.searchParams.get('id'),target=url.searchParams.get('target');
  const ws=new WebSocket(target,['chat','super']),events=[{type:'construct',state:ws.readyState,url:ws.url}];
  const job={ws,events,sendRejected:false,acceptRejected:false};ownedJobs.set(id,job);
  try{ws.send('early')}catch{job.sendRejected=true}try{ws.accept()}catch{job.acceptRejected=true}
  let opened;const open=new Promise(resolve=>opened=resolve);
  ws.onopen=()=>{events.push({type:'open',state:ws.readyState,protocol:ws.protocol});opened()};
  ws.onmessage=e=>{events.push({type:'message',data:e.data});ws.send('reply:'+e.data);ws.close(1000,'done\\0终')};
  ws.onerror=e=>events.push({type:'error',state:ws.readyState,message:e.message,error:String(e.error),
    errorType:typeof e.error,errorIsError:e.error instanceof Error,errorName:e.error?.name??null,errorMessage:e.error?.message??null});
  ws.onclose=e=>events.push({type:'close',state:ws.readyState,code:e.code,reason:e.reason,clean:e.wasClean});
  if(id.startsWith('streamed')) {
    job.returnedState=ws.readyState;
    return new Response(new ReadableStream({async start(controller){
      controller.enqueue(new TextEncoder().encode('streamed'));
      await new Promise(resolve=>job.release=resolve);
      controller.close();
    }}),{status:202});
  }
  if(id.startsWith('accepted'))await open;else await new Promise(resolve=>job.release=resolve);
  job.returnedState=ws.readyState;return new Response('ended',{status:202});
}
function connect(target,kind='echo',output=null,opened=null) {
  return new Promise(resolve=>{
    const ws=new WebSocket(target,['chat','super']); ws.binaryType='arraybuffer';
    const events=[]; let received=0,closes=0;
    events.push({type:'construct',state:ws.readyState,url:ws.url});
    let sendRejected=false,acceptRejected=false;
    try {ws.send('early')} catch {sendRejected=true}
    try {ws.accept()} catch {acceptRejected=true}
    ws.onopen=async()=>{
      events.push({type:'open',state:ws.readyState,protocol:ws.protocol});
      if(opened)opened();
      if(kind==='independent') await new Promise(r=>setTimeout(r,12));
      if(kind==='large') {const data=new Uint8Array(180000);for(let i=0;i<data.length;i++)data[i]=i%251;ws.send(data);data.fill(255)}
      else if(kind==='typed') {const data=new Uint8Array([1,2,3,4,5]);ws.send(data.subarray(1,4));data.fill(9)}
      else if(kind!=='coalesced') ws.send('hello');
    };
    ws.onmessage=e=>{
      received++;
      let data=e.data;
      if(data instanceof ArrayBuffer) {
        const bytes=new Uint8Array(data); let correct=true;
        for(let i=0;i<bytes.length;i++)if(bytes[i]!== (kind==='large'?i%251:i+2))correct=false;
        data={size:bytes.length,correct};
      }
      events.push({type:'message',data});
      if(kind!=='coalesced' || received===2) ws.close(1000,'done\\0终');
    };
    ws.onerror=e=>events.push({type:'error',state:ws.readyState,message:e.message,error:String(e.error),
      errorType:typeof e.error,errorIsError:e.error instanceof Error,errorName:e.error?.name??null,
      errorMessage:e.error?.message??null});
    ws.onclose=e=>{
      closes++;
      events.push({type:'close',state:ws.readyState,code:e.code,reason:e.reason,clean:e.wasClean});
      const result={events,sendRejected,acceptRejected};
      const finish=()=>{if(output)output.push(result);resolve(result)};
      if(!kind.startsWith('abrupt-')) {finish();return;}
      const operation=kind.slice('abrupt-'.length);
      const lifecycle=result.lifecycle={operation,closes};
      const call=name=>{try {
        const returned=name==='send'?ws.send('after EOF'):ws.close(1000,'after EOF');
        return {result:typeof returned,state:ws.readyState};
      } catch(error) {return {error:error.name,message:error.message,state:ws.readyState}}};
      return new Promise(settled=>{
        if(operation==='send-in')lifecycle.call=call('send');
        if(operation==='close-in')lifecycle.call=call('close');
        lifecycle.afterCallbackMethods=ws.readyState;
        queueMicrotask(()=>{lifecycle.microtask=ws.readyState});
        setTimeout(()=>{
          lifecycle.timer=ws.readyState;
          if(operation==='send-after')lifecycle.call=call('send');
          if(operation==='close-after')lifecycle.call=call('close');
          if(operation==='both-after')lifecycle.calls=[call('send'),call('close')];
          lifecycle.afterTimerMethods=ws.readyState;finish();settled();
        },15);
      });
    };
    if(kind==='cancel') ws.close();
    if(kind==='cancel-later') setTimeout(()=>ws.close(),75);
  });
}
export default {async fetch(request) {
  const url=new URL(request.url);
  if(url.pathname.endsWith('/owned-state'))return Response.json(inspectOwned(ownedJobs.get(url.searchParams.get('id'))));
  if(url.pathname.endsWith('/owned-release')){ownedJobs.get(url.searchParams.get('id')).release();return new Response('released')}
  if(url.pathname.endsWith('/owned-start'))return startOwned(url);
  if(url.pathname.endsWith('/probe')) {
    const checks=[];
    for(const [target,protocols] of [['http://localhost',[]],['ws://localhost/#',[]],['bad-url',[]],['ws://localhost',['a','a']],['ws://localhost',['invalid protocol']]]) {
      try {new WebSocket(target,protocols);checks.push('accepted')} catch(e) {checks.push(e.name)}
    }
    return Response.json(checks);
  }
  if(url.pathname.endsWith('/state')) return Response.json(independent);
  if(url.pathname.endsWith('/start')) {connect(url.searchParams.get('target'),'independent',independent);return new Response('started',{status:202})}
  if(url.pathname.endsWith('/start-open')) {await new Promise(resolve=>connect(url.searchParams.get('target'),'independent',independent,resolve));return new Response('started open',{status:202})}
  if(url.pathname.endsWith('/multi')) return Response.json(await Promise.all(url.searchParams.getAll('target').map(target=>connect(target))));
  if(url.pathname.endsWith('/run')) return Response.json(await connect(url.searchParams.get('target'),url.searchParams.get('kind')||'echo'));
  return new Response('healthy');
}};
`);

function wire(opcode,payload,fin=true) {
    payload=Buffer.from(payload); const header=Buffer.alloc(payload.length<126?2:payload.length<65536?4:10);
    header[0]=(fin?128:0)|opcode;
    if(header.length===2) header[1]=payload.length;
    else if(header.length===4) {header[1]=126;header.writeUInt16BE(payload.length,2);}
    else {header[1]=127;header.writeBigUInt64BE(BigInt(payload.length),2);}
    return Buffer.concat([header,payload]);
}
function upstream(socket) {
    sockets.add(socket); socket.on('close',()=>sockets.delete(socket)); socket.on('error',()=>{});
    let input=Buffer.alloc(0), upgraded=false,route='', fragments=[],opcode=0,ownedPeer;
    socket.on('close',()=>{if(ownedPeer)ownedPeer.closed=true});
    function parse() {
        while(input.length>=2) {
            assert.ok(input[1]&128,'outbound frames must be masked');
            let size=input[1]&127,offset=2;
            if(size===126) {if(input.length<4)return;size=input.readUInt16BE(2);offset=4}
            else if(size===127) {if(input.length<10)return;size=Number(input.readBigUInt64BE(2));offset=10}
            assert.ok(size<=1024*1024,'fixture frame size bound');
            if(input.length<offset+4+size)return;
            const mask=input.subarray(offset,offset+4);masks.add(mask.toString('hex'));frameCount++;
            const payload=Buffer.from(input.subarray(offset+4,offset+4+size));for(let i=0;i<size;i++)payload[i]^=mask[i%4];
            const type=input[0]&15,fin=!!(input[0]&128);input=input.subarray(offset+4+size);
            // Keep clean WS close separate from a simultaneous TCP EOF.
            // Both clients finish their transport after the reply; abrupt
            // EOF is covered independently by /abrupt.
            if(type===8) {if(ownedPeer)ownedPeer.closes.push(payload.readUInt16BE(0));socket.write(wire(8,payload));return}
            if(type===10) {assert.deepEqual(payload,Buffer.from('ping'));continue}
            assert.ok([0,1,2].includes(type),'only data/close/pong from client');
            if(type!==0) {assert.equal(fragments.length,0);opcode=type} else assert.ok(fragments.length,'continuation follows start');
            fragments.push(payload);
            if(fin) {
                const message=Buffer.concat(fragments);fragments=[];
                if(ownedPeer)ownedPeer.messages.push(message.toString());
                else if(route==='/abrupt') socket.destroy(); else socket.write(wire(opcode,message));
            }
        }
    }
    socket.on('data',chunk=>{
        input=Buffer.concat([input,chunk]);
        if(upgraded) {parse();return}
        const end=input.indexOf('\r\n\r\n');if(end<0)return;
        const head=input.subarray(0,end).toString('ascii');input=input.subarray(end+4);
        route=head.split(' ')[1];
        const key=/\r\nSec-WebSocket-Key: ([^\r]+)/i.exec(head)?.[1];
        assert.ok(key && Buffer.from(key,'base64').length===16,'random 16-byte nonce');
        assert.equal(key.length,24);assert.equal(Buffer.from(key,'base64').toString('base64'),key,'canonical handshake nonce');
        assert.ok(!nonces.has(key),'handshakes use distinct nonces');nonces.add(key);
        assert.match(head,/\r\nSec-WebSocket-Protocol: chat, super(?:\r\n|$)/i);
        assert.ok(!head.includes('user:secret'),'userinfo excluded from transport');
        accepted++;
        if(route==='/stall') return;
        if(route==='/not101' || route==='/redirect') {
            socket.end(route==='/not101'?'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n':'HTTP/1.1 302 Found\r\nLocation: /echo\r\nContent-Length: 0\r\n\r\n');return;
        }
        const accept=route==='/badkey'?'invalid':crypto.createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        const protocol=route==='/badprotocol'?'unrequested':'chat';
        let response=Buffer.from('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\nSec-WebSocket-Protocol: '+protocol+'\r\n\r\n');
        if(route.startsWith('/owner-')) {
            ownedPeer={socket,closed:false,messages:[],closes:[],upgrade:()=>{
                if(socket.destroyed)return false;
                socket.write(response);upgraded=true;traces.push(route);parse();return true;
            }};
            ownedPeers.set(route,ownedPeer);
            if(route.startsWith('/owner-accepted'))ownedPeer.upgrade();
            return;
        }
        if(route==='/coalesced') response=Buffer.concat([response,wire(1,'coalesced'),wire(1,[0xe7],false),wire(9,'ping'),wire(0,[0x95,0x8c])]);
        const milliseconds=route==='/slow'?120:route==='/medium'?50:0;
        setTimeout(()=>{if(socket.destroyed)return;socket.write(response);upgraded=true;traces.push(route);parse()},milliseconds);
    });
}
async function listen(factory) {
    // localhost prefers ::1 on Windows. Serve both loopback families on the
    // same port so every failed-handshake stress round reaches the peer
    // directly instead of measuring a refused IPv6 connection fallback.
    for (let attempt=0;attempt<10;++attempt) {
        const ipv4=factory(),ipv6=factory();
        const pair=[ipv4,ipv6];
        for(const server of pair)server.on('connection',socket=>{
            // Include connections that never finish TLS in final teardown.
            sockets.add(socket);socket.once('close',()=>sockets.delete(socket));socket.on('error',()=>{});
        });
        const bind=(server,host,port)=>new Promise((resolve,reject)=>{
            server.once('error',reject);
            server.listen({host,port,ipv6Only:host==='::1'},()=>{server.removeListener('error',reject);resolve();});
        });
        try {
            await bind(ipv4,'127.0.0.1',0);
            const port=ipv4.address().port;
            await bind(ipv6,'::1',port);
            servers.push(...pair);
            return port;
        } catch(error) {
            await Promise.all(pair.map(server=>new Promise(resolve=>server.listening?server.close(resolve):resolve())));
            if(error.code!=='EADDRINUSE')throw error;
        }
    }
    assert.fail('independent peer could not reserve both loopback families');
}
async function startBackend(directory,options={},ca) {
    if(!reference)return startCapsid(host,worker,bundle,directory,mode,options);
    const listener=net.createServer();await new Promise(resolve=>listener.listen(0,'127.0.0.1',resolve));const port=listener.address().port;
    await new Promise(resolve=>listener.close(resolve));
    const config=path.join(directory,'reference.capnp');
    fs.copyFileSync(bundle,path.join(directory,'main.mjs'));fs.copyFileSync(ca,path.join(directory,'trust.pem'));
    fs.writeFileSync(config,`using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
 services = [(name="main", worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),
 (name="internet",network=(allow=${options.permissions?'["local"]':'["public"]'},tlsOptions=(trustBrowserCas=false,trustedCertificates=[embed "trust.pem"])))],
 sockets = [(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child=spawn(reference,['serve',config],{stdio:['ignore','pipe','pipe']});let stderr='';child.stderr.setEncoding('utf8');child.stderr.on('data',chunk=>stderr+=chunk);
    try {
        const deadline=Date.now()+10000;
        for(;;) {
            assert.ok(Date.now()<deadline && child.exitCode===null && child.signalCode===null,stderr);
            try {const response=await fetch(`http://127.0.0.1:${port}/ready`);if(response.status===200)break}catch{}
            await delay(20);
        }
        return {child,port,stderr:()=>stderr};
    } catch(error) {await stopCapsid(child);throw error}
}
async function get(backend,target,kind='echo') {
    const query=new URLSearchParams({target,kind});
    const response=await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/run?${query}`,{signal:AbortSignal.timeout(15000)});
    const text=await response.text();assert.equal(response.status,200,`${text}\n${backend.stderr()}`);
    const parsed=new URL(target);const result=JSON.parse(text);assert.equal(result.events[0].url,target,'original JS-visible URL');
    cases.push({name:`${backend===denied?'denied:':''}${parsed.protocol}//${parsed.hostname}${parsed.pathname}:${kind}`,result});return result;
}
function success(result,expected,selected='chat') {
    return assertOutboundSuccess(result,expected,selected,mode,assert);
}
function failure(result) {
    assert.deepEqual(result.events.map(e=>e.type),['construct','error','close']);
    const error = result.events[1];
    assert.equal(error.errorType, 'object');
    assert.equal(error.errorIsError, true);
    assert.equal(error.error, `${error.errorName}: ${error.errorMessage}`);
    assert.equal(error.message, 'Uncaught ' + error.error);
    const pathname = new URL(result.events[0].url).pathname;
    if (pathname === '/not101' || pathname === '/redirect') {
        assert.equal(error.errorName, 'TypeError', 'non-101 response is a TypeError');
        assert.equal(error.errorMessage,
            `Failed to establish the WebSocket connection: expected server to reply with HTTP status code 101 (switching protocols), but received ${pathname === '/not101' ? 200 : 302} instead.`);
    } else assert.equal(error.errorName, 'Error');
    assert.equal(result.events[1].state,3);assert.deepEqual(result.events.at(-1),{type:'close',state:3,code:1006,reason:'Failed to establish websocket connection',clean:false});
}
async function ownedHttpCases(plainPort,ca) {
    const directory=path.join(dir,'owned');fs.mkdirSync(directory);
    fs.copyFileSync(bundle,path.join(directory,'bundle.mjs'));
    const allow=[`localhost:${plainPort}`];
    ownedBackend=await startBackend(directory,{permissions:{modules:[],fetch:{allow}},fetchTargets:allow,
        channelCapacity:{pendingDialsTotal:2}},ca);
    const route=(name,id)=>`http://127.0.0.1:${ownedBackend.port}/@capsid/orders/owned-${name}?${new URLSearchParams({id})}`;
    const requestRoute=async(name,id)=>{
        const response=await fetch(route(name,id),{signal:AbortSignal.timeout(10000)});
        const text=await response.text();assert.equal(response.status,200,`${text}\n${ownedBackend.stderr()}`);return text;
    };
    const wait=async condition=>{
        const deadline=performance.now()+8000;
        while(!condition()) {assert.ok(performance.now()<deadline,'pending owner transport deadline');await delay(5)}
    };
    for(const [name,id] of [
        ['owned-pending-success','pending-success'],['owned-pending-reject','pending-reject'],
        ['owned-pending-stall','pending-stall-0'],['owned-pending-stall','pending-stall-1'],
        ['owned-pending-stall','pending-stall-2'],['owned-accepted-after-HTTP','accepted-success'],
        ['owned-streamed-before-HTTP-end','streamed-success'],
    ]) {
        const path_='/owner-'+id,target=`ws://localhost:${plainPort}${path_}`;
        let header;
        const request=fetch(route('start',id)+'&'+new URLSearchParams({target}),{signal:AbortSignal.timeout(10000)});
        request.then(value=>header=value,()=>{});
        await wait(()=>ownedPeers.has(path_) || header && header.status!==202);
        if(header && header.status!==202)assert.fail(await header.text());
        const peer=ownedPeers.get(path_);
        const streamed=id.startsWith('streamed'),accepted=id.startsWith('accepted') || streamed;
        if(streamed) {
            peer.upgrade();
            const deadline=performance.now()+8000;
            for(;;) {
                const view=JSON.parse(await requestRoute('state',id));
                if(view.events.some(e=>e.type==='open'))break;
                assert.ok(performance.now()<deadline,'connection opens while response body remains unfinished');
                await delay(5);
            }
            await requestRoute('release',id);
        } else if(!accepted)await requestRoute('release',id);
        const response=await request;const text=await response.text();assert.equal(response.status,202,text);
        if(id==='pending-reject') {
            if(!peer.socket.destroyed)peer.socket.end('HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n');
        } else if(id==='pending-success') {
            if(peer.upgrade())peer.socket.write(wire(1,'after HTTP'));
        } else if(accepted)peer.socket.write(wire(1,'after HTTP'));
        await wait(()=>peer.closed);
        const result={response:{status:response.status,text},state:JSON.parse(await requestRoute('state',id)),
            peer:{closed:peer.closed,messages:[...peer.messages],closes:[...peer.closes]}};
        cases.push({name,result});
        assertOutboundHttpOwnership(result,accepted,mode,assert,streamed);
    }
    const healthy=await fetch(`http://127.0.0.1:${ownedBackend.port}/@capsid/orders/healthy`);
    assert.equal(healthy.status,200);assert.equal(await healthy.text(),'healthy','repeated abandoned dials leave HTTP healthy');
}
let backend,denied,ownedBackend,report;
const shutdowns=[];
const oldCA=process.env.SSL_CERT_FILE;
try {
    const plainPort=await listen(()=>net.createServer(upstream));
    const openssl=args=>execFileSync(option('--openssl','openssl'),args,{stdio:'ignore'});
    const ca=path.join(dir,'ca.pem'),cakey=path.join(dir,'ca.key'),key=path.join(dir,'server.key'),csr=path.join(dir,'server.csr'),cert=path.join(dir,'server.pem');
    openssl(['req','-x509','-newkey','rsa:2048','-nodes','-keyout',cakey,'-out',ca,'-days','1','-subj','/CN=Capsid test CA']);
    openssl(['req','-newkey','rsa:2048','-nodes','-keyout',key,'-out',csr,'-subj','/CN=localhost']);
    const extensions=path.join(dir,'extensions');fs.writeFileSync(extensions,'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n');
    openssl(['x509','-req','-in',csr,'-CA',ca,'-CAkey',cakey,'-CAcreateserial','-out',cert,'-days','1','-extfile',extensions]);
    const tlsPort=await listen(()=>tls.createServer({key:fs.readFileSync(key),cert:fs.readFileSync(cert)},socket=>{sni=socket.servername;upstream(socket)}));
    const untrustedKey=path.join(dir,'untrusted.key'),untrustedCert=path.join(dir,'untrusted.pem');
    openssl(['req','-x509','-newkey','rsa:2048','-nodes','-keyout',untrustedKey,'-out',untrustedCert,'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost']);
    const untrustedPort=await listen(()=>tls.createServer({key:fs.readFileSync(untrustedKey),cert:fs.readFileSync(untrustedCert)},upstream));
    const refusedPort=await listen(()=>net.createServer(socket=>{sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{})}));
    process.env.SSL_CERT_FILE=ca;
    const allow=[`127.0.0.1:${plainPort},${tlsPort}`,`localhost:${plainPort},${tlsPort},${refusedPort},${untrustedPort}`,'does-not-exist.invalid:80'];
    backend=await startBackend(dir,{permissions:{modules:[],fetch:{allow}},fetchTargets:allow,requestTimeoutMs:30000},ca);
    await ownedHttpCases(plainPort,ca);
    const response=await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/probe`);const probes=await response.json();assert.deepEqual(probes,Array(5).fill('SyntaxError'));cases.push({name:'constructor-invalid-url-protocols',result:probes});
    success(await get(backend,`ws://127.0.0.1:${plainPort}/echo`),['hello']);
    success(await get(backend,`ws://localhost:${plainPort}/echo`),['hello']);
    success(await get(backend,`ws://user:secret@localhost:${plainPort}/typed`,'typed'),[{size:3,correct:true}]);
    success(await get(backend,`ws://localhost:${plainPort}/large`,'large'),[{size:180000,correct:true}]);
    success(await get(backend,`ws://localhost:${plainPort}/coalesced`,'coalesced'),['coalesced','界']);
    success(await get(backend,`wss://localhost:${tlsPort}/echo`),['hello']);assert.equal(sni,'localhost');const validSNI=sni;
    for(const target of [`wss://127.0.0.1:${tlsPort}/echo`,`ws://localhost:${plainPort}/badkey`,
        `ws://localhost:${plainPort}/not101`,`ws://localhost:${plainPort}/redirect`,`ws://example.invalid/echo`,`ws://169.254.169.254/echo`,
        `wss://localhost:${untrustedPort}/untrusted`,`ws://does-not-exist.invalid/echo`]) failure(await get(backend,target));
    const unrequested=await get(backend,`ws://localhost:${plainPort}/badprotocol`);
    if(reference)success(unrequested,['hello'],'unrequested');else failure(unrequested);
    failure(await get(backend,`ws://localhost:${plainPort}/echo`,'cancel'));
    failure(await get(backend,`ws://localhost:${plainPort}/stall`,'cancel-later'));
    failure(await get(backend,`wss://localhost:${refusedPort}/echo`,'cancel-later'));
    const abrupt=await get(backend,`ws://localhost:${plainPort}/abrupt`);
    assertOutboundDisconnect(abrupt,undefined,assert);
    for(const operation of abruptOperations) {
        assertOutboundDisconnect(await get(backend,`ws://localhost:${plainPort}/abrupt`,'abrupt-'+operation),operation,assert);
    }
    if(!reference) {
        const began=Date.now();const result=await get(backend,`ws://localhost:${plainPort}/stall`);failure(result);
        const elapsed=Date.now()-began;
        result.elapsedMs=elapsed;
        assert.ok(elapsed>=9000 && elapsed<15000,`configured dial deadline rejects without killing HTTP worker (elapsed=${elapsed}ms)`);
    }
    for(let round=0;round<140;++round)failure(await get(backend,`ws://localhost:${plainPort}/badkey`));
    success(await get(backend,`ws://localhost:${plainPort}/echo`),['hello']);
    const before=traces.length;
    const query=new URLSearchParams();for(const route of ['/slow','/echo','/medium'])query.append('target',`ws://localhost:${plainPort}${route}`);
    const multi=await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/multi?${query}`,{signal:AbortSignal.timeout(15000)});
    assert.equal(multi.status,200,await multi.clone().text());const multiResults=await multi.json();
    cases.push({name:'out-of-order-dials',result:multiResults});
    for(const result of multiResults)success(result,['hello']);
    assert.deepEqual(traces.slice(before),['/echo','/medium','/slow'],'dial IDs correlate out-of-order completions');
    const start=await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/start?${new URLSearchParams({target:`ws://localhost:${plainPort}/echo`})}`);assert.equal(start.status,202);
    let state;const deadline=Date.now()+5000;
    do {state=await (await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/state`)).json();if(!state.length)await delay(20)} while(!state.length && Date.now()<deadline);
    assert.equal(state.length,0,'pending constructor callbacks end with the HTTP owner');
    cases.push({name:'pending-after-HTTP',result:state});
    const previous=state.length;
    const startOpen=await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/start-open?${new URLSearchParams({target:`ws://localhost:${plainPort}/echo`})}`);assert.equal(startOpen.status,202);
    const acceptedDeadline=Date.now()+5000;
    do {state=await (await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/state`)).json();if(state.length===previous)await delay(20)} while(state.length===previous && Date.now()<acceptedDeadline);
    cases.push({name:'accepted-after-HTTP',result:state.at(-1)});
    assert.equal(state.length,previous+1,'accepted connection survives completed HTTP owner');success(state.at(-1),['hello']);
    const deniedDir=path.join(dir,'denied');fs.mkdirSync(deniedDir);
    denied=await startBackend(deniedDir,{},ca);
    const hits=accepted;failure(await get(denied,`ws://localhost:${plainPort}/echo`));assert.equal(accepted,hits,'default deny reaches no upstream socket');
    assert.ok(frameCount>=20 && masks.size>=15,'multiple independently masked outbound frames');
    const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
    report={complete:true,mode,caseInvocations:cases.length,scenarioTypes:new Set(cases.map(c=>c.name)).size,assertions,cases,referenceParity:false,deviations,referenceLock:lock,
        sourceSHA256:hash(new URL(import.meta.url)),hostSHA256:reference?hash(reference):hash(host),workerSHA256:reference?undefined:hash(worker),
        client:{node:process.version,undici:process.versions.undici,peer:'independent raw Node TCP/TLS peer'},
        frames:frameCount,masks:masks.size,nonces:nonces.size,sni:validSNI};
} catch(error) {
    if(reportPath) fs.writeFileSync(reportPath,JSON.stringify({complete:false,mode,cases,traces,
        failure:{message:error.message,stack:error.stack},stderr:{allowed:backend?.stderr(),owned:ownedBackend?.stderr()}},null,2));
    console.error('host stderr:',backend?.stderr(),ownedBackend?.stderr());throw error;
}
finally {
    for(const [label,current] of [['denied',denied],['owned',ownedBackend],['allowed',backend]])if(current)shutdowns.push({label,...await stopCapsid(current.child),stderr:current.stderr()});
    for(const socket of sockets)socket.destroy();for(const server of servers)await new Promise(resolve=>server.close(resolve));
    if(oldCA===undefined)delete process.env.SSL_CERT_FILE;else process.env.SSL_CERT_FILE=oldCA;
    fs.rmSync(dir,{recursive:true,force:true});
}
for(const shutdown of shutdowns) {
    assert.equal(shutdown.forced,false,`bounded graceful stop: ${shutdown.stderr}`);
    assert.ok(shutdown.code===0 || reference && shutdown.signal==='SIGTERM',`clean host shutdown: ${JSON.stringify(shutdown)}`);
    assert.doesNotMatch(shutdown.stderr,/ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:/,'sanitizer failures during shutdown must fail');
}
report.assertions=assertions;report.shutdowns=shutdowns.map(({stderr,...result})=>result);
if(reportPath)fs.writeFileSync(reportPath,JSON.stringify(report,null,2));
console.log(JSON.stringify({...report,cases:undefined}));
