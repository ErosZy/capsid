import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import os from 'node:os';
import {randomUUID} from 'node:crypto';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid, stopCapsid} from './helpers/ws_host_backend.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';
import {root, readLock, verifyBinary, verifyClient, fileDigest} from '../tools/workerd-reference.mjs';
import {ordinaryFetchCasesForShard, ordinaryFetchInputPaths, ordinaryFetchGatedPlans, ordinaryFetchNativePlans, validateOrdinaryFetchReport} from '../tools/workerd-ordinary-fetch-contract.mjs';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    assert.ok(['--reference-from-env', '--host', '--worker', '--mode', '--report', '--shard'].includes(process.argv[index]), 'known ordinary fetch option');
    assert.ok(!args.has(process.argv[index]) && process.argv[index + 1], 'unique valued ordinary fetch option');
    args.set(process.argv[index], process.argv[index + 1]);
}
const referencePath = args.has('--reference-from-env') ? process.env.CAPSID_WORKERD_REF : undefined;
const reference = referencePath ? path.resolve(referencePath) : undefined;
if (args.has('--reference-from-env')) {
    assert.equal(args.get('--reference-from-env'), '1'); assert.ok(reference, 'CAPSID_WORKERD_REF required');
    for (const key of ['--host', '--worker', '--mode']) assert.ok(!args.has(key), 'no product alias for reference');
}
const host = args.has('--host') ? path.resolve(args.get('--host')) : undefined;
const worker = args.has('--worker') ? path.resolve(args.get('--worker')) : undefined;
const mode = reference ? 'reference' : args.get('--mode');
assert.ok(reference || host && worker && ['single-worker', 'static-pool', 'managed'].includes(mode));
const shard = args.get('--shard');
const cases = ordinaryFetchCasesForShard(shard);
const lock = readLock(); verifyClient(lock); if (reference) verifyBinary(reference, lock);
const fixture = path.join(root, ordinaryFetchInputPaths.fixture);
const inputs = Object.fromEntries(Object.entries(ordinaryFetchInputPaths).map(([key, name]) => [key, fileDigest(path.join(root, name))]));
const binaries = {host: fileDigest(reference ?? host), worker: reference ? null : fileDigest(worker)};
const runId = process.env.CAPSID_WS_RUN_ID ?? randomUUID();
const report = {schema: 'capsid-ordinary-fetch-v1', complete: false, shard, mode, runId, referenceLock: lock,
    client: {node: process.version, undici: process.versions.undici}, inputs, binaries, cases: []};
const destination = path.resolve(args.get('--report') ?? (process.env.CAPSID_WS_REPORT_DIR
    ? path.join(process.env.CAPSID_WS_REPORT_DIR, 'ordinary-fetch-' + shard + '-' + mode + '.json')
    : path.join(os.tmpdir(), 'capsid-ordinary-fetch-' + shard + '-' + runId + '-' + mode + '.json')));
assert.ok(!fs.existsSync(destination), 'preserve prior ordinary fetch evidence');
function save() {fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');}
save();
async function observeUntil(predicate, milliseconds = 3000) {
    const deadline = performance.now() + milliseconds;
    while (!predicate() && performance.now() < deadline) await delay(5);
    return predicate();
}

async function startFiniteUpstream(kind, challenge, role) {
    const sockets=new Map(),records=[],body=Buffer.from(Array.from({length:ordinaryFetchNativePlans[kind]?.bytes ?? 97},(_,index)=>(index*29+17)%251));
    const listener=net.createServer({allowHalfOpen:true},socket=>{
        const item={request:null,rawRequest:'',bytes:0,peerEnded:false,peerReset:false,fixtureEnded:false,fixtureDestroyed:false,events:[],
            localAddress:socket.localAddress,localPort:socket.localPort,sent:null};records.push(item);sockets.set(socket,item);
        let input=Buffer.alloc(0);
        socket.on('error',error=>{if(error.code==='ECONNRESET')item.peerReset=true;else item.error=error.code;});
        socket.on('end',()=>{item.peerEnded=true;item.events.push('peer-ended');if(item.held){item.fixtureEnded=true;item.events.push('server-half-close');socket.end();}});socket.on('close',()=>sockets.delete(socket));
        socket.on('data',chunk=>{
            input=Buffer.concat([input,chunk]);item.bytes+=chunk.length;item.rawRequest=input.toString('base64');if(item.request!==null)return;
            const end=input.indexOf('\r\n\r\n');if(end<0)return;item.request=input.subarray(0,end).toString();item.events.push('request');
            const landing=item.request.split('\r\n')[0].includes(' /landing?');
            let status=kind==='ordinary-meta-204'?204:201,phrase=status===204?'No Content':'Created';
            const empty=['ordinary-meta-empty','ordinary-meta-204'].includes(kind);
            let payload=empty?Buffer.alloc(0):body,headers=['Connection: close','Content-Type: application/octet-stream','X-Probe: '+challenge];
            if(kind.includes('redirect-')&&!landing){status=302;phrase='Found';payload=Buffer.alloc(0);headers.push('Location: /landing?challenge='+challenge);}
            if(kind==='ordinary-meta-duplicate-headers')headers.push('X-List: first','X-List: second','Set-Cookie: a=1','Set-Cookie: b=2');
            if (ordinaryFetchNativePlans[kind]?.padding) headers.push('X-Padding: '+'p'.repeat(ordinaryFetchNativePlans[kind].padding));
            const chunked=kind==='ordinary-meta-chunked';
            const held=kind.startsWith('ordinary-held-');headers.push(chunked?'Transfer-Encoding: chunked':'Content-Length: '+(held?1048673:payload.length));
            const head=Buffer.from('HTTP/1.1 '+status+' '+phrase+'\r\n'+headers.join('\r\n')+'\r\n\r\n');
            let wire=payload;
            if(kind==='ordinary-meta-head'||kind==='ordinary-clone-null-head'||status===204)wire=Buffer.alloc(0);
            else if(chunked)wire=Buffer.concat([Buffer.from('11\r\n'),payload.subarray(0,17),Buffer.from('\r\n50\r\n'),payload.subarray(17),Buffer.from('\r\n0\r\n\r\n')]);
            const packet=Buffer.concat([head,held?wire.subarray(0,17):wire]);item.sent=packet.toString('base64');item.held=held;if(held){item.declaredBodyBytes=1048673;item.sentBodyBytes=17;item.events.push('held-body');socket.write(packet);}else{item.fixtureEnded=true;item.events.push('sent-complete');socket.end(packet);}
        });
    });
    await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
    return {port:listener.address().port,records,snapshot(){return structuredClone(records);},async stop(){
        for(const [socket,item] of sockets){item.fixtureDestroyed=true;socket.destroy();}
        await new Promise(resolve=>listener.close(resolve));
    }};
}


async function startGatedUpstream(kind,challenge,role){
    const plan=ordinaryFetchGatedPlans[kind],cancel=kind.endsWith('-cancel'),body=Buffer.from(Array.from({length:plan.reduce((a,b)=>a+b,0)},(_,i)=>(i*29+17)%251));
    assert.ok(Array.isArray(plan));const sockets=new Map(),records=[];let active;
    const listener=net.createServer({allowHalfOpen:true},socket=>{
        assert.ok(!active,'one actual ordinary HTTP connection');let input=Buffer.alloc(0),offset=0;
        const item={request:null,rawRequest:'',bytes:0,peerEnded:false,peerReset:false,fixtureEnded:false,fixtureDestroyed:false,events:[],
            localAddress:socket.localAddress,localPort:socket.localPort,sent:null,bodyPackets:[],progress:[],declaredBodyBytes:cancel?1048673:body.length};
        records.push(item);sockets.set(socket,item);active={socket,item,writeNext(){
            const index=item.bodyPackets.length;
            assert.ok(index<=plan.length&&!item.fixtureEnded&&!item.peerEnded,'source gate released only while live');
            if(index<plan.length){
                const payload=body.subarray(offset,offset+plan[index]);offset+=payload.length;
                item.bodyPackets.push(payload.toString('base64'));item.events.push('body/'+index+'/'+payload.length);
                socket.write(payload);
                if(kind.includes('-byob-')&&index===plan.length-1&&!cancel){item.fixtureEnded=true;item.events.push('server-complete');socket.end();}
            }else{assert.ok(!cancel,'cancel source never completed by fixture');item.fixtureEnded=true;item.events.push('server-complete');socket.end();}
        }};
        socket.on('error',error=>{if(error.code==='ECONNRESET')item.peerReset=true;else item.error=error.code;});
        socket.on('end',()=>{item.peerEnded=true;item.events.push('peer-ended');if(!item.fixtureEnded){item.fixtureEnded=true;item.events.push('server-half-close');socket.end();}});
        socket.on('close',()=>sockets.delete(socket));
        socket.on('data',chunk=>{
            input=Buffer.concat([input,chunk]);item.bytes+=chunk.length;item.rawRequest=input.toString('base64');if(item.request!==null)return;
            const end=input.indexOf('\r\n\r\n');if(end<0)return;item.request=input.subarray(0,end).toString();item.events.push('request');
            const head=Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\nX-Probe: '+challenge+'\r\nContent-Length: '+item.declaredBodyBytes+'\r\n\r\n');
            item.sent=head.toString('base64');item.events.push('headers');socket.write(head);if(plan.length===0){item.fixtureEnded=true;item.events.push('server-complete');socket.end();}
        });
    });
    await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
    return {port:listener.address().port,records,snapshot(){return structuredClone(records);},headerRead(){
        assert.ok(active&&active.item.request!==null);active.item.events.push('headers-observed');if(plan.length!==0)active.writeNext();
    },read(index,length){
        const {item}=active;assert.equal(index,item.progress.length);assert.equal(length,plan[index]);
        item.progress.push({index,length});item.events.push('read/'+index+'/'+length);
        if(!cancel&&!item.fixtureEnded)active.writeNext();
    },async stop(){for(const [socket,item]of sockets){item.fixtureDestroyed=true;socket.destroy();}await new Promise(resolve=>listener.close(resolve));}};
}


async function startReference(directory) {
    const listener = net.createServer(); await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
    const port = listener.address().port; await new Promise(resolve => listener.close(resolve));
    fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
    const config = path.join(directory, 'config.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child = spawn(reference, ['serve', config], {stdio: ['ignore', 'pipe', 'pipe']});
    let stderr = ''; child.stderr.on('data', data => stderr += data); child.stdout.resume();
    const closed = new Promise(resolve => child.once('close', resolve)), backend = {child, closed, port, stderr: () => stderr};
    try {
        const deadline = performance.now() + 10000;
        for (;;) try {
            const response = await fetch(`http://127.0.0.1:${port}/state`, {signal: AbortSignal.timeout(500)});
            assert.equal(response.status, 200); await response.text(); return backend;
        } catch (error) {
            assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null, 'ordinary fetch reference startup: ' + error + stderr);
            await delay(20);
        }
    } catch (error) {await stopReference(backend); throw error;}
}
async function stopReference(backend) {
    let forced = false, timer;
    if (backend.child.exitCode === null && backend.child.signalCode === null) backend.child.kill('SIGTERM');
    try {await Promise.race([backend.closed, new Promise(resolve => {timer = setTimeout(() => {
        forced = true; backend.child.kill('SIGKILL'); resolve();
    }, 2000);})]); await backend.closed;} finally {clearTimeout(timer);}
    return {forced, code: backend.child.exitCode, signal: backend.child.signalCode};
}


for (const id of cases) {
    const value = {id, challenge: randomUUID()}; report.cases.push(value); save();
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ordinary-fetch-'));
    let backend, peer, source, readState;
    try {
        const gated = id.startsWith('ordinary-gated-');
        source = await (gated ? startGatedUpstream : startFiniteUpstream)(id, value.challenge, 'a');
        value.targets = [{host: 'localhost', port: source.port}];
        const allow = ['localhost:' + source.port];
        backend = reference ? await startReference(directory) : await startCapsid(host, worker, fixture, directory, mode,
            {permissions: {modules: [], fetch: {allow}}, fetchTargets: allow});
        const prefix = reference ? '' : '/@capsid/orders';
        readState = async () => {
            const response = await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`, {signal: AbortSignal.timeout(4000)});
            return {status: response.status, text: await response.text()};
        };
        value.initial = await readState();
        const target = `http://localhost:${source.port}/hold?challenge=${value.challenge}&role=a`;
        const query = new URLSearchParams({kind: id, challenge: value.challenge, role: 'a', target});
        peer = responseSession(backend.port, prefix + '/probe?' + query, {observeCloseReply: true, timeoutMs: 12000});
        value.upgradea = await peer.headers; assert.equal(value.upgradea.status, 101);
        peer.send('go'); assert.equal(await peer.text(), 'started');
        assert.ok(await observeUntil(() => source.records.length > 0 && source.records.every(entry => entry.request !== null)), 'actual ordinary HTTP source requests complete before snapshot');
        value.beforeAction = source.snapshot();
        if (gated) {
            assert.equal(await peer.text(), 'headers'); source.headerRead();
            for (const [index, length] of ordinaryFetchGatedPlans[id].entries()) {
                assert.equal(await peer.text(), 'read/' + index + '/' + length); source.read(index, length);
            }
        }
        value.wirea = await peer.complete;
        assert.ok(await observeUntil(() => source.records.length > 0 && source.records.every(entry => entry.peerEnded)),
            'source observes actual client EOF before host shutdown or fixture disposal');
        value.observed = await readState(); await delay(100); value.finalState = await readState();
        value.upstream = source.snapshot();
    } catch (error) {
        value.failure = error.stack;
        if (error.observation) value.failureObservation = error.observation;
        if (readState) try {value.failureState = await readState();} catch (nested) {value.failureStateError = nested.stack;}
    } finally {
        peer?.dispose();
        if (backend) {
            try {value.shutdown = reference ? await stopReference(backend) : await stopCapsid(backend.child);}
            catch (error) {value.cleanupFailure = error.stack;}
            value.stderr = backend.stderr();
        }
        if (source) await source.stop();
        save();
    }
}
for (const [key, name] of Object.entries(ordinaryFetchInputPaths))
    assert.equal(fileDigest(path.join(root, name)), inputs[key], 'unchanged formal ordinary-fetch input');
assert.equal(fileDigest(reference ?? host), binaries.host);
if (!reference) assert.equal(fileDigest(worker), binaries.worker);
report.complete = report.cases.every(value => !value.failure && !value.cleanupFailure && value.shutdown?.forced === false);
save();
validateOrdinaryFetchReport(report, {lock, inputs, runId, host: binaries.host, worker: binaries.worker, shard});
console.log(JSON.stringify({complete: true, mode, shard, cases: cases.length, report: destination}));
