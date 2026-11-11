import {createHeldOwnerSource} from '../tools/native-fetch-held-owner-peer.mjs';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const name = process.argv[index], value = process.argv[index+1];
    assert.ok(['--helper', '--worker', '--kind'].includes(name) && value && !args.has(name));
    args.set(name, value);
}
assert.equal(args.size, 3, 'actual C ABI helper, worker and scenario group');
const kind=args.get('--kind');assert.ok(['finished','held','owner'].includes(kind));
const group=kind==='finished'?'codec-branch':kind==='held'?'held-branch':'held-owner';
const input=await import('../tools/native-fetch-'+group+'-input.mjs');
const contract=await import('../tools/native-fetch-'+group+'-contract.mjs');
const nativeFetchGzipMemberInputPaths=input.heldCodecBranchInputPaths??input.codecBranchInputPaths;
const nativeFetchGzipMemberPlans=input.heldCodecBranchBasePlans??input.codecBranchBasePlans;
const nativeFetchGzipMemberReply=input.heldCodecBranchReply??input.codecBranchReply;
const helper = args.get('--helper'), worker = args.get('--worker');
const plans=contract.plans;
const fixture = fileURLToPath(new URL(kind==='owner'?'./fixtures/native-fetch-error-owner.mjs':'./fixtures/native-fetch-encoded-branches.mjs', import.meta.url));
const expected = {runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-native-encoded-'+kind,
    inputs: ({...Object.fromEntries(Object.entries(nativeFetchGzipMemberInputPaths).map(([key, name]) => [key, hash(path.join(root, name))])),branchFixture:hash(fixture),branchCollector:hash(fileURLToPath(import.meta.url))}),
    binaries: {helper: hash(helper), worker: hash(worker)}};
const report = {schema:'capsid-native-fetch-encoded-branches-v1',kind,complete:false,runId:expected.runId,inputs:expected.inputs,binaries:expected.binaries,cases:[]};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    destination = path.join(process.env.CAPSID_WS_REPORT_DIR, 'native-fetch-encoded-'+kind+'.json');
    assert.equal(fs.existsSync(destination), false, 'fresh observations only');
}
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2)+'\n');};
save();
for (const plan of plans) {
    const row = {plan}; report.cases.push(row);
    let record;
    const reply=nativeFetchGzipMemberReply(nativeFetchGzipMemberPlans.find(item=>item.id===plan.baseId));
    const ordinarySource=()=>{
        const sockets=new Set();
        const server=net.createServer({allowHalfOpen:true},socket=>{
            sockets.add(socket);
            record={request:null,rawRequest:'',peerEnded:false,peerReset:false,fixtureEnded:false,fixtureDestroyed:false,events:[],sent:reply.packet.toString('base64')};
            row.source=record;
            socket.on('end',()=>{record.peerEnded=true;record.events.push('peer-ended');});
            socket.on('close',()=>sockets.delete(socket));
            socket.on('error',error=>{if(error.code==='ECONNRESET')record.peerReset=true;else record.error=error.code;});
            let request=Buffer.alloc(0);
            socket.on('data',data=>{
                request=Buffer.concat([request,data]);record.rawRequest=request.toString('base64');
                if(record.request!==null)return;
                const end=request.indexOf('\r\n\r\n');if(end<0)return;
                record.request=request.subarray(0,end).toString();record.events.push('request');
                if(kind==='finished'){record.fixtureEnded=true;record.events.push('sent-complete');socket.end(reply.packet);}
                else{record.events.push('sent-held-http-chunk');socket.write(reply.packet);}
            });
        });
        return {server,sockets};
    };
    const {server,sockets}=kind==='owner'?createHeldOwnerSource(row,reply,value=>{record=value;}):ordinarySource();
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);});
    row.port = server.address().port; row.encodedBytes = reply.payload.length;
    row.payloadSHA256 = createHash('sha256').update(reply.payload).digest('hex');
    row.packetSHA256 = createHash('sha256').update(reply.packet).digest('hex');
    const child = spawn(helper, [worker, fixture, 'http://localhost:'+row.port+'/hold', '0', plan.view+'|'+plan.style], {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', resolveState;
    const stateReady = new Promise(resolve => {resolveState = resolve;});
    child.stdout.on('data', data => {
        stdout += data;
        const line = stdout.split(/\r?\n/).find(line => line.startsWith('LIMIT_PROBE '));
        if (line) try {row.state = JSON.parse(line.slice(12)); resolveState();} catch {}
    });
    child.stderr.on('data', data => {stderr += data;});
    const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
    const deadline = AbortSignal.timeout(12000);
    const bounded = promise => Promise.race([promise, new Promise((_, reject) => {
        if (deadline.aborted) reject(deadline.reason);
        else deadline.addEventListener('abort', () => reject(deadline.reason), {once: true});
    })]);
    let failure, acknowledged = false;
    try {
        await bounded(Promise.race([stateReady, closed.then(() => {assert.ok(row.state, 'helper produced actual state');})]));
        const end = performance.now()+3000;
        while (!record?.peerEnded && performance.now() < end) await delay(5);
        row.sourceEndedBeforeFinish = record?.peerEnded === true;
        row.sourceBeforeFinish = record ? structuredClone(record) : null;
        child.stdin.end('finish\n'); acknowledged = true;
        row.shutdown = await bounded(closed);
        row.stdout = stdout; row.stderr = stderr;
        const exit = stdout.split(/\r?\n/).filter(line => line.startsWith('LIMIT_EXIT '));
        assert.equal(exit.length, 1, 'one actual OS exit observation');
        row.workerExit = JSON.parse(exit[0].slice(11));
    } catch (error) {
        failure = error; row.collectionFailure = String(error.stack ?? error);
        if (!acknowledged) child.stdin.end('finish\n');
        if (child.exitCode === null && child.signalCode === null) child.kill();
        await Promise.race([closed.catch(() => {}), delay(2000)]);
        row.stdout = stdout; row.stderr = stderr;
    } finally {
        for (const socket of sockets) {record.fixtureDestroyed = true; socket.destroy();}
        await new Promise(resolve => server.close(resolve));
        save();
        console.log(JSON.stringify({case:plan.id,collected:!failure}));
    }
    if (failure) throw failure;
    assert.deepEqual(row.shutdown,{code:0,signal:null});
    contract.assertProductRow(row,plan);
}
assert.equal(hash(helper), expected.binaries.helper);
assert.equal(hash(worker), expected.binaries.worker);
assert.deepEqual(({...Object.fromEntries(Object.entries(nativeFetchGzipMemberInputPaths).map(([key, name]) => [key, hash(path.join(root, name))])),branchFixture:hash(fixture),branchCollector:hash(fileURLToPath(import.meta.url))}), expected.inputs);
report.complete = true; save();

console.log(JSON.stringify({complete: true, cases: plans.length, actualHeldSource: true, actualCleanExit: true}));
